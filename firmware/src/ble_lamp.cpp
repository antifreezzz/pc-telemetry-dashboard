#include "ble_lamp.h"
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <freertos/queue.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

// ---------------------------------------------------------------------------
// Device config
// ---------------------------------------------------------------------------
static const uint8_t TARGET_MAC[6] = TUYA_LAMP_MAC;
static const char *LOCAL_KEY_ASCII = TUYA_LOCAL_KEY;
static const char *CTRL_NAME = TUYA_CTRL_NAME;

// Timing from the verified reference (tuya-beacon-esphome).
#define TRACK_MS  300    // dwell for a single tracked command (scan is stopped during emit)
#define STEP_MS   250    // dwell per seq during the bootstrap sweep
#define TB_CATCH  0x40   // bootstrap sweep width (64 counters)
#define SCAN_MS   45000  // status-beacon scan window at boot

// ---------------------------------------------------------------------------
// Anti-replay / lamp state
// ---------------------------------------------------------------------------
// Seed from captured phone commands (the app's last accepted seq was 0x013929)
// is only a first-boot fallback. The real counter is persisted to NVS so
// reboots don't drift below the lamp's anti-replay value.
#define LAMP_SEED_SEQ 0x013929
static Preferences g_prefs;
static uint32_t g_seq = 0;
static bool g_synced = false;
static uint32_t g_bulb_counter = 0;
static bool g_have_counter = false;
static int g_mode = -1;  // -1 unknown, 0 white, 1 colour
static uint16_t g_color_h = 0;  // current colour HSV (for mode-aware brightness)
static uint8_t g_color_s = 0;

static void seq_save()
{
    g_prefs.putUInt("seq", g_seq);
}

static void seq_load()
{
    g_prefs.begin("blelamp", false);
    uint32_t saved = g_prefs.getUInt("seq", 0);
    if (saved != 0) {
        g_seq = saved;
        g_synced = true;      // exact counter known -> straight to track mode
    } else {
        g_seq = LAMP_SEED_SEQ;
        g_synced = false;     // sweep a small window on the first command
    }
}

// ---------------------------------------------------------------------------
// Crypto: big-endian XXTEA, 19 rounds, key = localKey as raw ASCII bytes.
// Byte-for-byte copy of the verified reference (tuya-beacon-esphome), which
// is validated on-device against known vectors in crypto_self_test().
// ---------------------------------------------------------------------------
#define XXTEA_DELTA 0x9E3779B9UL

