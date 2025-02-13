/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* PPPoS Client Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"
#include "mqtt_client.h"
#include "esp_modem_config.h"
#include "cxx_include/esp_modem_api.hpp"
#include "sock_dce.hpp"
#include "esp_log.h"
#include "tcp_transport_mbedtls.h"
#include "tcp_transport_at.h"
#include "esp_https_ota.h"      // For potential OTA configuration
#include "esp_mac.h"
#include "mcp3002.h"
#include "driver/gpio.h"

#define BROKER_URL "mqtt.eclipseprojects.io"
#define BROKER_PORT 8883

static const char *TAG = "modem_client";
uint8_t mac_addr[6] = {0};
static EventGroupHandle_t event_group = NULL;
static const int CONNECT_BIT = BIT0;
static const int GOT_DATA_BIT = BIT2;

#define TOTAL_ZONE 8
static uint8_t zone_alert_state[TOTAL_ZONE];
uint16_t zone_raw_value[TOTAL_ZONE];
uint16_t zone_upper_limit[TOTAL_ZONE];
uint16_t zone_lower_limit[TOTAL_ZONE];

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRId32, base, event_id);
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        msg_id = esp_mqtt_client_subscribe(client, "/topic/esp-pppos", 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        msg_id = esp_mqtt_client_subscribe(client, "/topic/esp-pppos1", 0);
        ESP_LOGI(TAG, "subscribe successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        msg_id = esp_mqtt_client_publish(client, "/topic/esp-pppos", "esp32-pppos", 0, 0, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        xEventGroupSetBits(event_group, GOT_DATA_BIT);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        break;
    default:
        ESP_LOGI(TAG, "MQTT other event id: %d", event->event_id);
        break;
    }
}


