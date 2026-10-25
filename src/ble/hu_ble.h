#pragma once

#include <stdint.h>

/** GATT 서버를 열고 OpenHU-NativeBTBroker-Setter로 광고를 시작한다. */
void hu_ble_start(void);
/** 끊긴 뒤 광고 재시작, 멈춘 세션 복구. broker_poll에서 호출한다. */
void hu_ble_poll(void);
/** 협상된 ATT MTU. 아직 없으면 23. */
uint16_t hu_ble_mtu(void);
/** 헤드유닛이 이벤트 indicate/notify를 구독 중이면 1. */
int hu_ble_event_ready(void);
/** 헤드유닛이 데이터 indicate/notify를 구독 중이면 1. */
int hu_ble_data_ready(void);
/** 제어 이벤트 조각 하나를 indicate한다. 구독 전이면 버린다. */
void hu_ble_notify_event(const uint8_t *packet, uint16_t len);
/** RFCOMM 데이터 조각 하나를 indicate한다. 구독 전이면 버린다. */
void hu_ble_notify_data(const uint8_t *packet, uint16_t len);
