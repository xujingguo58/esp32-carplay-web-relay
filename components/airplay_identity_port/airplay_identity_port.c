/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Identity, pairing operations, and the /info capability document are supplied by the
 * authorized deployment. This port never embeds or synthesizes credentials.
 */
#include "airplay_identity_port.h"

#include <string.h>

#include "airplay_hid_descriptor.h"
#include "airplay_bplist.h"

#define AIRPLAY_DISPLAY_FEATURE_HIGH_FIDELITY_TOUCH 0x08U
#define AIRPLAY_PRIMARY_TOUCH_DEVICE 1U
#define AIRPLAY_INFO_MAX_HID_DEVICES 32U

static airplay_bplist_result_t get_main_display_identity(
    const airplay_bplist_doc_t *doc,
    uint16_t *display_width,
    uint16_t *display_height,
    char *display_uuid,
    size_t display_uuid_capacity)
{
    airplay_bplist_object_t root;
    airplay_bplist_object_t displays;
    airplay_bplist_object_t display;
    airplay_bplist_object_t value;
    uint64_t features = 0U;
    uint64_t primary_input = 0U;
    size_t uuid_len = 0U;

    if (display_uuid == NULL || display_uuid_capacity == 0U) {
        return AIRPLAY_BPLIST_INVALID_ARGUMENT;
    }
    display_uuid[0] = '\0';
    airplay_bplist_result_t result = airplay_bplist_get_main_display_size(
        doc, display_width, display_height);
    if (result != AIRPLAY_BPLIST_OK) return result;
    result = airplay_bplist_root(doc, &root);
    if (result != AIRPLAY_BPLIST_OK) return result;
    result = airplay_bplist_dict_get(doc, root, "displays", &displays);
    if (result != AIRPLAY_BPLIST_OK) return result;
    result = airplay_bplist_array_get(doc, displays, 0U, &display);
    if (result != AIRPLAY_BPLIST_OK) return result;
    result = airplay_bplist_dict_get(doc, display, "uuid", &value);
    if (result != AIRPLAY_BPLIST_OK) return result;
    result = airplay_bplist_get_string(doc, value, display_uuid,
                                       display_uuid_capacity, &uuid_len);
    if (result != AIRPLAY_BPLIST_OK) return result;
    if (uuid_len == 0U) return AIRPLAY_BPLIST_WRONG_TYPE;
    result = airplay_bplist_dict_get(doc, display, "features", &value);
    if (result != AIRPLAY_BPLIST_OK) return result;
    result = airplay_bplist_get_uint(doc, value, &features);
    if (result != AIRPLAY_BPLIST_OK) return result;
    result = airplay_bplist_dict_get(doc, display, "primaryInputDevice", &value);
    if (result != AIRPLAY_BPLIST_OK) return result;
    result = airplay_bplist_get_uint(doc, value, &primary_input);
    if (result != AIRPLAY_BPLIST_OK) return result;
    if ((features & AIRPLAY_DISPLAY_FEATURE_HIGH_FIDELITY_TOUCH) == 0U ||
        primary_input != AIRPLAY_PRIMARY_TOUCH_DEVICE) {
        return AIRPLAY_BPLIST_WRONG_TYPE;
    }
    return AIRPLAY_BPLIST_OK;
}

static bool has_matching_main_touch_hid(const airplay_bplist_doc_t *doc,
                                        uint16_t display_width,
                                        uint16_t display_height,
                                        const char *display_uuid)
{
    airplay_bplist_object_t root;
    airplay_bplist_object_t devices;
    airplay_bplist_object_t device;
    airplay_bplist_object_t value;
    airplay_bplist_type_t type;
    uint64_t device_count = 0U;
    uint8_t expected[AIRPLAY_HID_TOUCH_DESCRIPTOR_BYTES];
    size_t expected_len = 0U;
    unsigned touch_device_count = 0U;

    if (airplay_hid_touch_descriptor_build(display_width, display_height,
                                            expected, sizeof(expected),
                                            &expected_len) != true ||
        airplay_bplist_root(doc, &root) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_dict_get(doc, root, "hidDevices", &devices) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_get_type(doc, devices, &type, &device_count) != AIRPLAY_BPLIST_OK ||
        type != AIRPLAY_BPLIST_ARRAY || device_count == 0U ||
        device_count > AIRPLAY_INFO_MAX_HID_DEVICES) {
        return false;
    }

    for (uint64_t index = 0U; index < device_count; ++index) {
        char uid[32];
        size_t uid_len = 0U;
        if (airplay_bplist_array_get(doc, devices, index, &device) != AIRPLAY_BPLIST_OK ||
            airplay_bplist_get_type(doc, device, &type, NULL) != AIRPLAY_BPLIST_OK ||
            type != AIRPLAY_BPLIST_DICTIONARY ||
            airplay_bplist_dict_get(doc, device, "uuid", &value) != AIRPLAY_BPLIST_OK ||
            airplay_bplist_get_string(doc, value, uid, sizeof(uid), &uid_len) != AIRPLAY_BPLIST_OK) {
            return false;
        }
        if (strcmp(uid, "2a2a2a2a") != 0) continue;
        ++touch_device_count;
        char target_uuid[64];
        size_t target_uuid_len = 0U;
        const uint8_t *descriptor = NULL;
        size_t descriptor_len = 0U;
        uint64_t vendor_id = 0U;
        uint64_t product_id = 0U;
        if (airplay_bplist_dict_get(doc, device, "displayUUID", &value) != AIRPLAY_BPLIST_OK ||
            airplay_bplist_get_string(doc, value, target_uuid, sizeof(target_uuid),
                                      &target_uuid_len) != AIRPLAY_BPLIST_OK ||
            strcmp(target_uuid, display_uuid) != 0 ||
            airplay_bplist_dict_get(doc, device, "hidDescriptor", &value) != AIRPLAY_BPLIST_OK ||
            airplay_bplist_get_data(doc, value, &descriptor, &descriptor_len) != AIRPLAY_BPLIST_OK ||
            descriptor_len != expected_len || memcmp(descriptor, expected, expected_len) != 0 ||
            airplay_bplist_dict_get(doc, device, "hidVendorID", &value) != AIRPLAY_BPLIST_OK ||
            airplay_bplist_get_uint(doc, value, &vendor_id) != AIRPLAY_BPLIST_OK ||
            vendor_id != 2U ||
            airplay_bplist_dict_get(doc, device, "hidProductID", &value) != AIRPLAY_BPLIST_OK ||
            airplay_bplist_get_uint(doc, value, &product_id) != AIRPLAY_BPLIST_OK ||
            product_id != 1U) {
            return false;
        }
    }
    return touch_device_count == 1U;
}

