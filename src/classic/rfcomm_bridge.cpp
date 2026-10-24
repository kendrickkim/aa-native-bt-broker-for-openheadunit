#include "rfcomm_bridge.h"

#include "aa_sdp.h"
#include "protocol/broker_protocol.h"
#include "broker/broker_runtime.h"

#include <Arduino.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"

static uint32_t s_handle;
static int s_open;
static int s_congested;
static uint32_t s_conn_gen;

/** SPP 서버 수명과 수신 바이트를 브로커 큐로 넘긴다. */
static void spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    switch (event) {
    case ESP_SPP_INIT_EVT:
        if (param->init.status != ESP_SPP_SUCCESS) {
            Serial.printf("SPP init failed: %d\n", param->init.status);
            return;
        }
        esp_spp_start_srv(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_SLAVE, 0, "AndroidAuto");
        break;

    case ESP_SPP_START_EVT:
        if (param->start.status != ESP_SPP_SUCCESS) {
            Serial.printf("SPP server failed: %d\n", param->start.status);
            return;
        }
        Serial.printf("SPP server on RFCOMM channel %d\n", param->start.scn);
        aa_sdp_publish((uint8_t)param->start.scn);
        broker_post(MSG_SPP_READY, nullptr, 0, nullptr, (uint8_t)param->start.scn, 0);
        break;

    case ESP_SPP_SRV_OPEN_EVT:
        if (param->srv_open.status != ESP_SPP_SUCCESS) {
            Serial.printf("phone RFCOMM open failed: %d\n", param->srv_open.status);
            return;
        }
        if (s_open && s_handle != 0 && s_handle != param->srv_open.handle) {
            /* Keep the first connection; a second open would overwrite and mix sessions. */
            Serial.printf("rejecting second RFCOMM handle %lu (active %lu)\n",
                          (unsigned long)param->srv_open.handle, (unsigned long)s_handle);
            esp_spp_disconnect(param->srv_open.handle);
            return;
        }
        s_handle = param->srv_open.handle;
        s_open = 1;
        s_congested = 0;
        s_conn_gen++;
        if (s_conn_gen == 0) {
            s_conn_gen = 1;
        }
        Serial.printf("phone RFCOMM connected, handle %lu gen %lu\n",
                      (unsigned long)s_handle, (unsigned long)s_conn_gen);
        broker_post(MSG_RFCOMM_OPEN, nullptr, 0, param->srv_open.rem_bda, 0, s_conn_gen);
        break;

    case ESP_SPP_CLOSE_EVT:
        if (s_handle != 0 && param->close.handle != s_handle) {
            Serial.printf("ignore RFCOMM close for stale handle %lu (active %lu)\n",
                          (unsigned long)param->close.handle, (unsigned long)s_handle);
            return;
        }
        Serial.println("phone RFCOMM closed");
        {
            uint32_t gen = s_conn_gen;
            s_open = 0;
            s_handle = 0;
            s_congested = 0;
            broker_post(MSG_RFCOMM_CLOSE, nullptr, 0, nullptr, 0, gen);
        }
        break;

    case ESP_SPP_DATA_IND_EVT: {
        if (!s_open || param->data_ind.handle != s_handle) {
            break;
        }
        const uint8_t *data = param->data_ind.data;
        size_t len = param->data_ind.len;
        size_t off = 0;
        while (off < len) {
            uint16_t chunk = (uint16_t)(len - off);
            if (chunk > BROKER_ATT_CHUNK) {
                chunk = BROKER_ATT_CHUNK;
            }
            if (broker_post(MSG_PHONE_RX, data + off, chunk, nullptr, 0, s_conn_gen) != 0) {
                /* Phone queue full — fail the stream rather than drop WPP bytes silently. */
                broker_post(MSG_RFCOMM_FAIL, nullptr, 0, nullptr, BROKER_ERR_OVERFLOW, s_conn_gen);
                break;
            }
            off += chunk;
        }
        break;
    }

    case ESP_SPP_CONG_EVT:
        if (s_handle != 0 && param->cong.handle != s_handle) {
            break;
        }
        s_congested = param->cong.cong ? 1 : 0;
        break;

    case ESP_SPP_WRITE_EVT:
        if (s_handle != 0 && param->write.handle != s_handle) {
            break;
        }
        if (param->write.status != ESP_SPP_SUCCESS) {
            Serial.printf("write to phone failed: %d\n", param->write.status);
            /* Uncertain delivery — do not retry the same bytes; end the stream. */
            broker_post(MSG_RFCOMM_FAIL, nullptr, 0, nullptr, BROKER_ERR_RFCOMM, s_conn_gen);
        }
        break;

    default:
        break;
    }
}

