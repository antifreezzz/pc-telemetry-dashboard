#include "protocol.h"
#include "ui.h"

#define SYNC 0xAA
#define MAX_PKT 64

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
static uint8_t pkt_buf[MAX_PKT];
static uint32_t pkt_count = 0;
#ifdef PROTO_DEBUG
static uint32_t last_debug_ms = 0;
#endif

static void handle_packet(uint8_t type, const uint8_t *p, uint16_t len)
{
    switch (type)
    {
    case 0x01:  // CPU%
        if (len >= 1) ui_set_cpu_pct(p[0]);
        break;
    case 0x02:  // RAM pct, used_mb, total_mb
        if (len >= 9) {
            uint8_t pct = p[0];
            uint32_t used = (uint32_t)p[1] | ((uint32_t)p[2] << 8) |
                            ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24);
            uint32_t total = (uint32_t)p[5] | ((uint32_t)p[6] << 8) |
                             ((uint32_t)p[7] << 16) | ((uint32_t)p[8] << 24);
            ui_set_ram_pct(pct, (int)used, (int)total);
        }
        break;
    case 0x03:  // GPU%
        if (len >= 1) ui_set_gpu_pct(p[0]);
        break;
    case 0x04:  // NET rx_kbps, tx_kbps
        if (len >= 8) {
            uint32_t rx = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                          ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            uint32_t tx = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                          ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
            ui_set_net(rx, tx);
        }
        break;
    }
#ifdef PROTO_DEBUG
    Serial.printf("pkt t=%02X len=%u\n", type, len);
#endif
    pkt_count++;
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
            if (b == SYNC) st = ST_LEN_LO;
            break;
        case ST_LEN_LO:
            pkt_len = b;
            st = ST_LEN_HI;
            break;
        case ST_LEN_HI:
            pkt_len |= ((uint16_t)b) << 8;
            if (pkt_len > MAX_PKT) {
                st = ST_SYNC;
                break;
            }
            pkt_pos = 0;
            st = ST_TYPE;
            break;
        case ST_TYPE:
            pkt_type = b;
            if (pkt_len == 0) {
                handle_packet(pkt_type, nullptr, 0);
                st = ST_SYNC;
            } else {
                pkt_pos = 0;
                st = ST_PAYLOAD;
            }
            break;
        case ST_PAYLOAD:
            pkt_buf[pkt_pos++] = b;
            if (pkt_pos >= pkt_len) {
                handle_packet(pkt_type, pkt_buf, pkt_len);
                st = ST_SYNC;
            }
            break;
        case ST_CSUM:  // reserved
            st = ST_SYNC;
            break;
        }
    }
}
