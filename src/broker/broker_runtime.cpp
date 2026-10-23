#include "broker_runtime.h"

#include "protocol/bridge_codec.h"
#include "protocol/broker_protocol.h"
#include "classic/hfp_hf.h"
#include "ble/hu_ble.h"
#include "classic/rfcomm_bridge.h"

#include <Arduino.h>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* 1이면 GATT/RFCOMM 페이로드를 16진수로 찍는다. 기본은 끔. */
#ifndef BROKER_LOG_DATA_BYTES
#define BROKER_LOG_DATA_BYTES 0
#endif

struct Msg {
    uint8_t kind;
    uint8_t extra;
    uint16_t len;
    uint8_t bda[6];
    uint8_t data[BROKER_ATT_CHUNK];
};

struct FrameLabel {
    uint8_t type;
    const char *name;
    const char *meaning;
};

static const FrameLabel k_labels[] = {
    {CMD_HELLO, "HELLO", nullptr},
    {CMD_SET_NETWORK, "SET_NETWORK", nullptr},
    {CMD_AA_ENABLE, "AA_ENABLE", nullptr},
    {CMD_AA_DISABLE, "AA_DISABLE", nullptr},
    {CMD_GET_STATUS, "GET_STATUS", nullptr},
    {CMD_SET_BOND_MODE, "SET_BOND_MODE", nullptr},
    {CMD_PROJECTION_UP, "PROJECTION_UP", nullptr},
    {EVT_HELLO_ACK, "HELLO_ACK", "hello ack to head unit"},
    {EVT_BT_READY, "BT_READY", "classic bluetooth is ready"},
    {EVT_PHONE_LINK, "PHONE_LINK", "phone address and HFP state"},
    {EVT_HFP_STATE, "HFP_STATE", "hands-free link state"},
    {EVT_RFCOMM_OPEN, "RFCOMM_OPEN", "phone Android Auto channel opened"},
    {EVT_RFCOMM_CLOSE, "RFCOMM_CLOSE", "phone Android Auto channel closed"},
    {EVT_ERROR, "ERROR", "broker error"},
    {EVT_STATUS, "STATUS", "broker status snapshot"},
    {DATA_RFCOMM_TX, "RFCOMM_TX", "head unit bytes forwarded to phone"},
    {DATA_RFCOMM_RX, "RFCOMM_RX", "phone bytes forwarded to head unit"},
    {EVT_CREDIT, "CREDIT", nullptr},
};

static QueueHandle_t s_q;
static BrokerFrag s_cmd_frag;
static BrokerFrag s_data_frag;
static uint8_t s_frame[BROKER_HEADER_LEN + BROKER_MAX_PAYLOAD];
static uint8_t s_net[480];
static uint16_t s_net_len;
static uint32_t s_generation;
static uint16_t s_app_seq;
static uint16_t s_transport;
static uint16_t s_dropped;
static uint16_t s_last_error;
static uint8_t s_aa;
static uint8_t s_paired;
static uint8_t s_bt_ready;
static uint8_t s_bt_announced;
static uint32_t s_bond_until;
/** 버튼이나 헤드유닛이 연 페어링 창. 부팅 직후 검색 창과는 구분한다. */
static int s_pairing;
static int s_pair_down;
static int s_pair_fired;
static uint32_t s_pair_hold_at;
static const int PIN_PAIR = 0;
static const int PIN_LED = 2;
static const uint32_t PAIR_HOLD_MS = 5000;
static const uint16_t PAIR_SECONDS = 120;
/** 부팅 뒤 마지막 폰으로 HFP를 다시 거는 시각. 0이면 시도하지 않는다. */
static uint32_t s_reconnect_until;
static uint32_t s_reconnect_next;
static int s_ble_up;
static uint32_t s_ble_down_at;
static int s_boot_window_armed;
/** 헤드유닛이 허용한 DATA_RFCOMM_RX 조각 수. EVT_CREDIT으로 채운다. */
static uint8_t s_credit;
static const uint8_t CREDIT_CAP = 8;
/** 프로젝션 소켓이 올라가 있다고 헤드유닛이 알린 상태. 상태 표시용. */
static int s_projection;

/** 리틀엔디언 16비트를 쓴다. */
static void put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xff);
    p[1] = (uint8_t)(value >> 8);
}

