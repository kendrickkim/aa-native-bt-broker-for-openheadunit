#pragma once

#include <stddef.h>
#include <stdint.h>

/** 클래식 블루투스와 Android Auto RFCOMM 서버를 시작한다. */
void rfcomm_bridge_init(void);
/** 검색 가능 여부를 바꾼다. 연결은 항상 허용한다. */
void rfcomm_set_discoverable(int discoverable);
/** 폰 RFCOMM이 열려 있으면 1. */
int rfcomm_is_open(void);
/** RFCOMM 송신 경로가 혼잡하면 1. */
int rfcomm_is_congested(void);
/** 현재 RFCOMM 연결 세대. OPEN마다 증가한다. */
uint32_t rfcomm_conn_gen(void);
/** 열린 RFCOMM을 끊는다. 이미 닫혀 있으면 아무 일도 하지 않는다. */
void rfcomm_disconnect(void);
/** 클래식 본드 목록을 모두 지운다. 새 폰 페어링 전에 호출한다. */
void rfcomm_clear_bonds(void);
/** 폰으로 바이트를 쓴다. 혼잡하거나 닫혀 있으면 -1. */
int rfcomm_send(const uint8_t *data, size_t len);
