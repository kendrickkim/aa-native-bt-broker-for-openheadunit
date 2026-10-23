#include "bridge_codec.h"

#include "broker_protocol.h"

#include <cstring>

/** CRC에 바이트열을 이어서 반영한다. */
static uint16_t crc_feed(uint16_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/** 리틀엔디언 16비트를 읽는다. */
static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/** 리틀엔디언 16비트를 쓴다. */
static void put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xff);
    p[1] = (uint8_t)(value >> 8);
}

uint16_t broker_crc16(const uint8_t *data, size_t len)
{
    return crc_feed(0xFFFF, data, len);
}

uint16_t broker_encode(uint8_t type, uint8_t flags, uint8_t channel, uint16_t seq,
                       const uint8_t *payload, uint16_t len,
                       uint8_t *out, uint16_t out_cap)
{
    if (len > BROKER_MAX_PAYLOAD || (uint32_t)BROKER_HEADER_LEN + len > out_cap) {
        return 0;
    }
    out[0] = BROKER_MAGIC0;
    out[1] = BROKER_MAGIC1;
    out[2] = BROKER_VERSION;
    out[3] = type;
    out[4] = flags;
    out[5] = channel;
    put_u16(out + 6, seq);
    put_u16(out + 8, len);
    if (len > 0 && payload != nullptr) {
        memcpy(out + BROKER_HEADER_LEN, payload, len);
    }
    /* CRC covers header[0..9] and the payload. Bytes 10..11 are filled afterwards. */
    uint16_t crc = crc_feed(0xFFFF, out, 10);
    if (len > 0) {
        crc = crc_feed(crc, out + BROKER_HEADER_LEN, len);
    }
    put_u16(out + 10, crc);
    return (uint16_t)(BROKER_HEADER_LEN + len);
}

int broker_decode(const uint8_t *frame, uint16_t frame_len,
                  uint8_t *type, uint8_t *flags, uint8_t *channel, uint16_t *seq,
                  const uint8_t **payload, uint16_t *payload_len)
{
    if (frame == nullptr || frame_len < BROKER_HEADER_LEN) {
        return BROKER_ERR_FRAME;
    }
    if (frame[0] != BROKER_MAGIC0 || frame[1] != BROKER_MAGIC1) {
        return BROKER_ERR_FRAME;
    }
    if (frame[2] != BROKER_VERSION) {
        return BROKER_ERR_VERSION;
    }
    uint16_t len = get_u16(frame + 8);
    if ((uint32_t)BROKER_HEADER_LEN + len != frame_len || len > BROKER_MAX_PAYLOAD) {
        return BROKER_ERR_TOO_BIG;
    }
    uint16_t expect = crc_feed(0xFFFF, frame, 10);
    if (len > 0) {
        expect = crc_feed(expect, frame + BROKER_HEADER_LEN, len);
    }
    uint16_t got = get_u16(frame + 10);
    if (got != expect) {
        return BROKER_ERR_CRC;
    }
    *type = frame[3];
    *flags = frame[4];
    *channel = frame[5];
    *seq = get_u16(frame + 6);
    *payload_len = len;
    *payload = len ? frame + BROKER_HEADER_LEN : nullptr;
    return 0;
}

void broker_frag_reset(BrokerFrag *frag)
{
    frag->active = 0;
    frag->expect = 0;
    frag->count = 0;
    frag->len = 0;
    frag->seq = 0;
}

int broker_frag_push(BrokerFrag *frag, const uint8_t *piece, uint16_t piece_len)
{
    if (piece == nullptr || piece_len < 4) {
        return -BROKER_ERR_FRAME;
    }
    uint16_t seq = get_u16(piece);
    uint8_t index = piece[2];
    uint8_t count = piece[3];
    const uint8_t *body = piece + 4;
    uint16_t body_len = (uint16_t)(piece_len - 4);
    if (count == 0 || index >= count) {
        return -BROKER_ERR_FRAME;
    }
    if (!frag->active || frag->seq != seq || index == 0) {
        broker_frag_reset(frag);
        frag->active = 1;
        frag->seq = seq;
        frag->count = count;
        frag->expect = 0;
    }
    if (index != frag->expect || count != frag->count) {
        broker_frag_reset(frag);
        return -BROKER_ERR_FRAME;
    }
    if ((uint32_t)frag->len + body_len > sizeof(frag->buf)) {
        broker_frag_reset(frag);
        return -BROKER_ERR_TOO_BIG;
    }
    if (body_len > 0) {
        memcpy(frag->buf + frag->len, body, body_len);
        frag->len = (uint16_t)(frag->len + body_len);
    }
    frag->expect++;
    if (frag->expect == frag->count) {
        frag->active = 0;
        return 1;
    }
    return 0;
}
