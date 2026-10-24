#include "hu_ble.h"

#include "protocol/broker_protocol.h"
#include "broker/broker_runtime.h"

#include <Arduino.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLESecurity.h>
#include <BLEServer.h>
#include <BLEUtils.h>

static BLECharacteristic *s_evt;
static BLECharacteristic *s_data_tx;
static bool s_evt_on;
static bool s_data_on;
static uint16_t s_mtu = 23;

/** Just Works 본딩. 입출력 장치가 없으므로 PIN은 확인만 한다. */
class BrokerSecurity : public BLESecurityCallbacks {
    uint32_t onPassKeyRequest() override { return 0; }
    void onPassKeyNotify(uint32_t pass_key) override { (void)pass_key; }
    bool onSecurityRequest() override { return true; }
    void onAuthenticationComplete(esp_ble_auth_cmpl_t result) override
    {
        Serial.printf("BLE bond %s\n", result.success ? "ok" : "failed");
    }
    bool onConfirmPIN(uint32_t pin) override
    {
        (void)pin;
        return true;
    }
};

/** 연결과 MTU를 기록하고, 끊기면 광고를 다시 시작한다. */
class BrokerServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *server) override
    {
        (void)server;
        broker_ble_up();
        Serial.println("head unit BLE connected");
    }

    void onDisconnect(BLEServer *server) override
    {
        (void)server;
        s_evt_on = false;
        s_data_on = false;
        s_mtu = 23;
        broker_ble_down();
        Serial.println("head unit BLE disconnected");
        BLEDevice::startAdvertising();
    }

    void onMtuChanged(BLEServer *server, esp_ble_gatts_cb_param_t *param) override
    {
        (void)server;
        if (param != nullptr && param->mtu.mtu >= 23) {
            s_mtu = param->mtu.mtu;
            Serial.printf("BLE MTU %u\n", s_mtu);
        }
    }
};

/** 암호화된 write를 브로커 큐로 넘긴다. kind로 명령과 데이터를 구분한다. */
class WriteCallbacks : public BLECharacteristicCallbacks {
    uint8_t kind;
public:
    explicit WriteCallbacks(uint8_t message) : kind(message) {}
    void onWrite(BLECharacteristic *characteristic) override
    {
        const uint8_t *data = characteristic->getData();
        size_t len = characteristic->getLength();
        if (data != nullptr && len > 0 && len <= BROKER_ATT_CHUNK) {
            broker_post(kind, data, (uint16_t)len, nullptr, 0, 0);
        }
    }
};

/** CCCD 쓰기로 indicate(또는 notify) 구독 여부를 해당 특성 플래그에 반영한다. */
class IndicateCallbacks : public BLEDescriptorCallbacks {
    bool *flag;
public:
    explicit IndicateCallbacks(bool *target) : flag(target) {}
    void onWrite(BLEDescriptor *descriptor) override
    {
        uint8_t *value = descriptor->getValue();
        /* bit0 = notify, bit1 = indicate. 헤드유닛은 indicate를 켠다. */
        *flag = value != nullptr && ((value[0] & 0x01) || (value[0] & 0x02));
    }
};

/** indicate 특성 하나와 구독 플래그를 서비스에 추가한다. */
static void add_indicate(BLEService *service, const char *uuid, BLECharacteristic **out, bool *flag)
{
    BLE2902 *ccc = new BLE2902();
    ccc->setCallbacks(new IndicateCallbacks(flag));
    *out = service->createCharacteristic(
        uuid, BLECharacteristic::PROPERTY_INDICATE | BLECharacteristic::PROPERTY_NOTIFY);
    (*out)->addDescriptor(ccc);
}

void hu_ble_start(void)
{
    BLEDevice::init("OpenHU-NativeBTBroker-Setter");
    BLEDevice::setMTU(247);
    BLEDevice::setSecurityCallbacks(new BrokerSecurity());
    BLESecurity security;
    security.setCapability(ESP_IO_CAP_NONE);
    security.setAuthenticationMode(ESP_LE_AUTH_BOND);
    security.setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    security.setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

    BLEServer *server = BLEDevice::createServer();
    server->setCallbacks(new BrokerServerCallbacks());
    BLEService *service = server->createService(BROKER_BLE_SERVICE);

    BLECharacteristic *cmd = service->createCharacteristic(
        BROKER_BLE_CMD_RX, BLECharacteristic::PROPERTY_WRITE);
    cmd->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
    cmd->setCallbacks(new WriteCallbacks(MSG_GATT_CMD));

    BLECharacteristic *data_rx = service->createCharacteristic(
        BROKER_BLE_DATA_RX, BLECharacteristic::PROPERTY_WRITE_NR);
    data_rx->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
    data_rx->setCallbacks(new WriteCallbacks(MSG_GATT_DATA));

    add_indicate(service, BROKER_BLE_EVT_TX, &s_evt, &s_evt_on);
    add_indicate(service, BROKER_BLE_DATA_TX, &s_data_tx, &s_data_on);

    service->start();
    BLEAdvertising *advertising = BLEDevice::getAdvertising();
    advertising->addServiceUUID(BROKER_BLE_SERVICE);
    advertising->start();
    Serial.println("advertising OpenHU-NativeBTBroker-Setter");
}

uint16_t hu_ble_mtu(void)
{
    return s_mtu;
}

int hu_ble_event_ready(void)
{
    return s_evt_on && s_evt != nullptr;
}

int hu_ble_data_ready(void)
{
    return s_data_on && s_data_tx != nullptr;
}

/** 구독 중인 특성으로 조각 하나를 indicate한다. 확인 응답이 있어 RFCOMM 중계에 맞다. */
static void indicate_packet(BLECharacteristic *characteristic, int ready, const uint8_t *packet, uint16_t len)
{
    if (!ready || characteristic == nullptr || packet == nullptr || len == 0) {
        return;
    }
    characteristic->setValue((uint8_t *)packet, len);
    characteristic->indicate();
}

void hu_ble_notify_event(const uint8_t *packet, uint16_t len)
{
    indicate_packet(s_evt, hu_ble_event_ready(), packet, len);
}

void hu_ble_notify_data(const uint8_t *packet, uint16_t len)
{
    indicate_packet(s_data_tx, hu_ble_data_ready(), packet, len);
}