extern "C" void app_main(void)
{

    /* Init and register system/core components */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_read_mac( mac_addr, ESP_MAC_EFUSE_FACTORY);
    event_group = xEventGroupCreate();

    MCP_t dev;
    mcpInit(&dev, MCP3008, CONFIG_MISO_GPIO, CONFIG_MOSI_GPIO, CONFIG_SCLK_GPIO, CONFIG_CS_GPIO, MCP_SINGLE);

    gpio_set_direction( (gpio_num_t)38, GPIO_MODE_OUTPUT);
    gpio_set_level( (gpio_num_t)38, 0);
    gpio_set_direction( (gpio_num_t)48, GPIO_MODE_INPUT);

    /* Configure and create the UART DTE */
    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    /* setup UART specific configuration based on kconfig options */
    dte_config.uart_config.tx_io_num = CONFIG_EXAMPLE_MODEM_UART_TX_PIN;
    dte_config.uart_config.rx_io_num = CONFIG_EXAMPLE_MODEM_UART_RX_PIN;
    dte_config.uart_config.rts_io_num = CONFIG_EXAMPLE_MODEM_UART_RTS_PIN;
    dte_config.uart_config.cts_io_num = CONFIG_EXAMPLE_MODEM_UART_CTS_PIN;
    dte_config.uart_config.rx_buffer_size = CONFIG_EXAMPLE_MODEM_UART_RX_BUFFER_SIZE;
    dte_config.uart_config.tx_buffer_size = CONFIG_EXAMPLE_MODEM_UART_TX_BUFFER_SIZE;
    dte_config.uart_config.event_queue_size = CONFIG_EXAMPLE_MODEM_UART_EVENT_QUEUE_SIZE;
    dte_config.task_stack_size = CONFIG_EXAMPLE_MODEM_UART_EVENT_TASK_STACK_SIZE * 2;
    dte_config.task_priority = CONFIG_EXAMPLE_MODEM_UART_EVENT_TASK_PRIORITY;
    dte_config.dte_buffer_size = CONFIG_EXAMPLE_MODEM_UART_RX_BUFFER_SIZE / 2;

    auto dte = esp_modem::create_uart_dte(&dte_config);
    assert(dte);

    /* Configure the DCE */
    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(CONFIG_EXAMPLE_MODEM_APN);

    /* create the DCE and initialize network manually (using AT commands) */
    auto dce = sock_dce::create(&dce_config, std::move(dte));
    if (!dce->init()) {
        ESP_LOGE(TAG,  "Failed to setup network 1");
        // gpio_set_level( (gpio_num_t)38, 0);
        // dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(CONFIG_EXAMPLE_MODEM_APN);
        // dce = sock_dce::create(&dce_config, std::move(dte));
        // if (!dce->init()) {
        //     ESP_LOGE(TAG,  "Failed to setup network 2");
        // }
        // return;
    }
    ESP_LOGI(TAG, "\"0x%X%X%X%X%X%X\" MAC address",
        mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    
    esp_mqtt_client_config_t mqtt_config = {};
    mqtt_config.broker.address.port = BROKER_PORT;
    mqtt_config.session.message_retransmit_timeout = 10000;
#ifndef CONFIG_EXAMPLE_CUSTOM_TCP_TRANSPORT
    mqtt_config.broker.address.uri = "mqtts://127.0.0.1";
    dce->start_listening(BROKER_PORT);
#else
    mqtt_config.broker.address.uri = "mqtt://" BROKER_URL;
    esp_transport_handle_t at = esp_transport_at_init(dce.get());
    esp_transport_handle_t ssl = esp_transport_tls_init(at);

    mqtt_config.network.transport = ssl;
#endif
    esp_mqtt_client_handle_t mqtt_client = esp_mqtt_client_init(&mqtt_config);
    esp_mqtt_client_register_event(mqtt_client, static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID), mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
#ifndef CONFIG_EXAMPLE_CUSTOM_TCP_TRANSPORT
    if (!dce->connect(BROKER_URL, BROKER_PORT)) {
        ESP_LOGE(TAG, "Failed to start DCE");
        return;
    }
    while (1) {
        while (dce->perform_sock()) {
            ESP_LOGV(TAG, "...performing");
        }
        ESP_LOGE(TAG, "Loop exit.. retrying");
        // handle disconnections errors
        if (!dce->init()) {
            ESP_LOGE(TAG, "Failed to reinit network");
            return;
        }
        if (!dce->connect(BROKER_URL, BROKER_PORT)) {
            ESP_LOGI(TAG, "Network reinitialized, retrying");
        }
    }
#else
    // esp_http_client_config_t config = {
    //     .url = CONFIG_EXAMPLE_PERFORM_OTA_URI,
    //     // .cert_pem = (char *)server_cert_pem_start,
    // };
    // esp_https_ota_config_t ota_config = {
    //     .http_config = &config,
    // };
    // esp_err_t ret = esp_https_ota(&ota_config);
    // if (ret == ESP_OK) {
    //     esp_restart();
    // }
    
    while (1) {
        static uint8_t loop_counter = 0;
        // loop_counter++;
        // for(uint8_t i = 0; i < TOTAL_ZONE; i++)
        // {
        //     zone_raw_value[i] = mcpReadData(&dev, i);
        //     if (zone_raw_value[i] < zone_lower_limit[i])
        //     {
        //         zone_alert_state[i] |= 0x01;
        //     }
        //     else if (zone_raw_value[i] > zone_upper_limit[i])
        //     {
        //         zone_alert_state[i] |= 0x02;
        //     }
        // }
        vTaskDelay(5000);
        if (loop_counter == 0)
        {  
            // if (dce->set_mode(esp_modem::modem_mode::CMUX_MODE)) {
            //     ESP_LOGI(TAG, "Modem has correctly entered multiplexed data mode");
            // } else {
            //     ESP_LOGE(TAG, "Failed to configure multiplexed command mode... exiting");
            //     // return;
            // }

		    printf("%d, %d,%d,%d,%d,%d,%d,%d,%d\r\n",gpio_get_level((gpio_num_t)48), mcpReadData(&dev, 0),mcpReadData(&dev, 1),mcpReadData(&dev, 2),
            mcpReadData(&dev, 3),mcpReadData(&dev, 4),mcpReadData(&dev, 5),mcpReadData(&dev, 6),mcpReadData(&dev, 7));
            // esp_mqtt_client_publish(mqtt_client, "/topic/esp-pppos", "esp32-pppos", 0, 0, 0);

            // if (dce->set_mode(esp_modem::modem_mode::COMMAND_MODE)) {
            //     ESP_LOGE(TAG, "Modem has correctly entered multiplexed command mode");
            // } else {
            //     ESP_LOGE(TAG, "Failed to configure multiplexed command mode... exiting");
            //     // return;
            // }
        }
    }
#endif

}