/** PIN과 페어링 결과를 처리한다. PIN은 0000이다. */
static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    uint8_t pin[4] = {'0', '0', '0', '0'};
    switch (event) {
    case ESP_BT_GAP_PIN_REQ_EVT:
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            Serial.printf("phone paired: %s\n", param->auth_cmpl.device_name);
            broker_post(MSG_PAIRED, nullptr, 0, param->auth_cmpl.bda, 1, 0);
        } else {
            Serial.printf("pairing failed: %d\n", param->auth_cmpl.stat);
        }
        break;
    default:
        break;
    }
}

void rfcomm_bridge_init(void)
{
    esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_cod_t cod = {};
    esp_spp_cfg_t spp_cfg = {};
    /* apply_spp: advertise as an AV accessory with AA RFCOMM, not a telephony HF device. */
    cod.major = ESP_BT_COD_MAJOR_DEV_AV;
    cod.minor = 0x08;
    cod.service = ESP_BT_COD_SRVC_AUDIO;
    spp_cfg.mode = ESP_SPP_MODE_CB;

    if (!btStart()) {
        Serial.println("Bluetooth controller did not start");
        return;
    }
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        esp_bluedroid_init();
    }
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
        esp_bluedroid_enable();
    }

    esp_bt_gap_set_device_name("OpenHU-NativeBTBroker");
    esp_bt_gap_set_cod(cod, ESP_BT_INIT_COD);
    esp_bt_gap_register_callback(gap_cb);
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 4, pin);

    /* apply_spp: do not register HFP — wake and transport are RFCOMM/SPP only. */

    if (esp_spp_register_callback(spp_cb) != ESP_OK) {
        Serial.println("SPP callback register failed");
        return;
    }
    if (esp_spp_enhanced_init(&spp_cfg) != ESP_OK) {
        Serial.println("SPP init failed");
    }
}

void rfcomm_set_discoverable(int discoverable)
{
    esp_bt_connection_mode_t conn = ESP_BT_CONNECTABLE;
    esp_bt_discovery_mode_t disc = discoverable ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE;
    esp_err_t err = esp_bt_gap_set_scan_mode(conn, disc);
    Serial.printf("classic discoverable=%d err=%d name=OpenHU-NativeBTBroker\n", discoverable, (int)err);
}

int rfcomm_is_open(void)
{
    return s_open;
}

int rfcomm_is_congested(void)
{
    return s_congested;
}

uint32_t rfcomm_conn_gen(void)
{
    return s_conn_gen;
}

void rfcomm_disconnect(void)
{
    if (!s_open || s_handle == 0) {
        return;
    }
    esp_spp_disconnect(s_handle);
}

void rfcomm_clear_bonds(void)
{
    int count = esp_bt_gap_get_bond_device_num();
    esp_bd_addr_t list[16];
    if (count <= 0) {
        Serial.println("no classic bonds to clear");
        return;
    }
    if (count > 16) {
        count = 16;
    }
    if (esp_bt_gap_get_bond_device_list(&count, list) != ESP_OK) {
        Serial.println("bond list read failed");
        return;
    }
    for (int i = 0; i < count; i++) {
        esp_err_t err = esp_bt_gap_remove_bond_device(list[i]);
        Serial.printf("removed bond %02X:%02X:%02X:%02X:%02X:%02X err=%d\n",
                      list[i][0], list[i][1], list[i][2], list[i][3], list[i][4], list[i][5],
                      (int)err);
    }
}

int rfcomm_send(const uint8_t *data, size_t len)
{
    size_t off = 0;
    if (!s_open || s_handle == 0 || data == nullptr || len == 0) {
        return -1;
    }
    if (s_congested) {
        return -1;
    }
    while (off < len) {
        size_t n = len - off;
        if (n > 990) {
            n = 990;
        }
        if (esp_spp_write(s_handle, (int)n, (uint8_t *)data + off) != ESP_OK) {
            return -1;
        }
        off += n;
    }
    return 0;
}