/** 리틀엔디언 32비트를 쓴다. */
static void put_u32(uint8_t *p, uint32_t value)
{
    put_u16(p, (uint16_t)value);
    put_u16(p + 2, (uint16_t)(value >> 16));
}

/** 프레임 종류에 해당하는 로그 이름을 찾는다. 없으면 null. */
static const FrameLabel *find_label(uint8_t type)
{
    for (size_t i = 0; i < sizeof(k_labels) / sizeof(k_labels[0]); i++) {
        if (k_labels[i].type == type) {
            return &k_labels[i];
        }
    }
    return nullptr;
}

#if BROKER_LOG_DATA_BYTES
/** 바이트열을 방향과 태그 뒤에 16진수로 찍는다. */
static void log_bytes(const char *dir, const char *tag, const uint8_t *data, uint16_t len)
{
    Serial.printf("%s %s len=%u", dir, tag, len);
    if (data == nullptr || len == 0) {
        Serial.println();
        return;
    }
    for (uint16_t i = 0; i < len; i++) {
        Serial.printf(" %02X", data[i]);
    }
    Serial.println();
}
#endif

/** 논리 프레임을 만들어 BLE 이벤트 또는 데이터 특성으로 나눠 보낸다. */
static void emit(uint8_t type, uint8_t channel, const uint8_t *payload, uint16_t len, int as_data)
{
    const FrameLabel *label = find_label(type);
    uint16_t seq = s_app_seq++;
    uint16_t frame_len = broker_encode(type, BROKER_FLAG_START | BROKER_FLAG_END, channel,
                                       seq, payload, len, s_frame, sizeof(s_frame));
    uint16_t room;
    uint8_t count;
    uint16_t transport;
    if (frame_len == 0) {
        s_dropped++;
        return;
    }
    Serial.printf("tx frame type=%02X %s ch=%u seq=%u len=%u | %s\n",
                  type, label ? label->name : "UNKNOWN", channel, seq, len,
                  label && label->meaning ? label->meaning : "broker message");
#if BROKER_LOG_DATA_BYTES
    log_bytes("tx", "frame", payload, len);
#endif
    room = (uint16_t)(hu_ble_mtu() - 3 - 4);
    if (room > 508) {
        room = 508;
    }
    if (room < 8) {
        room = 8;
    }
    count = (uint8_t)((frame_len + room - 1) / room);
    transport = s_transport++;
    for (uint8_t index = 0; index < count; index++) {
        uint16_t off = (uint16_t)index * room;
        uint16_t n = (uint16_t)(frame_len - off);
        uint8_t packet[512];
        if (n > room) {
            n = room;
        }
        put_u16(packet, transport);
        packet[2] = index;
        packet[3] = count;
        memcpy(packet + 4, s_frame + off, n);
#if BROKER_LOG_DATA_BYTES
        log_bytes("tx", as_data ? "ble-data" : "ble-evt", packet, (uint16_t)(n + 4));
#endif
        if (as_data) {
            hu_ble_notify_data(packet, (uint16_t)(n + 4));
        } else {
            hu_ble_notify_event(packet, (uint16_t)(n + 4));
        }
    }
}

/** 오류 코드를 헤드유닛에 알리고 마지막 오류로 기억한다. */
static void emit_error(uint16_t code)
{
    uint8_t body[2];
    s_last_error = code;
    put_u16(body, code);
    emit(EVT_ERROR, BROKER_CH_CONTROL, body, sizeof(body), 0);
}

