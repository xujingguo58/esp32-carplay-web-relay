/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "airplay_rtsp_server.h"
#include "esp_err.h"
#include <stddef.h>

typedef struct {
    const char *key;
    const char *value;
} airplay_mdns_txt_record_t;

typedef struct {
    /*
     * RTSP pairing callbacks, protected identity material and binary /info plist.
     * /info must declare one main-display touch HID device with the DiPlay
     * report descriptor matching displays[0]'s UUID and pixel dimensions.
     */
    airplay_rtsp_server_config_t rtsp_server;
    /* Host and AirPlay Bonjour metadata supplied by the authorized deployment. */
    const char *mdns_hostname;
    const char *mdns_instance_name;
    const airplay_mdns_txt_record_t *mdns_txt_records;
    size_t mdns_txt_record_count;
    /* Matching receiver identity advertised by iAP2 CarPlayStartSession (0x4301). */
    const char *device_identifier;
    const char *public_key;
    const char *source_version;
    /* Filled from /info displays[0]; rotation is provider-supplied and defaults to 0. */
    uint16_t main_display_width_pixels;
    uint16_t main_display_height_pixels;
    uint16_t main_display_rotation_degrees;
} airplay_identity_config_t;

typedef esp_err_t (*airplay_identity_get_config_cb_t)(
    airplay_identity_config_t *out_config,
    void *context);

typedef struct {
    airplay_identity_get_config_cb_t get_config;
    void *context;
} airplay_identity_provider_t;

/* Register once during app startup. Provider-owned callback contexts and identity strings must
 * remain valid for the lifetime of the RTSP server. No keys or pairing codes are stored here. */
esp_err_t airplay_identity_port_register_provider(
    const airplay_identity_provider_t *provider);

/* Retrieves the provider's deployment identity and RTSP configuration. */
esp_err_t airplay_identity_port_get_config(airplay_identity_config_t *out_config);
int airplay_identity_port_is_configured(void);