static airplay_identity_provider_t s_provider;
static int s_registered;

esp_err_t airplay_identity_port_register_provider(
    const airplay_identity_provider_t *provider)
{
    if (provider == NULL || provider->get_config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_registered) {
        return ESP_ERR_INVALID_STATE;
    }
    s_provider = *provider;
    s_registered = 1;
    return ESP_OK;
}

esp_err_t airplay_identity_port_get_config(airplay_identity_config_t *out_config)
{
    if (out_config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_config, 0, sizeof(*out_config));
    if (!s_registered) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t result = s_provider.get_config(out_config, s_provider.context);
    if (result != ESP_OK) {
        memset(out_config, 0, sizeof(*out_config));
        return result;
    }
    if (out_config->rtsp_server.info_plist == NULL ||
        out_config->rtsp_server.info_plist_len == 0U ||
        out_config->mdns_hostname == NULL || out_config->mdns_hostname[0] == '\0' ||
        strlen(out_config->mdns_hostname) > 63U ||
        out_config->mdns_instance_name == NULL || out_config->mdns_instance_name[0] == '\0' ||
        strlen(out_config->mdns_instance_name) > 63U ||
        out_config->mdns_txt_records == NULL || out_config->mdns_txt_record_count == 0U ||
        out_config->mdns_txt_record_count > 16U ||
        out_config->device_identifier == NULL || out_config->device_identifier[0] == '\0' ||
        out_config->public_key == NULL || out_config->public_key[0] == '\0' ||
        out_config->source_version == NULL || out_config->source_version[0] == '\0') {
        memset(out_config, 0, sizeof(*out_config));
        return ESP_ERR_INVALID_ARG;
    }
    airplay_bplist_doc_t info_doc;
    uint16_t display_width = 0U;
    uint16_t display_height = 0U;
    char display_uuid[64];
    if (airplay_bplist_open(out_config->rtsp_server.info_plist,
                            out_config->rtsp_server.info_plist_len,
                            &info_doc) != AIRPLAY_BPLIST_OK ||
        get_main_display_identity(&info_doc, &display_width, &display_height,
                                  display_uuid, sizeof(display_uuid)) != AIRPLAY_BPLIST_OK ||
        !has_matching_main_touch_hid(&info_doc, display_width, display_height,
                                     display_uuid) ||
        (out_config->main_display_rotation_degrees != 0U &&
         out_config->main_display_rotation_degrees != 90U &&
         out_config->main_display_rotation_degrees != 180U &&
         out_config->main_display_rotation_degrees != 270U)) {
        memset(out_config, 0, sizeof(*out_config));
        return ESP_ERR_INVALID_ARG;
    }
    out_config->main_display_width_pixels = display_width;
    out_config->main_display_height_pixels = display_height;

    size_t mdns_txt_bytes = 0U;
    for (size_t i = 0U; i < out_config->mdns_txt_record_count; ++i) {
        const char *key = out_config->mdns_txt_records[i].key;
        const char *value = out_config->mdns_txt_records[i].value;
        if (key == NULL || key[0] == '\0' || value == NULL ||
            strlen(key) > 63U || strlen(value) > 255U || strchr(key, '=') != NULL ||
            strchr(key, '\r') != NULL || strchr(key, '\n') != NULL ||
            strchr(value, '\r') != NULL || strchr(value, '\n') != NULL) {
            memset(out_config, 0, sizeof(*out_config));
            return ESP_ERR_INVALID_ARG;
        }
        mdns_txt_bytes += strlen(key) + strlen(value) + 2U;
        if (mdns_txt_bytes > 1024U) {
            memset(out_config, 0, sizeof(*out_config));
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t j = 0U; j < i; ++j) {
            if (strcmp(key, out_config->mdns_txt_records[j].key) == 0) {
                memset(out_config, 0, sizeof(*out_config));
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    return ESP_OK;
}

int airplay_identity_port_is_configured(void)
{
    return s_registered;
}