/** 페어링, RFCOMM, HFP, 네트워크 세대가 들어 있는 24바이트 상태를 보낸다. */
static void emit_status(void)
{
    uint8_t body[24];
    uint16_t mtu = hu_ble_mtu();
    uint8_t flags = 0;
    memset(body, 0, sizeof(body));
    if (s_paired) {
        flags |= 0x01;
    }
    if (rfcomm_is_open()) {
        flags |= 0x02;
    }
    if (s_aa) {
        flags |= 0x04;
    }
    if (s_net_len) {
        flags |= 0x08;
    }
    if (hu_ble_event_ready()) {
        flags |= 0x10;
    }
    if (s_bond_until != 0) {
        flags |= 0x20;
    }
    if (s_projection) {
        flags |= 0x40;
    }
    body[0] = BROKER_VERSION;
    body[1] = flags;
    body[2] = hfp_hf_state();
    body[3] = (uint8_t)(rfcomm_is_open() ? 1 : 0);
    put_u16(body + 4, mtu);
    body[6] = (uint8_t)(s_q ? uxQueueMessagesWaiting(s_q) : 0);
    /* body[7]은 헤드유닛이 아직 소진하지 않은 RX 크레딧. */
    body[7] = s_credit;
    put_u16(body + 8, s_dropped);
    put_u16(body + 10, s_last_error);
    put_u32(body + 12, s_generation);
    memcpy(body + 16, "arduino", 7);
    emit(EVT_STATUS, BROKER_CH_CONTROL, body, sizeof(body), 0);
}

/** 폰 RFCOMM 바이트를 DATA_RFCOMM_RX로 헤드유닛에 넘긴다. BLINK의 ZB 중계와 같다. */
static void forward_phone_rx(const uint8_t *data, uint16_t len)
{
    if (!s_aa || data == nullptr || len == 0) {
        return;
    }
    if (!hu_ble_data_ready()) {
        return;
    }
    if (s_credit == 0) {
        s_dropped++;
        return;
    }
    s_credit--;
    emit(DATA_RFCOMM_RX, BROKER_CH_RFCOMM, data, len, 1);
}

/** 헤드유닛이 준 크레딧을 쌓는다. */
static void add_credit(uint8_t extra)
{
    if (extra == 0) {
        extra = 1;
    }
    if (extra > CREDIT_CAP) {
        extra = CREDIT_CAP;
    }
    uint8_t room = (uint8_t)(CREDIT_CAP - s_credit);
    if (extra > room) {
        extra = room;
    }
    s_credit = (uint8_t)(s_credit + extra);
}

/** 스키마 0x01 스냅샷의 generation을 읽고, 형식이 아니면 기존 값을 유지한다. */
static uint32_t read_generation(const uint8_t *payload, uint16_t len)
{
    if (payload == nullptr || len < 5 || payload[0] != 0x01) {
        return s_generation;
    }
    return (uint32_t)payload[1] | ((uint32_t)payload[2] << 8) |
           ((uint32_t)payload[3] << 16) | ((uint32_t)payload[4] << 24);
}

/** 헤드유닛이 보낸 Wi-Fi 스냅샷을 저장하고 상태를 알린다. */
static void remember_network(const uint8_t *payload, uint16_t len)
{
    if (len > sizeof(s_net)) {
        emit_error(BROKER_ERR_TOO_BIG);
        return;
    }
    memcpy(s_net, payload, len);
    s_net_len = len;
    s_generation = read_generation(payload, len);
    Serial.printf("network snapshot stored, %u bytes, generation %lu\n",
                  s_net_len, (unsigned long)s_generation);
    emit_status();
}

/** 저장된 폰 연결을 끊고 클래식 본드를 지운 뒤 새 폰을 받을 준비를 한다. */
static void prepare_new_phone(void)
{
    s_aa = 0;
    s_paired = 0;
    s_reconnect_until = 0;
    s_reconnect_next = 0;
    rfcomm_disconnect();
    s_projection = 0;
    s_credit = 0;
    hfp_hf_forget_last();
    rfcomm_clear_bonds();
    Serial.println("cleared phone bonds for new pairing");
}

/** 폰이 이 브로커를 찾을 수 있는 페어링 창을 연다. GPIO2 LED는 이 창에서만 깜빡인다. */
static void enter_pairing(uint16_t seconds)
{
    if (seconds == 0) {
        seconds = PAIR_SECONDS;
    }
    s_bond_until = millis() + (uint32_t)seconds * 1000UL;
    s_pairing = 1;
    rfcomm_set_discoverable(1);
    Serial.printf("pairing mode for %u s\n", seconds);
}

/** 페어링 창을 닫고 LED를 끈다. */
static void leave_pairing(void)
{
    s_bond_until = 0;
    s_pairing = 0;
    digitalWrite(PIN_LED, LOW);
    rfcomm_set_discoverable(0);
    Serial.println("pairing mode ended");
}

