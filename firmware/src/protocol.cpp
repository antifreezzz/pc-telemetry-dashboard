#include "protocol.h"
#include "ui.h"

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
        case FIELD_DISK:
            ui_set_disk(rd_u32(&p[pos]), rd_u32(&p[pos + 4]), p[pos + 8]);
            break;
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
                char model_buf[25] = {0};
                uint8_t str_len = (flen > 3) ? (flen - 3) : 0;
                if (str_len > 24) str_len = 24;
                memcpy(model_buf, &p[pos + 3], str_len);
                model_buf[str_len] = '\0';
                ui_set_llm(status, tps_x10 / 10.0f, model_buf);
            }
            break;
        case FIELD_LLM_MODELS:
            ui_set_llm_models(&p[pos], flen);
            break;
        default:
            break;  // unknown field id: skip via field_len
        }
        pos += flen;
    }
    return true;
}

void protocol_init()
{
    Serial.begin(115200);
    st = ST_SYNC;
}

void protocol_poll()
{
#ifdef PROTO_DEBUG
    uint32_t now = millis();
    if (now - last_debug_ms > 1000)
    {
        Serial.print("s:");
        Serial.print(pkt_count);
        Serial.print(" r:");
        Serial.print(pkt_bad);
        Serial.print(" a:");
        Serial.print(Serial.available());
        Serial.println();
        Serial.flush();
        last_debug_ms = now;
    }
#endif
    while (Serial.available())
    {
        uint8_t b = Serial.read();
        switch (st)
        {
        case ST_SYNC:
            if (b == PROTO_SYNC) st = ST_LEN_LO;
            break;
        case ST_LEN_LO:
            pkt_len = b;
            st = ST_LEN_HI;
            break;
        case ST_LEN_HI:
            pkt_len |= ((uint16_t)b) << 8;
            if (pkt_len > PROTO_MAX_PKT) {
                st = ST_SYNC;
                break;
            }
            st = ST_TYPE;
            break;
        case ST_TYPE:
            pkt_type = b;
            pkt_crc = crc8_step(0, pkt_type);
            pkt_pos = 0;
            st = (pkt_len == 0) ? ST_CSUM : ST_PAYLOAD;
            break;
        case ST_PAYLOAD:
            pkt_buf[pkt_pos++] = b;
            pkt_crc = crc8_step(pkt_crc, b);
            if (pkt_pos >= pkt_len)
                st = ST_CSUM;
            break;
        case ST_CSUM:
            if (b == pkt_crc) {
                if (pkt_type == PROTO_TYPE && handle_payload(pkt_buf, pkt_len)) {
#ifdef PROTO_DEBUG
                    Serial.printf("pkt t=%02X len=%u crc ok\n", pkt_type, pkt_len);
#endif
                    pkt_count++;
                } else {
                    pkt_bad++;
                }
            } else {
                pkt_bad++;
            }
            st = ST_SYNC;
            break;
        }
    }
}

void protocol_send_cmd(uint8_t cmd_id)
{
    if (cmd_id == CMD_STOP_ALL) {
        Serial.println("CMD:STOP_ALL");
    } else if (cmd_id == CMD_START_FAVORITE) {
        Serial.println("CMD:START_FAVORITE");
    }
}

void protocol_send_start_model(const char *model_id)
{
    if (model_id && *model_id) {
        Serial.printf("CMD:START_MODEL:%s\n", model_id);
    }
}

void protocol_send_start_model_profile(const char *model_id, const char *profile)
{
    if (!model_id || !*model_id) return;
    if (profile && *profile) {
        Serial.printf("CMD:START_MODEL:%s:%s\n", model_id, profile);
    } else {
        Serial.printf("CMD:START_MODEL:%s\n", model_id);
    }
}