#include "phone_wpp.h"

#include "classic/rfcomm_bridge.h"

#include <Arduino.h>
#include <string.h>

static const uint16_t TYPE_START = 1;
static const uint16_t TYPE_INFO_REQ = 2;
static const uint16_t TYPE_INFO = 3;
static const uint16_t TYPE_VERSION = 4;
static const uint16_t TYPE_VERSION_RSP = 5;
static const uint16_t TYPE_CONNECT = 6;
static const uint16_t TYPE_START_RSP = 7;
static const uint16_t TYPE_PING = 8;
static const uint16_t TYPE_PING_RSP = 9;

static char s_ip[64];
static char s_ssid[64];
static char s_pass[64];
static char s_bssid[18];
static uint16_t s_port;
static int s_ready;
static int s_stage;
/** 헤드유닛이 프로젝션 소켓이 올라가 있다고 알린 상태. 이 동안에는 WifiStart를 다시 보내지 않는다. */
static int s_projection;
/** 마지막 PROJECTION_UP. 이보다 오래 조용하면 헤드유닛이 소켓을 잃은 것이다. */
static uint32_t s_projection_at;
static const uint32_t PROJECTION_SILENCE_MS = 4000;
static uint32_t s_deadline;
static uint8_t s_rx[1024];
static uint16_t s_rx_len;

/** protobuf varint 하나를 쓴다. 공간이 모자라면 -1. */
static int pb_varint(uint8_t *out, int cap, int at, uint32_t value)
{
    while (value >= 0x80) {
        if (at >= cap) {
            return -1;
        }
        out[at++] = (uint8_t)((value & 0x7f) | 0x80);
        value >>= 7;
    }
    if (at >= cap) {
        return -1;
    }
    out[at++] = (uint8_t)value;
    return at;
}

/** protobuf 필드 키를 쓴다. */
static int pb_key(uint8_t *out, int cap, int at, int field, int wire)
{
    return pb_varint(out, cap, at, (uint32_t)((field << 3) | wire));
}

/** protobuf varint 필드를 쓴다. */
static int pb_int(uint8_t *out, int cap, int at, int field, uint32_t value)
{
    at = pb_key(out, cap, at, field, 0);
    if (at < 0) {
        return -1;
    }
    return pb_varint(out, cap, at, value);
}

/** protobuf length-delimited 필드를 쓴다. */
static int pb_bytes(uint8_t *out, int cap, int at, int field, const uint8_t *data, int len)
{
    at = pb_key(out, cap, at, field, 2);
    if (at < 0) {
        return -1;
    }
    at = pb_varint(out, cap, at, (uint32_t)len);
    if (at < 0 || at + len > cap) {
        return -1;
    }
    if (len > 0) {
        memcpy(out + at, data, (size_t)len);
    }
    return at + len;
}

/** protobuf 문자열 필드를 쓴다. */
static int pb_str(uint8_t *out, int cap, int at, int field, const char *text)
{
    return pb_bytes(out, cap, at, field, (const uint8_t *)text, (int)strlen(text));
}

/** 길이 2바이트와 타입 2바이트가 앞에 붙는 WPP 프레임을 폰으로 보낸다. */
static int send_wpp(uint16_t type, const uint8_t *payload, int len)
{
    uint8_t frame[700];
    if (len < 0 || len + 4 > (int)sizeof(frame)) {
        return -1;
    }
    frame[0] = (uint8_t)(len >> 8);
    frame[1] = (uint8_t)len;
    frame[2] = (uint8_t)(type >> 8);
    frame[3] = (uint8_t)type;
    if (len > 0) {
        memcpy(frame + 4, payload, (size_t)len);
    }
    Serial.printf("phone WPP type %u, %d bytes\n", type, len);
    return rfcomm_send(frame, (size_t)(len + 4));
}

/** 버전 교환을 보낸다. 포트 5288은 여기 넣지 않는다. */
static void send_version(void)
{
    uint8_t body[32];
    int at = 0;
    /* Projection address goes in WifiStartRequest. This message must not name port 5288,
     * or the phone dials that port speaking WPP and the projection handshake is reset. */
    at = pb_int(body, sizeof(body), at, 1, 4);
    at = pb_int(body, sizeof(body), at, 2, 2);
    send_wpp(TYPE_VERSION, body, at);
}

/** 프로젝션이 올라가 있지 않을 때만 WifiStart를 보낸다. */
static void send_start(void)
{
    if (s_projection) {
        Serial.println("projection is up; not dialing 5288 again");
        return;
    }
    uint8_t body[96];
    int at = 0;
    at = pb_str(body, sizeof(body), at, 1, s_ip);
    at = pb_int(body, sizeof(body), at, 2, s_port);
    at = pb_int(body, sizeof(body), at, 3, 0);
    send_wpp(TYPE_START, body, at);
    if (s_stage < 2) {
        s_stage = 2;
    }
}

/** 현재 네트워크를 WifiInfo로 보낸다. */
static void send_info(void)
{
    uint8_t body[240];
    int at = 0;
    at = pb_str(body, sizeof(body), at, 1, s_ssid);
    at = pb_str(body, sizeof(body), at, 2, s_pass);
    at = pb_str(body, sizeof(body), at, 3, s_bssid);
    at = pb_int(body, sizeof(body), at, 4, 8);
    at = pb_int(body, sizeof(body), at, 5, 0);
    Serial.printf("phone WifiInfo %s %s:%u\n", s_ssid, s_ip, s_port);
    send_wpp(TYPE_INFO, body, at);
    if (s_stage < 3) {
        s_stage = 3;
    }
}

