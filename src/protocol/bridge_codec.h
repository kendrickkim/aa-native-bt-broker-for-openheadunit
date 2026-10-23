#pragma once

#include "broker_protocol.h"

#include <stddef.h>
#include <stdint.h>

/** CRC-16/CCITT-FALSE. 초기값 0xFFFF, 다항식 0x1021. */
uint16_t broker_crc16(const uint8_t *data, size_t len);

/** 논리 프레임을 만든다. 들어가지 않으면 0. */
uint16_t broker_encode(uint8_t type, uint8_t flags, uint8_t channel, uint16_t seq,
                       const uint8_t *payload, uint16_t len,
                       uint8_t *out, uint16_t out_cap);

/** 논리 프레임을 검사하고 필드를 채운다. 성공하면 0, 실패하면 BROKER_ERR_*. */
int broker_decode(const uint8_t *frame, uint16_t frame_len,
                  uint8_t *type, uint8_t *flags, uint8_t *channel, uint16_t *seq,
                  const uint8_t **payload, uint16_t *payload_len);

struct BrokerFrag {
    uint16_t seq;
    uint8_t expect;
    uint8_t count;
    uint16_t len;
    uint8_t active;
    uint8_t buf[BROKER_HEADER_LEN + BROKER_MAX_PAYLOAD];
};

/** 조각 조립 상태를 비운다. */
void broker_frag_reset(BrokerFrag *frag);

/** BLE 조각 하나를 붙인다. 프레임이 완성되면 1, 더 필요하면 0, 오류면 음수 오류 코드. */
int broker_frag_push(BrokerFrag *frag, const uint8_t *piece, uint16_t piece_len);
