#pragma once

#include <stdint.h>

/** HFP Hands-Free 역할을 등록한다. 음성 PCM은 받지 않는다. */
void hfp_hf_init(void);
/** 지정한 폰 주소로 HFP를 연결한다. */
void hfp_hf_connect(const uint8_t bda[6]);
/** 저장된 클래식 본드마다 HFP 연결을 시도한다. */
void hfp_hf_connect_bonded(void);
/** 페어링으로 본드에 등록된 폰만 다음 부팅의 재연결 대상으로 남긴다. */
void hfp_hf_remember(const uint8_t bda[6]);
/** 마지막으로 연결됐던 폰이 아직 본드에 있으면 그 폰으로 HFP를 연결한다. */
void hfp_hf_connect_last(void);
/** 저장된 마지막 폰 주소를 지운다. */
void hfp_hf_forget_last(void);
/** 마지막 HFP 연결 상태 값. */
uint8_t hfp_hf_state(void);
/** HFP가 붙어 있거나 서비스 레벨까지 올라왔으면 1. */
int hfp_hf_is_up(void);
