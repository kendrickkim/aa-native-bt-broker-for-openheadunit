#include "aa_sdp.h"

#include "protocol/broker_protocol.h"

#include <Arduino.h>

extern "C" {
uint32_t SDP_CreateRecord(void);
uint8_t SDP_AddSequence(uint32_t handle, uint16_t attr_id, uint16_t num_elem,
                        uint8_t type[], uint8_t len[], uint8_t *p_val[]);
uint8_t SDP_AddProtocolList(uint32_t handle, uint16_t num_elem, void *elem_list);
uint8_t SDP_AddUuidSequence(uint32_t handle, uint16_t attr_id, uint16_t num_uuids, uint16_t *uuids);
uint8_t SDP_AddAttribute(uint32_t handle, uint16_t attr_id, uint8_t attr_type, uint32_t attr_len, uint8_t *val);
}

#define ATTR_SERVICE_CLASS 0x0001
#define ATTR_BROWSE_GROUP 0x0005
#define ATTR_SERVICE_NAME 0x0100
#define UUID_DESC_TYPE 3
#define TEXT_DESC_TYPE 4
#define UUID_PROTO_L2CAP 0x0100
#define UUID_PROTO_RFCOMM 0x0003
#define UUID_PUBLIC_BROWSE 0x1002

typedef struct {
    uint16_t protocol_uuid;
    uint16_t num_params;
    uint16_t params[2];
} aa_proto_elem_t;

/* 4de17a00-52cb-11e6-bdf4-0800200c9a66, SDP 바이트 순서. */
static uint8_t s_uuid[16] = {
    0x4d, 0xe1, 0x7a, 0x00, 0x52, 0xcb, 0x11, 0xe6,
    0xbd, 0xf4, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66
};

void aa_sdp_publish(uint8_t rfcomm_scn)
{
    uint8_t types[1] = {UUID_DESC_TYPE};
    uint8_t lens[1] = {16};
    uint8_t *vals[1] = {s_uuid};
    uint16_t browse = UUID_PUBLIC_BROWSE;
    aa_proto_elem_t proto[2] = {};
    const char *name = "Android Auto";
    uint32_t handle = SDP_CreateRecord();

    if (handle == 0) {
        Serial.println("SDP_CreateRecord failed");
        return;
    }

    proto[0].protocol_uuid = UUID_PROTO_L2CAP;
    proto[1].protocol_uuid = UUID_PROTO_RFCOMM;
    proto[1].num_params = 1;
    proto[1].params[0] = rfcomm_scn;

    if (!SDP_AddSequence(handle, ATTR_SERVICE_CLASS, 1, types, lens, vals) ||
        !SDP_AddProtocolList(handle, 2, proto) ||
        !SDP_AddUuidSequence(handle, ATTR_BROWSE_GROUP, 1, &browse) ||
        !SDP_AddAttribute(handle, ATTR_SERVICE_NAME, TEXT_DESC_TYPE, 12, (uint8_t *)name)) {
        Serial.println("failed to fill the Android Auto SDP record");
        return;
    }

    Serial.printf("Android Auto RFCOMM UUID %s is on channel %u\n", AA_RFCOMM_UUID, rfcomm_scn);
}