static uint32_t rd32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void wr32be(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static void xxtea_encrypt_be(uint8_t block[16], const uint8_t key[16])
{
    uint32_t v[4], k[4];
    for (int i = 0; i < 4; i++) { v[i] = rd32be(block + 4 * i); k[i] = rd32be(key + 4 * i); }
    uint32_t sum = 0, z = v[3];
    for (int q = 0; q < 19; q++) {
        sum += XXTEA_DELTA;
        uint32_t e = (sum >> 2) & 3;
        for (int p = 0; p < 4; p++) {
            uint32_t y = v[(p + 1) & 3];
            uint32_t mx = (((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) ^ ((sum ^ y) + (k[(p & 3) ^ e] ^ z));
            v[p] += mx; z = v[p];
        }
    }
    for (int i = 0; i < 4; i++) wr32be(block + 4 * i, v[i]);
}

static void xxtea_decrypt_be(const uint8_t in[16], const uint8_t key[16], uint8_t out[16])
{
    uint32_t v[4], k[4];
    for (int i = 0; i < 4; i++) { v[i] = rd32be(in + 4 * i); k[i] = rd32be(key + 4 * i); }
    uint32_t sum = (uint32_t)(19 * XXTEA_DELTA), y = v[0];
    for (int q = 0; q < 19; q++) {
        uint32_t e = (sum >> 2) & 3;
        for (int p = 3; p > 0; p--) {
            uint32_t z = v[p - 1];
            y = v[p] -= (((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) ^ ((sum ^ y) + (k[(p & 3) ^ e] ^ z));
        }
        uint32_t z = v[3];
        y = v[0] -= (((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) ^ ((sum ^ y) + (k[e] ^ z));
        sum -= XXTEA_DELTA;
    }
    for (int i = 0; i < 4; i++) wr32be(out + 4 * i, v[i]);
}

static uint8_t crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80) crc = (crc << 1) ^ 0x07;
            else crc <<= 1;
        }
    }
    return crc;
}

// ---------------------------------------------------------------------------
// DP bodies (Tuya compact: dp_id | dp_type | dp_len | value)
// ---------------------------------------------------------------------------
static void dp_onoff(uint8_t out[4], bool on)
{
    out[0] = 0x01; out[1] = 0x01; out[2] = 0x01; out[3] = on ? 0x01 : 0x00;
}

static void dp_mode(uint8_t out[4], uint8_t m)
{
    out[0] = 0x02; out[1] = 0x04; out[2] = 0x01; out[3] = m;
}

static void dp_bright(uint8_t out[7], uint16_t lvl)
{
    out[0] = 0x03; out[1] = 0x02; out[2] = 0x04;
    out[3] = 0; out[4] = 0; out[5] = lvl >> 8; out[6] = lvl & 0xFF;
}

static void dp_cct(uint8_t out[7], uint16_t t)
{
    out[0] = 0x04; out[1] = 0x02; out[2] = 0x04;
    out[3] = 0; out[4] = 0; out[5] = t >> 8; out[6] = t & 0xFF;
}

static void dp_color(uint8_t out[7], uint16_t hue, uint8_t sat, uint8_t val)
{
    out[0] = 0x0B; out[1] = 0x00; out[2] = 0x04;
    out[3] = hue >> 8; out[4] = hue & 0xFF; out[5] = sat; out[6] = val;
}

// ---------------------------------------------------------------------------
// Command framing + advertisement
// ---------------------------------------------------------------------------
static void build_ct(uint32_t seq, const uint8_t *body, uint8_t blen, uint8_t ct[16])
{
    uint8_t pt[16];
    memset(pt, 0, 16);
    pt[0] = 0x07;  // DOWNLOAD_DP
    pt[1] = 0x2C;  // frame marker (this lamp uses 0x2C, not the reference's 0x24)
    pt[2] = (seq >> 16) & 0xFF;
    pt[3] = (seq >> 8) & 0xFF;
    pt[4] = seq & 0xFF;
    for (uint8_t i = 0; i < blen && i < 9; i++) pt[5 + i] = body[i];
    pt[14] = crc8(pt + 1, 13);  // CRC over [1:14], opcode byte excluded
    xxtea_encrypt_be(pt, (const uint8_t *)LOCAL_KEY_ASCII);
    memcpy(ct, pt, 16);
}

static uint8_t build_adv(const uint8_t ct[16], uint8_t adv[31])
{
    uint8_t *p = adv;
    *p++ = 0x02; *p++ = 0x01; *p++ = 0x01;              // Flags: LE General Discoverable (matches phone)
    size_t nlen = strlen(CTRL_NAME);
    if (nlen > 8) nlen = 8;                              // clamp so the advert stays <= 31 bytes
    *p++ = (uint8_t)(nlen + 1); *p++ = 0x09;            // Complete Local Name
    memcpy(p, CTRL_NAME, nlen); p += nlen;
    *p++ = 0x11; *p++ = 0x07;                           // Complete 128-bit Service UUID = ciphertext
    memcpy(p, ct, 16); p += 16;
    return (uint8_t)(p - adv);
}

static void scan_start();
static void scan_stop();

// ---------------------------------------------------------------------------
// Radio: connectable ADV_IND with the raw 31-byte payload
// ---------------------------------------------------------------------------
static void emit(uint32_t s, const uint8_t *body, uint8_t blen, uint32_t ms)
{
    uint8_t ct[16], adv[31];
    build_ct(s, body, blen, ct);
    uint8_t n = build_adv(ct, adv);

    scan_stop();  // scan and advertise share the radio; stop scan during the burst
    NimBLEAdvertising *pAdv = NimBLEDevice::getAdvertising();
    pAdv->stop();
    NimBLEAdvertisementData advData;
    advData.addData((char *)adv, n);
    pAdv->setAdvertisementData(advData);
    pAdv->setAdvertisementType(BLE_GAP_CONN_MODE_UND);
    pAdv->setScanResponse(false);
    pAdv->setMinInterval(0x20);  // 20 ms
    pAdv->setMaxInterval(0x40);  // 40 ms
    bool ok = pAdv->start();
    Serial.printf("[BLE] seq=0x%06X adv=%uB start=%s\n", s, n, ok ? "OK" : "FAIL");

    uint32_t t = 0;
    while (t < ms) {
        uint32_t chunk = (ms - t > 120) ? 120 : (ms - t);
        vTaskDelay(pdMS_TO_TICKS(chunk));
        t += chunk;
    }
    pAdv->stop();
    scan_start();  // resume background scan
}

static void send_dp(const uint8_t *body, uint8_t blen)
{
    if (!g_synced) {
        if (g_have_counter) g_seq = g_bulb_counter;
        Serial.printf("[BLE] bootstrap sweep 0x%06X..0x%06X (ctr=%d)\n",
                      g_seq + 1, g_seq + TB_CATCH, (int)g_have_counter);
        for (uint32_t s = g_seq + 1; s <= g_seq + TB_CATCH; s++)
            emit(s, body, blen, STEP_MS);
        g_seq = (g_seq + TB_CATCH) & 0xFFFFFF;
        g_synced = true;
        seq_save();
        Serial.printf("[BLE] bootstrap done, counter=0x%06X\n", g_seq);
        return;
    }
    if (g_have_counter && g_bulb_counter > g_seq)
        g_seq = g_bulb_counter;
    uint32_t s = (g_seq + 1) & 0xFFFFFF;
    emit(s, body, blen, TRACK_MS);
    g_seq = s;
    seq_save();
}

// ---------------------------------------------------------------------------
// Crypto self-test against the reference vectors for THIS key + marker.
// seq=0x01389A, DP1 on -> ciphertext f6e44d4b7e87eaf418dbfd10beda05f2
// ---------------------------------------------------------------------------
static void crypto_self_test()
{
    uint8_t body[4];
    dp_onoff(body, true);
    uint8_t ct[16];
    build_ct(0x01389A, body, 4, ct);
    const uint8_t exp[16] = {0xf6,0xe4,0x4d,0x4b,0x7e,0x87,0xea,0xf4,
                             0x18,0xdb,0xfd,0x10,0xbe,0xda,0x05,0xf2};
    bool ok = (memcmp(ct, exp, 16) == 0);
    Serial.printf("[BLE] crypto self-test: %s (ct=", ok ? "OK" : "FAIL");
    for (int i = 0; i < 16; i++) Serial.printf("%02x", ct[i]);
    Serial.println(")");
}

// ---------------------------------------------------------------------------
// Status-beacon scan: the bulb broadcasts company 0x07D0 (on-air D0 07)
//   | 0x04 | MAC[6] | sn[4 BE] | enc[16]
// Decrypt enc -> pt[4:8] is the anti-replay command counter (pt[8:11]==ff ff ff).
// Only SOME status frames carry the counter, so we must inspect every beacon.
// ---------------------------------------------------------------------------
class LampStatusCb : public NimBLEAdvertisedDeviceCallbacks {
public:
    void onResult(NimBLEAdvertisedDevice *adv) override
    {
        uint8_t *payload = adv->getPayload();
        size_t plen = adv->getPayloadLength();
        size_t p = 0;
        while (p + 1 < plen) {
            uint8_t alen = payload[p];
            if (alen == 0 || p + 1 + alen > plen) break;
            // Phone command advert: 128-bit Service UUID = XXTEA ciphertext.
            if (payload[p + 1] == 0x07 && alen == 0x11) {
                uint8_t ct[16], pt[16];
                memcpy(ct, payload + p + 2, 16);
                xxtea_decrypt_be(ct, (const uint8_t *)LOCAL_KEY_ASCII, pt);
                Serial.printf("[CMD] %s rssi=%d pt=%02x%02x%02x%02x%02x%02x%02x%02x "
                              "%02x%02x%02x%02x%02x%02x%02x%02x\n",
                              adv->getAddress().toString().c_str(), adv->getRSSI(),
                              pt[0],pt[1],pt[2],pt[3],pt[4],pt[5],pt[6],pt[7],
                              pt[8],pt[9],pt[10],pt[11],pt[12],pt[13],pt[14],pt[15]);
                uint32_t seq = ((uint32_t)pt[2] << 16) | ((uint32_t)pt[3] << 8) | pt[4];
                if ((pt[0] == 0x03 || pt[0] == 0x07) && pt[1] == 0x2C)
                    Serial.printf("[CMD]   op=%02X marker=%02X seq=0x%06X body=%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
                                  pt[0], pt[1], seq, pt[5],pt[6],pt[7],pt[8],pt[9],pt[10],pt[11],pt[12],pt[13]);
                // App-drift resync: the phone is also a controller; if it moved
                // the lamp's counter ahead, fast-forward ours so we stay in sync.
                if ((pt[0] == 0x03 || pt[0] == 0x07) && pt[1] == 0x2C && seq > g_seq) {
                    g_seq = seq;
                    Serial.printf("[BLE] resync from phone: seq=0x%06X\n", seq);
                }
            }
            if (payload[p + 1] == 0xFF && alen >= 30) {
                const uint8_t *m = payload + p + 2;   // company ID bytes (on-air: 07 D0 = 0xD007 LE)
                if (m[0] == 0x07 && m[1] == 0xD0) {
                    const uint8_t *d = m + 2;
                    if (d[0] == 0x04) {
                        uint8_t enc[16], pt[16];
                        memcpy(enc, d + 11, 16);
                        xxtea_decrypt_be(enc, (const uint8_t *)LOCAL_KEY_ASCII, pt);
                        uint32_t sn = rd32be(d + 7);
                        bool is_lamp = (memcmp(d + 1, TARGET_MAC, 6) == 0);
                        if (is_lamp && pt[8] == 0xFF && pt[9] == 0xFF && pt[10] == 0xFF) {
                            g_bulb_counter = rd32be(pt + 4);
                            g_have_counter = true;
                            Serial.printf("[BLE] *** STATUS COUNTER sn=0x%08X counter=0x%06X ***\n",
                                          sn, g_bulb_counter);
                        }
                    }
                }
            }
            p += alen + 1;
        }
    }
};
static LampStatusCb g_lamp_cb;

static void scan_done(NimBLEScanResults results)
{
    (void)results;
}

static void scan_start()
{
    NimBLEScan *pScan = NimBLEDevice::getScan();
    pScan->setAdvertisedDeviceCallbacks(&g_lamp_cb, true);  // wantDuplicates -> every beacon
    pScan->setActiveScan(false);                            // passive
    pScan->setInterval(100);
    pScan->setWindow(50);
    pScan->start(0, scan_done, true);                       // continuous, non-blocking
}

static void scan_stop()
{
    NimBLEScan *pScan = NimBLEDevice::getScan();
    pScan->stop();
}

// ---------------------------------------------------------------------------
// Queue + task
// ---------------------------------------------------------------------------
struct LampCommand {
    uint8_t dp_len;
    uint8_t body[9];
};

static QueueHandle_t g_lamp_queue = NULL;
static bool g_ambient_sync = false;

static void enqueue(const uint8_t *body, uint8_t len)
{
    if (!g_lamp_queue) return;
    LampCommand cmd;
    cmd.dp_len = len;
    memcpy(cmd.body, body, len);
    xQueueSend(g_lamp_queue, &cmd, 0);
}

static void ble_lamp_task(void *pvParameters)
{
    Serial.println("[NimBLE] Smart Lamp Queue Processor started on Core 0");
    NimBLEDevice::init(CTRL_NAME);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    esp_log_level_set("NimBLEScan", ESP_LOG_WARN);
    esp_log_level_set("NimBLEDevice", ESP_LOG_WARN);

    crypto_self_test();

    seq_load();
    scan_start();  // continuous background scan: captures phone commands + lamp status
    Serial.printf("[BLE] seed seq=0x%06X (synced=%d)\n", g_seq, (int)g_synced);

    LampCommand cmd;
    while (true) {
        if (xQueueReceive(g_lamp_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            Serial.printf("[BLE Lamp] >>> SEND DP %d <<<\n", cmd.body[0]);
            send_dp(cmd.body, cmd.dp_len);
        }
    }
}

void ble_lamp_init()
{
    if (!g_lamp_queue) {
        g_lamp_queue = xQueueCreate(16, sizeof(LampCommand));
    }
    xTaskCreatePinnedToCore(ble_lamp_task, "ble_lamp", 8192, NULL, 1, NULL, 0);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void ble_lamp_set_power(bool on)
{
    Serial.printf("[UI Action] ble_lamp_set_power(%d)\n", on);
    uint8_t body[4];
    dp_onoff(body, on);
    enqueue(body, 4);
}

void ble_lamp_set_brightness(uint16_t bright_0_1000)
{
    Serial.printf("[UI Action] ble_lamp_set_brightness(%d)\n", bright_0_1000);
    if (bright_0_1000 < 10) bright_0_1000 = 10;
    if (bright_0_1000 > 1000) bright_0_1000 = 1000;

    if (g_mode == 1) {
        // Colour mode: brightness = colour value (V), keep hue/saturation.
        uint8_t body[7];
        dp_color(body, g_color_h, g_color_s, (uint8_t)(bright_0_1000 / 10));
        enqueue(body, 7);
        return;
    }
    if (g_mode != 0) {           // white channel
        uint8_t m[4];
        dp_mode(m, 0);
        enqueue(m, 4);
        g_mode = 0;
    }
    uint8_t body[7];
    dp_bright(body, bright_0_1000);
    enqueue(body, 7);
}

void ble_lamp_set_cct(uint16_t temp_0_1000)
{
    Serial.printf("[UI Action] ble_lamp_set_cct(%d)\n", temp_0_1000);
    if (temp_0_1000 > 1000) temp_0_1000 = 1000;

    if (g_mode != 0) {
        uint8_t m[4];
        dp_mode(m, 0);
        enqueue(m, 4);
        g_mode = 0;
    }
    uint8_t body[7];
    dp_cct(body, temp_0_1000);
    enqueue(body, 7);
}

void ble_lamp_set_color_hsv(uint16_t hue, uint8_t sat, uint8_t val)
{
    Serial.printf("[UI Action] ble_lamp_set_color_hsv(%u,%u,%u)\n", hue, sat, val);
    if (hue > 360) hue = 360;
    if (sat > 100) sat = 100;
    if (val > 100) val = 100;
    g_color_h = hue;
    g_color_s = sat;

    if (g_mode != 1) {           // mode must precede colour (colour channel)
        uint8_t m[4];
        dp_mode(m, 1);
        enqueue(m, 4);
        g_mode = 1;
    }
    uint8_t body[7];
    dp_color(body, hue, sat, val);
    enqueue(body, 7);
}

void ble_lamp_set_color_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    Serial.printf("[UI Action] ble_lamp_set_color_rgb(%d,%d,%d)\n", r, g, b);

    float rf = r / 255.0f;
    float gf = g / 255.0f;
    float bf = b / 255.0f;
    float max_c = max(rf, max(gf, bf));
    float min_c = min(rf, min(gf, bf));
    float delta = max_c - min_c;

    float h = 0, s = 0, v = max_c;
    if (max_c > 0.0f) s = delta / max_c;
    if (delta > 0.0001f) {
        if (max_c == rf) h = 60.0f * fmodf(((gf - bf) / delta), 6.0f);
        else if (max_c == gf) h = 60.0f * (((bf - rf) / delta) + 2.0f);
        else h = 60.0f * (((rf - gf) / delta) + 4.0f);
        if (h < 0.0f) h += 360.0f;
    }

    uint16_t hu = (uint16_t)h;
    uint8_t su = (uint8_t)(s * 100.0f);
    uint8_t vu = (uint8_t)(v * 100.0f);
    g_color_h = hu;
    g_color_s = su;

    if (g_mode != 1) {           // mode must precede colour (colour channel)
        uint8_t m[4];
        dp_mode(m, 1);
        enqueue(m, 4);
        g_mode = 1;
    }
    uint8_t body[7];
    dp_color(body, hu, su, vu);
    enqueue(body, 7);
}

void ble_lamp_set_ambient_sync(bool enabled)
{
    g_ambient_sync = enabled;
}

void ble_lamp_run_hue_sweep()
{
    Serial.println("[BLE] HUE SWEEP 0..330 step 30");
    uint8_t m[4];
    dp_mode(m, 1);
    send_dp(m, 4);
    for (int h = 0; h <= 330; h += 30) {
        uint8_t body[7];
        dp_color(body, (uint16_t)h, 100, 100);
        Serial.printf("[SWEEP] hue=%d\n", h);
        send_dp(body, 7);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    Serial.println("[BLE] sweep done");
}

bool ble_lamp_get_ambient_sync()
{
    return g_ambient_sync;
}

bool ble_lamp_get_power()
{
    return true;
}