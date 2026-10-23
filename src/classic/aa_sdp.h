#pragma once

#include <stdint.h>

/** Android Auto RFCOMM UUID를 주어진 채널 번호로 SDP에 게시한다. */
void aa_sdp_publish(uint8_t rfcomm_scn);
