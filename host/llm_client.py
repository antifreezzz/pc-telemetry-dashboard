#!/usr/bin/env python3
"""LLMControl REST API client and background status monitor."""

import json
import logging
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Dict, List, Optional, Tuple

from host.protocol import (
    LLM_STATUS_IDLE,
    LLM_STATUS_OFFLINE,
    LLM_STATUS_RUNNING,
    LLM_STATUS_STARTING,
    LLM_STATUS_PROMPT_EVAL,
    LLM_STATUS_GENERATING,
)

logger = logging.getLogger(__name__)


def _status_str_to_code(status_str: str, has_active_model: bool = False, phase: str = "") -> int:
    status_lower = (status_str or "").strip().lower()
    if status_lower == "running" or has_active_model:
        if phase == "prompt_eval":
            return LLM_STATUS_PROMPT_EVAL
        elif phase == "generating":
            return LLM_STATUS_GENERATING
        return LLM_STATUS_RUNNING
    elif status_lower in ("starting", "loading"):
        return LLM_STATUS_STARTING
    elif status_lower in ("idle", "stopped"):
        return LLM_STATUS_IDLE
    return LLM_STATUS_IDLE


class LLMControlClient:
    """HTTP REST client for llmcontrol service."""

    def __init__(self, base_url: str = "http://127.0.0.1:8666", timeout: float = 2.0):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout

    def _request(
        self,
        method: str,
        path: str,
        data: Optional[Dict[str, Any]] = None,
    ) -> Optional[Any]:
        url = f"{self.base_url}{path}"
        headers = {"Accept": "application/json"}
        req_data = None
        if data is not None:
            headers["Content-Type"] = "application/json"
            req_data = json.dumps(data).encode("utf-8")

        req = urllib.request.Request(
            url,
            data=req_data,
            headers=headers,
            method=method,
        )
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                raw = resp.read()
                if not raw:
                    return {}
                return json.loads(raw.decode("utf-8"))
        except (urllib.error.URLError, TimeoutError, OSError, json.JSONDecodeError) as e:
            logger.debug("llmcontrol request error %s %s: %s", method, url, e)
            return None

    def get_status(self) -> Optional[Dict[str, Any]]:
        """GET /api/status -> dict or None if offline."""
        res = self._request("GET", "/api/status")
        if isinstance(res, dict):
            return res
        return None

    def get_models(self) -> List[Dict[str, Any]]:
        """GET /api/models -> list of model objects."""
        res = self._request("GET", "/api/models")
        if isinstance(res, list):
            return res
        return []

    def stop_all(self) -> bool:
        """POST /api/models/stop-all -> bool."""
        res = self._request("POST", "/api/models/stop-all", data={})
        return res is not None

    def start_model(self, model_id: str, profile: str = "default") -> bool:
        """POST /api/models/{id}/start -> bool."""
        quoted_id = urllib.parse.quote(model_id, safe="")
        res = self._request(
            "POST",
            f"/api/models/{quoted_id}/start",
            data={"profile": profile or "default"},
        )
        return res is not None

    def start_favorite(self, fallback_profile: str = "default") -> bool:
        """Start the favorite model (or first available model if none marked favorite)."""
        models = self.get_models()
        if not models:
            return False

        target = None
        for m in models:
            if m.get("is_favorite"):
                target = m
                break
        if target is None:
            target = models[0]

        model_id = target.get("id") or target.get("name")
        if not model_id:
            return False

        profile = target.get("default_profile") or fallback_profile or "default"
        return self.start_model(model_id, profile=profile)


