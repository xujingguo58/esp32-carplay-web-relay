#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../components/bt_port/bt_port.c"

static void rx_callback(const uint8_t *data, size_t len, void *context)
{
    (void)data; (void)len; (void)context;
}

static void state_callback(bool connected, void *context)
{
    (void)connected; (void)context;
}

static void simulate_open(uint32_t handle)
{
    esp_spp_cb_param_t event = {0};
    event.open.status = ESP_SPP_SUCCESS;
    event.open.handle = handle;
    spp_callback(ESP_SPP_OPEN_EVT, &event);
}

static void simulate_rx(const uint8_t *data, uint16_t length)
{
    esp_spp_cb_param_t event = {0};
    event.data_ind.status = ESP_SPP_SUCCESS;
    event.data_ind.data = (uint8_t *)data;
    event.data_ind.len = length;
    spp_callback(ESP_SPP_DATA_IND_EVT, &event);
}

static bt_rx_event_t queue_event(size_t index)
{
    bt_rx_event_t event;
    size_t slot = (s_rx_queue->head + index) % s_rx_queue->depth;
    memcpy(&event, s_rx_queue->items + slot * s_rx_queue->item_size, sizeof(event));
    return event;
}

int main(void)
{
    static const uint8_t chunk[BT_PORT_RX_CHUNK_BYTES] = {0};
    assert(bt_port_start(rx_callback, state_callback, NULL) == ESP_OK);

    esp_spp_cb_param_t init = {0};
    init.init.status = ESP_SPP_SUCCESS;
    spp_callback(ESP_SPP_INIT_EVT, &init);
    bt_port_set_discovery_enabled(true);
    simulate_open(42U);
    assert(bt_port_is_connected());
    (void)xQueueReset(s_rx_queue); /* Model the dispatcher consuming the OPEN event. */

    for (size_t index = 0; index < BT_PORT_RX_QUEUE_DEPTH; ++index) {
        simulate_rx(chunk, sizeof(chunk));
    }
    assert(s_rx_queue->count == BT_PORT_RX_QUEUE_DEPTH);
    simulate_rx(chunk, sizeof(chunk));

    assert(mock_spp_disconnect_call_count == 1U);
    assert(bt_port_rx_dropped_chunks() == 1U);
    assert(!bt_port_is_connected());
    assert(s_rx_queue->count == 1U);
    bt_rx_event_t disconnected = queue_event(0U);
    assert(disconnected.type == BT_RX_EVENT_STATE);
    assert(!disconnected.connected);

    simulate_open(43U);
    assert(bt_port_is_connected());
    (void)xQueueReset(s_rx_queue);
    for (size_t index = 0; index < BT_PORT_RX_QUEUE_DEPTH; ++index) {
        simulate_rx(chunk, sizeof(chunk));
    }
    post_state(false);
    assert(s_rx_queue->count == 1U);
    disconnected = queue_event(0U);
    assert(disconnected.type == BT_RX_EVENT_STATE);
    assert(!disconnected.connected);

    puts("bt_port host test passed (RX overflow disconnects and prioritizes state)");
    return 0;
}
