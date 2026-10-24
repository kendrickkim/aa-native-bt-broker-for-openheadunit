#pragma once

#include <stdint.h>

/* Project 5 wire contract. Like FYT BLINK/DUDUAUTO: ESP32 owns Classic RFCOMM,
 * and relays phone Android Auto bytes to the head unit over BLE. The head unit owns
 * WPP framing; ESP32 does not decode Android Auto.
 *
 * apply_spp policy: no HFP. Pairing is Classic bond only; Android Auto starts when the
 * phone opens the AA RFCOMM UUID. CMD_AA_ENABLE arms the RFCOMM relay (and may open a
 * short discoverable window); it does not connect hands-free.
 *
 * Logical frame (little-endian), 12-byte header + payload:
 *   0..1  magic 0xA5 0x5A
 *   2     version 0x01
 *   3     type
 *   4     flags: bit0 START, bit1 END, bit2 ACK_REQUIRED
 *   5     channel: 0 control, 1 RFCOMM
 *   6     seq uint16
 *   8     payload_len uint16
 *   10    crc16 of bytes [0..9] + payload, CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF)
 * A complete control message uses START|END.
 *
 * BLE write (head unit → broker) / indicate (broker → head unit) pieces:
 *   uint16 transport_seq, uint8 frag_index, uint8 frag_count, then that slice.
 * Concatenate slices in index order to recover one logical frame.
 *
 * BLINK mapping:
 *   EVT_PHONE_LINK  ≈ JH (hands-free up)
 *   EVT_RFCOMM_OPEN ≈ AA (phone opened Android Auto; head unit speaks first)
 *   EVT_RFCOMM_CLOSE≈ SS
 *   DATA_RFCOMM_RX  ≈ ZB  (phone → head unit)
 *   DATA_RFCOMM_TX  ≈ AT#ZA (head unit → phone)
 * EVT_CREDIT tops up how many DATA_RFCOMM_RX pieces may be indicated.
 *
 * CMD_SET_NETWORK payload, schema 0x01:
 *   u8 schema, u32 generation, u16 tcpPort,
 *   u8 ssidLen, ssid, u8 passLen, pass, u8 bssid[6],
 *   u8 securityLen, security, u8 ipLen, ip.
 * A payload that starts with '{' is stored as JSON and is not parsed.
 */

#define BROKER_MAGIC0 0xA5
#define BROKER_MAGIC1 0x5A
#define BROKER_VERSION 0x01
#define BROKER_HEADER_LEN 12
#define BROKER_MAX_PAYLOAD 4096

#define BROKER_FLAG_START 0x01
#define BROKER_FLAG_END 0x02
#define BROKER_FLAG_ACK 0x04

#define BROKER_CH_CONTROL 0
#define BROKER_CH_RFCOMM 1

#define CMD_HELLO 0x01
#define CMD_SET_NETWORK 0x02
#define CMD_AA_ENABLE 0x03
#define CMD_AA_DISABLE 0x04
#define CMD_GET_STATUS 0x05
#define CMD_SET_BOND_MODE 0x06
#define CMD_PROJECTION_UP 0x07

/* CMD_SET_BOND_MODE payload:
 *   u8 mode: 0 = close pairing window,
 *            1 = open Classic discoverable window (keep bonds),
 *            2 = drop phone bonds/RFCOMM then open pairing for a new phone,
 *   u16 seconds little-endian (0 means 120).
 */

#define EVT_HELLO_ACK 0x20
#define EVT_BT_READY 0x21
#define EVT_PHONE_LINK 0x22
#define EVT_HFP_STATE 0x23
#define EVT_RFCOMM_OPEN 0x24
#define EVT_RFCOMM_CLOSE 0x25
#define EVT_ERROR 0x26
#define EVT_STATUS 0x27
#define DATA_RFCOMM_TX 0x40
#define DATA_RFCOMM_RX 0x41
#define EVT_CREDIT 0x42

#define BROKER_ERR_RFCOMM 1
#define BROKER_ERR_TOO_BIG 2
#define BROKER_ERR_CRC 3
#define BROKER_ERR_OVERFLOW 4
#define BROKER_ERR_VERSION 5
#define BROKER_ERR_NO_NETWORK 6
#define BROKER_ERR_FRAME 7

#define BROKER_BLE_SERVICE "8e6d1000-3f45-4f1e-a000-6e2b8d012345"
#define BROKER_BLE_CMD_RX "8e6d1001-3f45-4f1e-a000-6e2b8d012345"
#define BROKER_BLE_EVT_TX "8e6d1002-3f45-4f1e-a000-6e2b8d012345"
#define BROKER_BLE_DATA_RX "8e6d1003-3f45-4f1e-a000-6e2b8d012345"
#define BROKER_BLE_DATA_TX "8e6d1004-3f45-4f1e-a000-6e2b8d012345"

#define AA_RFCOMM_UUID "4de17a00-52cb-11e6-bdf4-0800200c9a66"

#define BROKER_ATT_CHUNK 512