class LLMMonitor:
    """Non-blocking background monitor and controller for llmcontrol."""

    def __init__(
        self,
        base_url: str = "http://127.0.0.1:8666",
        poll_interval: float = 1.0,
        timeout: float = 2.0,
        client: Optional[LLMControlClient] = None,
    ):
        self.client = client or LLMControlClient(base_url=base_url, timeout=timeout)
        self.poll_interval = poll_interval
        self._lock = threading.Lock()
        self._status = LLM_STATUS_OFFLINE
        self._tps = 0.0
        self._active_model = ""
        self._cache_hit_pct = 255
        self._prompt_tokens = 0
        self._has_alert = False
        self._models: List[Dict[str, Any]] = []
        self._last_models_fetch = time.time()
        self._running = True
        try:
            init_models = self.client.get_models()
            if init_models:
                self._models = init_models
            init_status = self.client.get_status()
            if init_status:
                raw_status = init_status.get("status", "idle")
                active_model = init_status.get("active_model") or ""
                self._active_model = active_model
                self._status = _status_str_to_code(raw_status, bool(active_model))
        except Exception:
            pass

        self._thread = threading.Thread(target=self._worker, daemon=True, name="LLMMonitor")
        self._thread.start()

    def _worker(self) -> None:
        while self._running:
            try:
                status_dict = self.client.get_status()
                now = time.time()
                models_list = None
                if now - self._last_models_fetch > 2.0:
                    models_list = self.client.get_models()
                    self._last_models_fetch = now

                if status_dict is not None:
                    raw_status = status_dict.get("status", "idle")
                    active_model = status_dict.get("active_model") or ""
                    llm_metrics = status_dict.get("llm_metrics") or {}
                    phase = llm_metrics.get("phase") or ""
                    code = _status_str_to_code(raw_status, bool(active_model), phase=phase)
                    tps = float(status_dict.get("tps") or 0.0)
                    cache_hit_pct = int(round(float(llm_metrics.get("cache_hit_pct", 255)))) if "cache_hit_pct" in llm_metrics else 255
                    prompt_tokens = int(llm_metrics.get("prompt_tokens") or 0)
                    has_alert = bool(llm_metrics.get("has_alert"))

                    with self._lock:
                        self._status = code
                        self._tps = max(0.0, tps)
                        self._active_model = active_model
                        self._cache_hit_pct = cache_hit_pct
                        self._prompt_tokens = prompt_tokens
                        self._has_alert = has_alert
                        if models_list is not None:
                            self._models = models_list
                else:
                    with self._lock:
                        self._status = LLM_STATUS_OFFLINE
                        self._tps = 0.0
                        self._active_model = ""
                        self._cache_hit_pct = 255
                        self._prompt_tokens = 0
                        self._has_alert = False
                        if models_list is not None:
                            self._models = models_list
            except Exception as e:
                logger.debug("LLMMonitor worker loop error: %s", e)
                with self._lock:
                    self._status = LLM_STATUS_OFFLINE
                    self._tps = 0.0
                    self._active_model = ""
                    self._cache_hit_pct = 255
                    self._prompt_tokens = 0
                    self._has_alert = False

            # Sleep in small increments for responsive shutdown
            slept = 0.0
            while slept < self.poll_interval and self._running:
                time.sleep(0.05)
                slept += 0.05

    def snapshot(self) -> Tuple[int, float, str, int, int, bool]:
        """Return (status_code, tps, active_model, cache_hit_pct, prompt_tokens, has_alert). Non-blocking and thread-safe."""
        with self._lock:
            return (
                self._status,
                self._tps,
                self._active_model,
                self._cache_hit_pct,
                self._prompt_tokens,
                self._has_alert,
            )

    def models_snapshot(self) -> List[Dict[str, Any]]:
        """Return list of models. Non-blocking and thread-safe."""
        with self._lock:
            return list(self._models)

    def get_model_profiles(self, model_id: str) -> List[Dict[str, Any]]:
        """Return profiles for model_id."""
        with self._lock:
            models = list(self._models)
        for m in models:
            if m.get("id") == model_id or m.get("name") == model_id:
                return list(m.get("profiles") or [])
        # Fallback fetch if not in cached list yet
        try:
            fresh_models = self.client.get_models()
            if fresh_models:
                with self._lock:
                    self._models = fresh_models
                for m in fresh_models:
                    if m.get("id") == model_id or m.get("name") == model_id:
                        return list(m.get("profiles") or [])
        except Exception:
            pass
        return []

    def stop_all(self) -> bool:
        """Issue stop-all command via client, update local snapshot immediately."""
        ok = self.client.stop_all()
        if ok:
            with self._lock:
                self._status = LLM_STATUS_IDLE
                self._tps = 0.0
                self._active_model = ""
        return ok

    def start_favorite(self, profile: str = "default") -> bool:
        """Issue start favorite command via client."""
        return self.client.start_favorite(fallback_profile=profile)

    def start_model(self, model_id: str, profile: str = "default") -> bool:
        """Issue start model command via client."""
        return self.client.start_model(model_id=model_id, profile=profile)

    def stop(self) -> None:
        """Stop background worker thread."""
        self._running = False
        if self._thread.is_alive():
            self._thread.join(timeout=1.0)
