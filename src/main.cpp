#include "broker/broker_runtime.h"
#include "ble/hu_ble.h"
#include "classic/rfcomm_bridge.h"

#include <Arduino.h>
#include <WiFi.h>

/** 시리얼, 브로커, 클래식 블루투스, BLE 광고를 차례로 시작한다. */
void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println("OpenHU-BTBroker");
    broker_self_check();
    WiFi.mode(WIFI_OFF);

    broker_runtime_init();
    rfcomm_bridge_init();
    hu_ble_start();
    Serial.println("Classic name OpenHU-BTBroker, BLE name OpenHU-BTBroker-Setter, HFP role HF");
}

/** 브로커 큐를 비우고 WPP 시간 제한을 확인한다. */
void loop()
{
    broker_poll();
    delay(2);
}
