/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_bplist.h"
#include "airplay_command_plist.h"
#include "airplay_hid_plist.h"
#include "input_bridge.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t report[INPUT_BRIDGE_HID_REPORT_BYTES];
    uint32_t hid_uid;
    size_t report_count;
} harness_t;

static bool send_report(uint32_t hid_uid,
                        const uint8_t *report,
                        size_t report_len,
                        void *context)
{
    harness_t *harness = context;
    uint8_t plist[256];
    size_t plist_len = 0U;
    airplay_bplist_doc_t doc;
    airplay_bplist_object_t root;
    airplay_bplist_object_t value;
    const uint8_t *encoded_report = NULL;
    size_t encoded_report_len = 0U;
    char uid[16];
    size_t uid_len = 0U;

    assert(hid_uid == INPUT_BRIDGE_HID_UID_TOUCH);
    assert(report_len == sizeof(harness->report));
    memcpy(harness->report, report, report_len);
    harness->hid_uid = hid_uid;
    ++harness->report_count;

    assert(airplay_hid_plist_encode(hid_uid, report, report_len,
                                    plist, sizeof(plist), &plist_len));
    assert(airplay_bplist_open(plist, plist_len, &doc) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, root, "uuid", &value) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_string(&doc, value, uid, sizeof(uid), &uid_len) ==
           AIRPLAY_BPLIST_OK);
    assert(uid_len == 8U && strcmp(uid, "2a2a2a2a") == 0);
    assert(airplay_bplist_dict_get(&doc, root, "hidReport", &value) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_data(&doc, value, &encoded_report,
                                   &encoded_report_len) == AIRPLAY_BPLIST_OK);
    assert(encoded_report_len == report_len);
    assert(memcmp(encoded_report, report, report_len) == 0);
    return true;
}

static uint16_t read_u16_le(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8U);
}

int main(void)
{
    uint8_t command_plist[96];
    size_t command_plist_len = 0U;
    airplay_bplist_doc_t command_doc;
    airplay_bplist_object_t command_root;
    airplay_bplist_object_t command_type;
    char command_type_text[32];
    size_t command_type_len = 0U;
    assert(airplay_command_plist_encode_force_keyframe(
        command_plist, sizeof(command_plist), &command_plist_len));
    assert(airplay_bplist_open(command_plist, command_plist_len,
                               &command_doc) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&command_doc, &command_root) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&command_doc, command_root, "type",
                                   &command_type) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_string(&command_doc, command_type,
                                     command_type_text, sizeof(command_type_text),
                                     &command_type_len) == AIRPLAY_BPLIST_OK);
    assert(command_type_len == sizeof("forceKeyFrame") - 1U);
    assert(strcmp(command_type_text, "forceKeyFrame") == 0);

    harness_t harness = {0};
    input_bridge_t *bridge = input_bridge_create(1920U, 1080U, 0U,
                                                  send_report, &harness);
    assert(bridge != NULL);

    assert(input_bridge_touch(bridge, INPUT_BRIDGE_TOUCH_DOWN, 73U,
                              0.5, 0.25, 0U) == INPUT_BRIDGE_OK);
    assert(harness.report_count == 1U);
    assert(harness.report[0] == 0U && harness.report[1] == 1U);
    assert(read_u16_le(&harness.report[2]) == 960U);
    assert(read_u16_le(&harness.report[4]) == 270U);

    assert(input_bridge_touch(bridge, INPUT_BRIDGE_TOUCH_MOVE, 73U,
                              1.0, 1.0, 30U) == INPUT_BRIDGE_OK);
    assert(harness.report_count == 2U);
    assert(harness.report[1] == 1U);
    assert(read_u16_le(&harness.report[2]) == 1920U);
    assert(read_u16_le(&harness.report[4]) == 1080U);

    assert(input_bridge_touch(bridge, INPUT_BRIDGE_TOUCH_UP, 73U,
                              1.0, 1.0, 60U) == INPUT_BRIDGE_OK);
    assert(harness.report_count == 3U);
    assert(harness.report[1] == 0U);
    assert(input_bridge_active_contacts(bridge) == 0U);
    assert(harness.hid_uid == INPUT_BRIDGE_HID_UID_TOUCH);

    input_bridge_destroy(bridge);
    puts("touch to HID plist bridge host test passed");
    return 0;
}