/** 스냅샷에서 길이 접두 문자열 하나를 꺼낸다. */
static int take_string(const uint8_t *net, uint16_t len, uint16_t *at, char *out, int cap)
{
    uint8_t n;
    if (*at >= len || cap < 2) {
        return -1;
    }
    n = net[(*at)++];
    if ((uint16_t)(*at + n) > len || n >= cap) {
        return -1;
    }
    memcpy(out, net + *at, n);
    out[n] = 0;
    *at = (uint16_t)(*at + n);
    return 0;
}

/** 스키마 0x01 Wi-Fi 스냅샷을 WPP에 쓸 주소와 자격으로 푼다. */
static int load_network(const uint8_t *net, uint16_t len)
{
    uint16_t at = 0;
    uint8_t mac[6];
    char security[32];
    if (net == nullptr || len < 8 || net[0] != 0x01) {
        return -1;
    }
    at = 5;
    s_port = (uint16_t)(net[at] | (net[at + 1] << 8));
    at = 7;
    if (take_string(net, len, &at, s_ssid, sizeof(s_ssid)) != 0) {
        return -1;
    }
    if (take_string(net, len, &at, s_pass, sizeof(s_pass)) != 0) {
        return -1;
    }
    if ((uint16_t)(at + 6) > len) {
        return -1;
    }
    memcpy(mac, net + at, 6);
    at = (uint16_t)(at + 6);
    snprintf(s_bssid, sizeof(s_bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (take_string(net, len, &at, security, sizeof(security)) != 0) {
        return -1;
    }
    if (take_string(net, len, &at, s_ip, sizeof(s_ip)) != 0) {
        return -1;
    }
    return s_ssid[0] != 0 && s_ip[0] != 0 && s_port != 0 ? 0 : -1;
}

/** 완성된 WPP 메시지 하나에 답한다. */
static void handle_message(uint16_t type, const uint8_t *payload, uint16_t len)
{
    Serial.printf("phone WPP rx type %u, %u bytes\n", type, len);
    if (type == TYPE_PING) {
        send_wpp(TYPE_PING_RSP, payload, len);
        return;
    }
    if (type == TYPE_VERSION_RSP && s_stage < 2) {
        send_start();
        return;
    }
    if (type == TYPE_INFO_REQ) {
        if (s_stage < 2) {
            send_start();
        }
        send_info();
    }
}

/** 수신 버퍼에서 길이가 맞는 WPP 프레임을 꺼내 처리한다. */
static void drain_rx(void)
{
    while (s_rx_len >= 4) {
        uint16_t size = (uint16_t)((s_rx[0] << 8) | s_rx[1]);
        uint16_t type = (uint16_t)((s_rx[2] << 8) | s_rx[3]);
        if ((uint32_t)size + 4 > sizeof(s_rx)) {
            s_rx_len = 0;
            return;
        }
        if (s_rx_len < (uint16_t)(size + 4)) {
            return;
        }
        handle_message(type, s_rx + 4, size);
        memmove(s_rx, s_rx + 4 + size, s_rx_len - (size + 4));
        s_rx_len = (uint16_t)(s_rx_len - (size + 4));
    }
}

void phone_wpp_on_open(const uint8_t *net, uint16_t net_len)
{
    s_rx_len = 0;
    s_stage = 0;
    s_ready = load_network(net, net_len) == 0;
    if (!s_ready) {
        Serial.println("phone RFCOMM open, no Wi-Fi snapshot to send");
        return;
    }
    send_version();
    if (s_projection) {
        s_stage = 2;
        Serial.println("projection is up; answering with version only");
        return;
    }
    Serial.printf("phone RFCOMM open, sending start info %s %s:%u\n", s_ssid, s_ip, s_port);
    s_stage = 1;
    s_deadline = millis() + 3000UL;
}

void phone_wpp_on_close(void)
{
    s_ready = 0;
    s_stage = 0;
    s_rx_len = 0;
}

void phone_wpp_on_rx(const uint8_t *data, uint16_t len)
{
    if (!s_ready || data == nullptr || len == 0) {
        return;
    }
    if ((uint32_t)s_rx_len + len > sizeof(s_rx)) {
        s_rx_len = 0;
    }
    memcpy(s_rx + s_rx_len, data, len);
    s_rx_len = (uint16_t)(s_rx_len + len);
    drain_rx();
}

void phone_wpp_poll(void)
{
    if (s_projection && (int32_t)(millis() - s_projection_at) >= (int32_t)PROJECTION_SILENCE_MS) {
        s_projection = 0;
        Serial.println("projection heartbeat lost");
    }
    if (s_ready && s_stage == 1 && (int32_t)(millis() - s_deadline) >= 0) {
        send_start();
    }
}

void phone_wpp_allow_start(void)
{
    if (s_projection) {
        Serial.println("projection is up; not arming another start");
    }
}

void phone_wpp_set_projection(int up)
{
    if (up) {
        s_projection_at = millis();
        if (s_projection) {
            return;
        }
        s_projection = 1;
        Serial.println("projection socket is up");
        return;
    }
    if (!s_projection) {
        return;
    }
    s_projection = 0;
    Serial.println("projection socket is down");
}
