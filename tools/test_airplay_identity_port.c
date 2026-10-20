#include "airplay_identity_port.h"
#include "airplay_bplist.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "test_airplay_identity_info.h"

static const airplay_mdns_txt_record_t k_txt_records[] = {
    {.key = "sf", .value = "0x00"},
};

static airplay_identity_config_t s_provider_config;

static esp_err_t get_config(airplay_identity_config_t *out_config, void *context)
{
    (void)context;
    *out_config = s_provider_config;
    return ESP_OK;
}

static const uint8_t *find_touch_descriptor(const uint8_t *plist,
                                           size_t plist_len,
                                           size_t *descriptor_len)
{
    airplay_bplist_doc_t doc;
    airplay_bplist_object_t root;
    airplay_bplist_object_t devices;
    airplay_bplist_object_t device;
    airplay_bplist_object_t descriptor_object;
    const uint8_t *descriptor = NULL;
    uint64_t count = 0U;
    airplay_bplist_type_t type;
    if (airplay_bplist_open(plist, plist_len, &doc) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_root(&doc, &root) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_dict_get(&doc, root, "hidDevices", &devices) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_get_type(&doc, devices, &type, &count) != AIRPLAY_BPLIST_OK ||
        type != AIRPLAY_BPLIST_ARRAY || count == 0U ||
        airplay_bplist_array_get(&doc, devices, 0U, &device) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_dict_get(&doc, device, "hidDescriptor", &descriptor_object) !=
            AIRPLAY_BPLIST_OK ||
        airplay_bplist_get_data(&doc, descriptor_object, &descriptor,
                                descriptor_len) != AIRPLAY_BPLIST_OK) {
        return NULL;
    }
    return descriptor;
}

int main(void)
{
    s_provider_config.rtsp_server.info_plist = k_touch_info_plist;
    s_provider_config.rtsp_server.info_plist_len = sizeof(k_touch_info_plist);
    s_provider_config.mdns_hostname = "relay.local";
    s_provider_config.mdns_instance_name = "CarPlay Relay";
    s_provider_config.mdns_txt_records = k_txt_records;
    s_provider_config.mdns_txt_record_count = sizeof(k_txt_records) / sizeof(k_txt_records[0]);
    s_provider_config.device_identifier = "00:11:22:33:44:55";
    s_provider_config.public_key = "test-only-public-key";
    s_provider_config.source_version = "test";
    s_provider_config.main_display_rotation_degrees = 270U;

    airplay_identity_provider_t provider = {
        .get_config = get_config,
        .context = NULL,
    };
    assert(airplay_identity_port_register_provider(&provider) == ESP_OK);
    assert(airplay_identity_port_is_configured());

    airplay_identity_config_t config;
    assert(airplay_identity_port_get_config(&config) == ESP_OK);
    assert(config.main_display_width_pixels == 1920U);
    assert(config.main_display_height_pixels == 1080U);
    assert(config.main_display_rotation_degrees == 270U);

    s_provider_config.rtsp_server.info_plist = k_display_only_info_plist;
    s_provider_config.rtsp_server.info_plist_len = sizeof(k_display_only_info_plist);
    assert(airplay_identity_port_get_config(&config) == ESP_ERR_INVALID_ARG);

    uint8_t mismatched_info[sizeof(k_touch_info_plist)];
    memcpy(mismatched_info, k_touch_info_plist, sizeof(mismatched_info));
    size_t descriptor_len = 0U;
    const uint8_t *descriptor = find_touch_descriptor(
        mismatched_info, sizeof(mismatched_info), &descriptor_len);
    assert(descriptor != NULL && descriptor_len == 109U);
    mismatched_info[(size_t)(descriptor - mismatched_info)] ^= 0x01U;
    s_provider_config.rtsp_server.info_plist = mismatched_info;
    s_provider_config.rtsp_server.info_plist_len = sizeof(mismatched_info);
    assert(airplay_identity_port_get_config(&config) == ESP_ERR_INVALID_ARG);

    s_provider_config.rtsp_server.info_plist = k_touch_info_plist;
    s_provider_config.rtsp_server.info_plist_len = sizeof(k_touch_info_plist);

    s_provider_config.main_display_rotation_degrees = 45U;
    assert(airplay_identity_port_get_config(&config) == ESP_ERR_INVALID_ARG);
    assert(config.main_display_width_pixels == 0U);
    assert(config.main_display_height_pixels == 0U);
    assert(airplay_identity_port_register_provider(&provider) == ESP_ERR_INVALID_STATE);

    puts("airplay identity port host test passed");
    return 0;
}
