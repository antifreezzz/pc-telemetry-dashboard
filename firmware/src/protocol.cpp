#include "protocol.h"
#include "ui.h"
#include "ble_lamp.h"

volatile uint32_t g_epoch_sec = 0;
volatile uint32_t g_uptime_sec = 0;
char g_hostname[24] = {0};

enum State {
    ST_SYNC,
    ST_LEN_LO,
    ST_LEN_HI,
    ST_TYPE,
    ST_PAYLOAD,
    ST_CSUM
};

static State st = ST_SYNC;
static uint8_t pkt_type = 0;
static uint16_t pkt_len = 0;
static uint16_t pkt_pos = 0;
static uint8_t pkt_buf[PROTO_MAX_PKT];
static uint8_t pkt_crc = 0;
static uint32_t pkt_count = 0;
static uint32_t pkt_bad = 0;
#ifdef PROTO_DEBUG
static uint32_t last_debug_ms = 0;
#endif

static uint8_t crc8_step(uint8_t crc, uint8_t b)
{
    crc ^= b;
    for (int i = 0; i < 8; i++) {
        if (crc & 0x80)
            crc = (uint8_t)((crc << 1) ^ 0x07);
        else
            crc = (uint8_t)(crc << 1);
    }
    return crc;
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Parse the TLV payload; returns false on TLV overrun (caller resyncs).
static bool handle_payload(const uint8_t *p, uint16_t len)
{
    uint16_t pos = 0;
    while (pos + 2 <= len) {
        uint8_t id = p[pos];
        uint8_t flen = p[pos + 1];
        pos += 2;
        if (pos + flen > len)
            return false;
        switch (id) {
        case FIELD_CPU: {
            uint8_t pct = p[pos];
            uint8_t ncores = (uint8_t)(flen - 1);
            ui_set_cpu_pct(pct, &p[pos + 1], ncores);
            break;
        }
        case FIELD_RAM:
            ui_set_ram_pct(p[pos], rd_u32(&p[pos + 1]), rd_u32(&p[pos + 5]));
            break;
        case FIELD_GPU:
            ui_set_gpu_pct(p[pos], p[pos + 1],
                           rd_u32(&p[pos + 2]), rd_u32(&p[pos + 6]));
            break;
        case FIELD_NET:
            ui_set_net(rd_u32(&p[pos]), rd_u32(&p[pos + 4]));
            break;
        case FIELD_DISK: {
            uint8_t d1 = p[pos + 8];
            uint8_t d2 = (flen >= 10) ? p[pos + 9] : 255;
            ui_set_disk(rd_u32(&p[pos]), rd_u32(&p[pos + 4]), d1, d2);
            break;
        }
        case FIELD_HEADER: {
            uint32_t uptime = rd_u32(&p[pos]);
            uint32_t epoch = rd_u32(&p[pos + 4]);
            memcpy(g_hostname, &p[pos + 8], 24);
            g_hostname[23] = '\0';
            g_uptime_sec = uptime;
            g_epoch_sec = epoch;
            ui_set_header(uptime, epoch, g_hostname);
            break;
        }
        case FIELD_PROC:
            ui_set_proc(&p[pos], flen);
            break;
        case FIELD_LLM:
            if (flen >= 3) {
                uint8_t status = p[pos];
                uint16_t tps_x10 = (uint16_t)p[pos + 1] | ((uint16_t)p[pos + 2] << 8);
                uint8_t cache_hit = 255;
                uint16_t prompt_k = 0;
                uint8_t flags = 0;
                char model_buf[25] = {0};

                if (flen >= 7) {
                    cache_hit = p[pos + 3];
                    prompt_k = (uint16_t)p[pos + 4] | ((uint16_t)p[pos + 5] << 8);
                    flags = p[pos + 6];
                }
                if (flen > 7) {
                    uint8_t mlen = flen - 7;
                    if (mlen > 24) mlen = 24;
                    memcpy(model_buf, &p[pos + 7], mlen);
                    model_buf[mlen] = '\0';
                } else if (flen > 3 && flen < 7) {
                    uint8_t mlen = flen - 3;
                    if (mlen > 24) mlen = 24;
                    memcpy(model_buf, &p[pos + 3], mlen);
                    model_buf[mlen] = '\0';
                }
                ui_set_llm(status, tps_x10 / 10.0f, model_buf, cache_hit, prompt_k, flags);
            }
            break;
        case FIELD_LLM_MODELS:
            ui_set_llm_models(&p[pos], flen);
            break;
        case FIELD_LLM_PROFILES:
            ui_set_llm_profiles(&p[pos], flen);
            break;
        case FIELD_SET_SCREEN:
            if (flen >= 1) {
                ui_open_screen(p[pos]);
            }
            break;
        default:
            // Forward compatibility: skip unknown TLVs
            break;
        }
        pos += flen;
    }
    return true;
}

void protocol_init()
{
    Serial.begin(115200);
    st = ST_SYNC;
    pkt_pos = 0;
    pkt_len = 0;
    pkt_count = 0;
    pkt_bad = 0;
}

void protocol_poll()
{
    while (Serial.available() > 0) {
        uint8_t b = (uint8_t)Serial.read();
        switch (st) {
        case ST_SYNC:
            if (b == PROTO_SYNC) {
                st = ST_LEN_LO;
                pkt_crc = 0;
            }
            break;
        case ST_LEN_LO:
            pkt_len = b;
            st = ST_LEN_HI;
            break;
        case ST_LEN_HI:
            pkt_len |= ((uint16_t)b << 8);
            if (pkt_len > PROTO_MAX_PKT) {
                pkt_bad++;
                st = ST_SYNC;
            } else {
                st = ST_TYPE;
            }
            break;
        case ST_TYPE:
            pkt_type = b;
            pkt_crc = crc8_step(0, b);
            pkt_pos = 0;
            if (pkt_len == 0)
                st = ST_CSUM;
            else
                st = ST_PAYLOAD;
            break;
        case ST_PAYLOAD:
            pkt_buf[pkt_pos++] = b;
            pkt_crc = crc8_step(pkt_crc, b);
            if (pkt_pos >= pkt_len)
                st = ST_CSUM;
            break;
        case ST_CSUM:
            if (b == pkt_crc && pkt_type == PROTO_TYPE) {
                pkt_count++;
                if (!handle_payload(pkt_buf, pkt_len))
                    pkt_bad++;
            } else {
                pkt_bad++;
            }
            st = ST_SYNC;
            break;
        }
    }

#ifdef PROTO_DEBUG
    uint32_t now = millis();
    if (now - last_debug_ms >= 5000) {
        last_debug_ms = now;
        Serial.printf("[proto] ok=%u bad=%u\n", pkt_count, pkt_bad);
    }
#endif
}

void protocol_send_cmd(uint8_t cmd_id)
{
    if (cmd_id == CMD_STOP_ALL) {
        Serial.println("cmd:STOP_ALL");
    } else if (cmd_id == CMD_START_FAVORITE) {
        Serial.println("cmd:START_FAVORITE");
    }
}

void protocol_send_start_model(const char *model_id)
{
    if (!model_id) return;
    Serial.printf("cmd:START_MODEL:%s\n", model_id);
}

void protocol_send_start_model_profile(const char *model_id, const char *profile)
{
    if (!model_id || !profile) return;
    Serial.printf("cmd:START_MODEL:%s:%s\n", model_id, profile);
}

void protocol_send_get_profiles(const char *model_id)
{
    if (!model_id) return;
    Serial.printf("cmd:GET_PROFILES:%s\n", model_id);
}
