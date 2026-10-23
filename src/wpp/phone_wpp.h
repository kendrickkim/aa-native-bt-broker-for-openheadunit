#pragma once

#include <stdint.h>

/** RFCOMM이 열리면 Wi-Fi 스냅샷으로 WPP 버전 교환을 시작한다. */
void phone_wpp_on_open(const uint8_t *net, uint16_t net_len);
/** RFCOMM이 닫히면 수신 버퍼와 이번 세션 단계를 버린다. */
void phone_wpp_on_close(void);
/** 폰에서 받은 RFCOMM 바이트를 WPP 프레임으로 모아 처리한다. */
void phone_wpp_on_rx(const uint8_t *data, uint16_t len);
/** 시작 응답 대기와 프로젝션 생존 시간을 확인한다. */
void phone_wpp_poll(void);
/** 헤드유닛의 프로젝션 소켓이 살아 있는지 반영한다. */
void phone_wpp_set_projection(int up);
/** 헤드유닛이 새 Android Auto 시작을 요청했다. 이전 시작 금지를 푼다. */
void phone_wpp_allow_start(void);
