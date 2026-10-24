#pragma once

#include <stdint.h>

#define MSG_GATT_CMD 1
#define MSG_GATT_DATA 2
#define MSG_PHONE_RX 3
#define MSG_HFP 4
#define MSG_RFCOMM_OPEN 5
#define MSG_RFCOMM_CLOSE 6
#define MSG_SPP_READY 7
#define MSG_PAIRED 8
/** Phone RX queue overflow or RFCOMM write failure — fail the active stream. */
#define MSG_RFCOMM_FAIL 9

/** 큐와 BLE 조각 버퍼를 준비한다. */
void broker_runtime_init(void);
/** 대기 중인 블루투스 이벤트를 처리하고 시간 제한을 확인한다. */
void broker_poll(void);
/** 코덱이 알려진 CRC와 왕복 결과를 내는지 확인하고, 실패하면 시리얼에 남긴다. */
void broker_self_check(void);
/** 헤드유닛 BLE 연결이 올라왔음을 기록한다. */
void broker_ble_up(void);
/** BLE가 끊기면 조각 버퍼를 버리고, 이후 재연결 대기를 시작한다. */
void broker_ble_down(void);
/**
 * 블루투스 콜백에서 받은 이벤트를 루프 큐에 넣는다. 큐가 가득 차면 -1.
 * [gen]은 RFCOMM 연결 세대. 제어 이벤트는 0을 쓴다.
 */
int broker_post(uint8_t kind, const uint8_t *data, uint16_t len, const uint8_t bda[6],
                uint8_t extra, uint32_t gen);
