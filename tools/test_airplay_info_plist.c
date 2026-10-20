#include "airplay_bplist.h"
#include "airplay_hid_descriptor.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    assert(size > 0);
    assert(fseek(file, 0, SEEK_SET) == 0);
    uint8_t *bytes = malloc((size_t)size);
    assert(bytes != NULL);
    assert(fread(bytes, 1U, (size_t)size, file) == (size_t)size);
    assert(fclose(file) == 0);
    *length = (size_t)size;
    return bytes;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    size_t plist_len = 0U;
    uint8_t *plist_bytes = read_file(argv[1], &plist_len);
    airplay_bplist_doc_t doc;
    assert(airplay_bplist_open(plist_bytes, plist_len, &doc) == AIRPLAY_BPLIST_OK);

    uint16_t width = 0U;
    uint16_t height = 0U;
    assert(airplay_bplist_get_main_display_size(&doc, &width, &height) == AIRPLAY_BPLIST_OK);
    assert(width == 800U && height == 480U);

    airplay_bplist_object_t root;
    airplay_bplist_object_t displays;
    airplay_bplist_object_t display;
    airplay_bplist_object_t hid_devices;
    airplay_bplist_object_t hid;
    airplay_bplist_object_t value;
    airplay_bplist_type_t type;
    uint64_t count = 0U;
    uint64_t numeric_value = 0U;
    char text[96];
    size_t text_len = 0U;
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, root, "displays", &displays) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_array_get(&doc, displays, 0U, &display) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, display, "uuid", &value) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_string(&doc, value, text, sizeof(text), &text_len) == AIRPLAY_BPLIST_OK);
    assert(strcmp(text, "b7e6c5a0-1111-4000-8000-000000000001") == 0);
    assert(airplay_bplist_dict_get(&doc, display, "features", &value) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_uint(&doc, value, &numeric_value) == AIRPLAY_BPLIST_OK);
    assert((numeric_value & 0x08U) != 0U);
    assert(airplay_bplist_dict_get(&doc, display, "primaryInputDevice", &value) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_uint(&doc, value, &numeric_value) == AIRPLAY_BPLIST_OK);
    assert(numeric_value == 1U);
    assert(airplay_bplist_dict_get(&doc, root, "hidDevices", &hid_devices) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_type(&doc, hid_devices, &type, &count) == AIRPLAY_BPLIST_OK);
    assert(type == AIRPLAY_BPLIST_ARRAY && count == 1U);
    assert(airplay_bplist_array_get(&doc, hid_devices, 0U, &hid) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, hid, "uuid", &value) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_string(&doc, value, text, sizeof(text), &text_len) == AIRPLAY_BPLIST_OK);
    assert(strcmp(text, "2a2a2a2a") == 0);
    assert(airplay_bplist_dict_get(&doc, hid, "hidDescriptor", &value) == AIRPLAY_BPLIST_OK);
    const uint8_t *descriptor = NULL;
    size_t descriptor_len = 0U;
    assert(airplay_bplist_get_data(&doc, value, &descriptor, &descriptor_len) == AIRPLAY_BPLIST_OK);
    uint8_t expected[AIRPLAY_HID_TOUCH_DESCRIPTOR_BYTES];
    size_t expected_len = 0U;
    assert(airplay_hid_touch_descriptor_build(width, height, expected, sizeof(expected), &expected_len));
    assert(descriptor_len == expected_len);
    assert(memcmp(descriptor, expected, expected_len) == 0);

    free(plist_bytes);
    puts("embedded AirPlay /info plist parses and matches the touch descriptor");
    return 0;
}