/** GPIO0이 5초 이상 눌리면 페어링 창을 연다. 창이 열려 있는 동안 GPIO2를 깜빡인다. */
static void poll_pair_button(void)
{
    if (digitalRead(PIN_PAIR) == LOW) {
        if (!s_pair_down) {
            s_pair_down = 1;
            s_pair_hold_at = millis();
        }
        if (!s_pair_fired && (int32_t)(millis() - s_pair_hold_at) >= (int32_t)PAIR_HOLD_MS) {
            s_pair_fired = 1;
            enter_pairing(PAIR_SECONDS);
            emit_status();
        }
    } else {
        s_pair_down = 0;
        s_pair_fired = 0;
    }
    if (s_pairing) {
        digitalWrite(PIN_LED, ((millis() / 250) & 1) ? HIGH : LOW);
    }
}

/** 아직 알리지 않은 클래식 블루투스 준비를 한 번만 보낸다.
 *  require_notify가 참이면 이벤트 notify가 켜진 뒤에만 보낸다. */
static void announce_bt_ready(int require_notify)
{
    uint8_t ready = 1;
    if (!s_bt_ready || s_bt_announced) {
        return;
    }
    if (require_notify && !hu_ble_event_ready()) {
        return;
    }
    emit(EVT_BT_READY, BROKER_CH_CONTROL, &ready, 1, 0);
    s_bt_announced = 1;
}

/** 폰 주소 6바이트와 HFP 상태 1바이트를 헤드유닛에 보낸다. */
static void emit_phone_link(const uint8_t bda[6], uint8_t state)
{
    uint8_t link[7];
    memcpy(link, bda, 6);
    link[6] = state;
    emit(EVT_PHONE_LINK, BROKER_CH_CONTROL, link, sizeof(link), 0);
}

/** 조립이 끝난 논리 프레임을 명령으로 처리한다. */
static void on_frame(const uint8_t *frame, uint16_t frame_len)
{
    uint8_t type, flags, channel;
    uint16_t seq, len;
    const uint8_t *payload = nullptr;
    const FrameLabel *label;
    int rc = broker_decode(frame, frame_len, &type, &flags, &channel, &seq, &payload, &len);
    if (rc != 0) {
        emit_error((uint16_t)rc);
        return;
    }
    label = find_label(type);
    Serial.printf("rx frame type=%02X %s flags=%02X ch=%u seq=%u len=%u\n",
                  type, label ? label->name : "UNKNOWN", flags, channel, seq, len);
#if BROKER_LOG_DATA_BYTES
    log_bytes("rx", "frame", payload, len);
#endif
    switch (type) {
    case CMD_HELLO: {
        uint8_t ack[9] = {BROKER_VERSION, 'b', 'r', 'o', 'k', 'e', 'r', '1', 0};
        emit(EVT_HELLO_ACK, BROKER_CH_CONTROL, ack, sizeof(ack), 0);
        announce_bt_ready(0);
        break;
    }
    case CMD_SET_NETWORK:
        remember_network(payload, len);
        break;
    case CMD_AA_ENABLE:
        /* BLINK와 같이 WPP는 헤드유닛이 한다. 여기서는 HFP와 RFCOMM 중계만 켠다. */
        s_aa = 1;
        hfp_hf_connect_bonded();
        if (rfcomm_is_open()) {
            /* 이미 열린 채널이면 헤드유닛이 핸드셰이크를 시작할 수 있게 다시 알린다. */
            emit(EVT_RFCOMM_OPEN, BROKER_CH_CONTROL, nullptr, 0, 0);
        }
        emit_status();
        break;
    case CMD_AA_DISABLE:
        s_aa = 0;
        s_credit = 0;
        emit_status();
        break;
    case CMD_PROJECTION_UP:
        s_projection = len > 0 && payload != nullptr && payload[0] != 0;
        emit_status();
        break;
    case CMD_GET_STATUS:
        emit_status();
        break;
    case CMD_SET_BOND_MODE: {
        uint8_t mode = len > 0 && payload != nullptr ? payload[0] : 0;
        uint16_t seconds = 120;
        if (len >= 3 && payload != nullptr) {
            seconds = (uint16_t)(payload[1] | (payload[2] << 8));
            if (seconds == 0) {
                seconds = 120;
            }
        }
        if (mode == 0) {
            leave_pairing();
        } else {
            if (mode == 2) {
                prepare_new_phone();
            }
            enter_pairing(seconds);
        }
        emit_status();
        break;
    }
    case EVT_CREDIT:
        if (payload != nullptr && len > 0) {
            add_credit(payload[0]);
        } else {
            add_credit(1);
        }
        break;
    case DATA_RFCOMM_TX:
        /* 헤드유닛이 보낸 WPP/핸드셰이크 바이트를 폰 RFCOMM으로 중계한다. */
        if (!rfcomm_is_open() || payload == nullptr) {
            emit_error(BROKER_ERR_RFCOMM);
            break;
        }
#if BROKER_LOG_DATA_BYTES
        log_bytes("tx", "phone", payload, len);
#endif
        if (rfcomm_send(payload, len) != 0) {
            emit_error(BROKER_ERR_RFCOMM);
        }
        break;
    default:
        emit_error(BROKER_ERR_FRAME);
        break;
    }
}

