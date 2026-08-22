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
)

logger = logging.getLogger(__name__)


def _status_str_to_code(status_str: str, has_active_model: bool = False) -> int:
    status_lower = (status_str or "").strip().lower()
    if status_lower == "running":
        return LLM_STATUS_RUNNING
    elif status_lower in ("starting", "loading"):
        return LLM_STATUS_STARTING
    elif status_lower in ("idle", "stopped"):
        return LLM_STATUS_IDLE
    elif has_active_model:
        return LLM_STATUS_RUNNING
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
        self._models: List[Dict[str, Any]] = []
        self._last_models_fetch = 0.0
        self._running = True
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
                    code = _status_str_to_code(raw_status, bool(active_model))
                    tps = float(status_dict.get("tps") or 0.0)
                    with self._lock:
                        self._status = code
                        self._tps = max(0.0, tps)
                        self._active_model = active_model
                        if models_list is not None:
                            self._models = models_list
                else:
                    with self._lock:
                        self._status = LLM_STATUS_OFFLINE
                        self._tps = 0.0
                        self._active_model = ""
                        if models_list is not None:
                            self._models = models_list
            except Exception as e:
                logger.debug("LLMMonitor worker loop error: %s", e)
                with self._lock:
                    self._status = LLM_STATUS_OFFLINE
                    self._tps = 0.0
                    self._active_model = ""

            # Sleep in small increments for responsive shutdown
            slept = 0.0
            while slept < self.poll_interval and self._running:
                time.sleep(0.05)
                slept += 0.05

    def snapshot(self) -> Tuple[int, float, str]:
        """Return (status_code, tps, active_model). Non-blocking and thread-safe."""
        with self._lock:
            return self._status, self._tps, self._active_model

    def models_snapshot(self) -> List[Dict[str, Any]]:
        """Return list of models. Non-blocking and thread-safe."""
        with self._lock:
            return list(self._models)

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
