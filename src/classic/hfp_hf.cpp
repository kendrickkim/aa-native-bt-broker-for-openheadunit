#include "hfp_hf.h"

#include "broker/broker_runtime.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cstring>

#include "esp_hf_client_api.h"
#include "esp_gap_bt_api.h"

static const char *NVS_NS = "broker";
static const char *NVS_LAST = "lastbda";

static uint8_t s_state;
static uint8_t s_peer[6];
static int s_peer_valid;

/** SCO 음성은 브로커가 중계하지 않는다. 콜백만 등록해 둔다. */
static void hf_recv(const uint8_t *buf, uint32_t len)
{
    (void)buf;
    (void)len;
}

/** 폰으로 보낼 음성도 없다. */
static uint32_t hf_send(uint8_t *buf, uint32_t len)
{
    (void)buf;
    (void)len;
    return 0;
}

/** 연결 상태만 브로커로 넘긴다. 오디오 상태는 로그만 남긴다. */
static void hf_cb(esp_hf_client_cb_event_t event, esp_hf_client_cb_param_t *param)
{
    if (event == ESP_HF_CLIENT_CONNECTION_STATE_EVT && param != nullptr) {
        s_state = (uint8_t)param->conn_stat.state;
        Serial.printf("HFP HF state %u\n", s_state);
        if (s_state == ESP_HF_CLIENT_CONNECTION_STATE_CONNECTED ||
            s_state == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED) {
            memcpy(s_peer, param->conn_stat.remote_bda, 6);
            s_peer_valid = 1;
        } else if (s_state == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED) {
            s_peer_valid = 0;
        }
        broker_post(MSG_HFP, nullptr, 0, param->conn_stat.remote_bda, s_state, 0);
    } else if (event == ESP_HF_CLIENT_AUDIO_STATE_EVT && param != nullptr) {
        Serial.printf("HFP audio state %u\n", (unsigned)param->audio_stat.state);
    }
}

void hfp_hf_init(void)
{
    if (esp_hf_client_register_callback(hf_cb) != ESP_OK) {
        Serial.println("HFP HF callback failed");
        return;
    }
    esp_hf_client_register_data_callback(hf_recv, hf_send);
    if (esp_hf_client_init() != ESP_OK) {
        Serial.println("HFP HF init failed");
        return;
    }
    Serial.println("HFP HF registered");
}

void hfp_hf_connect(const uint8_t bda[6])
{
    if (bda == nullptr) {
        return;
    }
    esp_hf_client_connect((uint8_t *)bda);
}

static int phone_is_bonded(const uint8_t bda[6])
{
    int count = esp_bt_gap_get_bond_device_num();
    esp_bd_addr_t list[8];
    if (count <= 0) {
        return 0;
    }
    if (count > 8) {
        count = 8;
    }
    if (esp_bt_gap_get_bond_device_list(&count, list) != ESP_OK) {
        return 0;
    }
    for (int i = 0; i < count; i++) {
        if (memcmp(list[i], bda, 6) == 0) {
            return 1;
        }
    }
    return 0;
}

void hfp_hf_remember(const uint8_t bda[6])
{
    if (bda == nullptr) {
        return;
    }
    if (!phone_is_bonded(bda)) {
        Serial.println("phone is not paired yet; not remembering");
        return;
    }
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.putBytes(NVS_LAST, bda, 6);
    prefs.end();
    Serial.printf("remembered phone %02X:%02X:%02X:%02X:%02X:%02X\n",
                  bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

void hfp_hf_forget_last(void)
{
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.remove(NVS_LAST);
    prefs.end();
    Serial.println("forgot last phone");
}

void hfp_hf_connect_last(void)
{
    uint8_t bda[6];
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    size_t n = prefs.getBytes(NVS_LAST, bda, sizeof(bda));
    prefs.end();
    if (n != sizeof(bda)) {
        Serial.println("no last phone to reconnect");
        return;
    }
    int count = esp_bt_gap_get_bond_device_num();
    if (count <= 0) {
        Serial.println("last phone is not bonded");
        return;
    }
    if (count > 8) {
        count = 8;
    }
    esp_bd_addr_t list[8];
    if (esp_bt_gap_get_bond_device_list(&count, list) != ESP_OK) {
        return;
    }
    for (int i = 0; i < count; i++) {
        if (memcmp(list[i], bda, 6) == 0) {
            Serial.printf("reconnecting last phone %02X:%02X:%02X:%02X:%02X:%02X\n",
                          bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
            hfp_hf_connect(bda);
            return;
        }
    }
    Serial.println("last phone is no longer bonded");
}

void hfp_hf_connect_bonded(void)
{
    int count = esp_bt_gap_get_bond_device_num();
    if (count <= 0) {
        Serial.println("no classic bond for HFP");
        return;
    }
    if (count > 4) {
        count = 4;
    }
    esp_bd_addr_t list[4];
    if (esp_bt_gap_get_bond_device_list(&count, list) != ESP_OK) {
        return;
    }
    for (int i = 0; i < count; i++) {
        hfp_hf_connect(list[i]);
    }
}

void hfp_hf_disconnect(void)
{
    uint8_t bda[6];
    if (s_peer_valid) {
        memcpy(bda, s_peer, 6);
    } else {
        Preferences prefs;
        prefs.begin(NVS_NS, true);
        size_t n = prefs.getBytes(NVS_LAST, bda, sizeof(bda));
        prefs.end();
        if (n != sizeof(bda)) {
            Serial.println("HFP disconnect: no peer");
            return;
        }
    }
    Serial.printf("HFP disconnect %02X:%02X:%02X:%02X:%02X:%02X\n",
                  bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    esp_hf_client_disconnect(bda);
}

uint8_t hfp_hf_state(void)
{
    return s_state;
}

int hfp_hf_is_up(void)
{
    return s_state == ESP_HF_CLIENT_CONNECTION_STATE_CONNECTED ||
           s_state == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED;
}