/** BLE 조각을 이어 붙이고, 한 프레임이 되면 바로 처리한다. */
static void take_fragment(BrokerFrag *frag, const uint8_t *data, uint16_t len)
{
    int rc = broker_frag_push(frag, data, len);
    if (rc == 1) {
        uint16_t n = frag->len;
        on_frame(frag->buf, n);
        broker_frag_reset(frag);
    } else if (rc < 0) {
        emit_error((uint16_t)(-rc));
    }
}

void broker_runtime_init(void)
{
    if (s_q == nullptr) {
        s_q = xQueueCreate(12, sizeof(Msg));
    }
    broker_frag_reset(&s_cmd_frag);
    broker_frag_reset(&s_data_frag);
    pinMode(PIN_PAIR, INPUT_PULLUP);
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);
}

void broker_ble_up(void)
{
    s_ble_up = 1;
    s_ble_down_at = 0;
}

void broker_ble_down(void)
{
    broker_frag_reset(&s_cmd_frag);
    broker_frag_reset(&s_data_frag);
    s_bt_announced = 0;
    s_ble_up = 0;
    if (s_ble_down_at == 0) {
        s_ble_down_at = millis();
    }
}

int broker_post(uint8_t kind, const uint8_t *data, uint16_t len, const uint8_t bda[6], uint8_t extra)
{
    Msg msg;
    if (s_q == nullptr) {
        return -1;
    }
    memset(&msg, 0, sizeof(msg));
    msg.kind = kind;
    msg.extra = extra;
    if (len > BROKER_ATT_CHUNK) {
        len = BROKER_ATT_CHUNK;
    }
    msg.len = len;
    if (data != nullptr && len > 0) {
        memcpy(msg.data, data, len);
    }
    if (bda != nullptr) {
        memcpy(msg.bda, bda, 6);
    }
    if (xQueueSend(s_q, &msg, 0) != pdTRUE) {
        s_dropped++;
        return -1;
    }
    return 0;
}

/** 큐에서 꺼낸 블루투스 이벤트 하나를 처리한다. */
static void on_msg(const Msg *msg)
{
    switch (msg->kind) {
    case MSG_GATT_CMD:
#if BROKER_LOG_DATA_BYTES
        log_bytes("rx", "ble-cmd", msg->data, msg->len);
#endif
        take_fragment(&s_cmd_frag, msg->data, msg->len);
        break;
    case MSG_GATT_DATA:
#if BROKER_LOG_DATA_BYTES
        log_bytes("rx", "ble-data", msg->data, msg->len);
#endif
        take_fragment(&s_data_frag, msg->data, msg->len);
        break;
    case MSG_PHONE_RX:
#if BROKER_LOG_DATA_BYTES
        log_bytes("rx", "phone", msg->data, msg->len);
#endif
        /* BLINK의 ZB와 같이 폰 바이트는 헤드유닛으로만 넘긴다. ESP32는 WPP를 해석하지 않는다. */
        forward_phone_rx(msg->data, msg->len);
        break;
    case MSG_HFP:
        emit(EVT_HFP_STATE, BROKER_CH_CONTROL, &msg->extra, 1, 0);
        if (msg->extra == 2 || msg->extra == 3) {
            hfp_hf_remember(msg->bda);
        }
        if (msg->extra >= 3) {
            s_paired = 1;
            emit_phone_link(msg->bda, msg->extra);
        }
        break;
    case MSG_RFCOMM_OPEN:
        hfp_hf_remember(msg->bda);
        /* 헤드유닛이 크레딧을 채우기 전에 오는 첫 응답을 버리지 않도록 조금 열어 둔다. */
        if (s_aa) {
            add_credit(4);
        }
        /* BLINK의 AA: 채널이 열리면 헤드유닛이 먼저 말한다. */
        emit(EVT_RFCOMM_OPEN, BROKER_CH_CONTROL, nullptr, 0, 0);
        break;
    case MSG_RFCOMM_CLOSE:
        emit(EVT_RFCOMM_CLOSE, BROKER_CH_CONTROL, &msg->extra, 1, 0);
        break;
    case MSG_SPP_READY:
        s_bt_ready = 1;
        if (s_bond_until == 0 && !s_boot_window_armed) {
            s_boot_window_armed = 1;
            s_bond_until = millis() + 180000UL;
        }
        rfcomm_set_discoverable(s_bond_until != 0);
        announce_bt_ready(1);
        /* 헤드유닛의 AA_ENABLE을 기다리지 않는다. 전원이 들어오면 마지막 폰을 부른다. */
        s_reconnect_until = millis() + 60000UL;
        s_reconnect_next = millis() + 5000UL;
        hfp_hf_connect_last();
        break;
    case MSG_PAIRED:
        s_paired = 1;
        hfp_hf_remember(msg->bda);
        if (s_aa) {
            hfp_hf_connect(msg->bda);
        }
        if (s_pairing) {
            leave_pairing();
        }
        emit_phone_link(msg->bda, hfp_hf_state());
        break;
    default:
        break;
    }
}

void broker_poll(void)
{
    Msg msg;
    int budget = 8;
    poll_pair_button();
    if (s_pairing && (rfcomm_is_open() || hfp_hf_is_up())) {
        leave_pairing();
    }
    if (s_bond_until != 0 && (int32_t)(millis() - s_bond_until) >= 0) {
        if (s_pairing) {
            leave_pairing();
        } else {
            s_bond_until = 0;
            rfcomm_set_discoverable(0);
        }
    }
    if (s_reconnect_until != 0) {
        if (rfcomm_is_open() || hfp_hf_is_up() ||
            (int32_t)(millis() - s_reconnect_until) >= 0) {
            s_reconnect_until = 0;
        } else if (hfp_hf_state() == 0 && (int32_t)(millis() - s_reconnect_next) >= 0) {
            s_reconnect_next = millis() + 5000UL;
            hfp_hf_connect_last();
        }
    }
    if (!s_ble_up && s_ble_down_at != 0 && (int32_t)(millis() - s_ble_down_at) >= 10000) {
        s_ble_down_at = 0;
        if (rfcomm_is_open()) {
            Serial.println("head unit BLE down for 10s; closing the phone link");
            rfcomm_disconnect();
        }
        s_projection = 0;
        s_credit = 0;
        s_aa = 0;
    }
    announce_bt_ready(1);
    while (budget-- > 0 && s_q != nullptr && xQueueReceive(s_q, &msg, 0) == pdTRUE) {
        on_msg(&msg);
    }
}

void broker_self_check(void)
{
    static const uint8_t tv[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    uint16_t crc = broker_crc16(tv, sizeof(tv));
    uint8_t encoded[32];
    uint8_t type, flags, channel;
    uint16_t seq, len;
    const uint8_t *payload = nullptr;
    uint16_t n = broker_encode(CMD_HELLO, BROKER_FLAG_START | BROKER_FLAG_END, BROKER_CH_CONTROL,
                               7, tv, sizeof(tv), encoded, sizeof(encoded));
    int rc = broker_decode(encoded, n, &type, &flags, &channel, &seq, &payload, &len);
    if (crc != 0x29B1 || rc != 0 || type != CMD_HELLO || seq != 7 || len != sizeof(tv)) {
        Serial.printf("broker self-check failed crc=%04x rc=%d\n", crc, rc);
    }
}
