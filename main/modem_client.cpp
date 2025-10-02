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
#include "esp_task_wdt.h"
#include "esp_timer.h"

#include "nvs_flash.h"
#include "wifi_app.h"
#include "sntp_time_sync.h"

#define PACKET_TIMEOUT 600                  //30 seconds
#define BROKER_URL "broker.hivemq.com"
#define BROKER_PORT 8883

#define FW_VER "0.03"

MCP_t dev;

static const char *TAG = "modem_client";
uint8_t mac_addr[6] = {0};
char mac_string[13] = "0123456789AB";
static EventGroupHandle_t event_group = NULL;
static const int CONNECT_BIT = BIT0;
static const int GOT_DATA_BIT = BIT2;
struct sock_dce::MODEM_DNA_STATS modem_dna;

char data_buff[255];
char topic_buff[255];

#define TOTAL_ZONE 10
static uint8_t zone_alert_state[TOTAL_ZONE];
uint16_t zone_raw_value[TOTAL_ZONE];
// uint16_t zone_lower_limit[TOTAL_ZONE] = {300,300,300,300,300,300,300,300,0,0};
// uint16_t zone_upper_limit[TOTAL_ZONE] = {700,700,700,700,700,700,700,700,0,0};
uint16_t zone_lower_limit[TOTAL_ZONE] = {500,500,500,500,500,500,500,500,0,0};
uint16_t zone_upper_limit[TOTAL_ZONE] = {900,900,900,900,900,900,900,900,0,0};

uint16_t alert_flg = 0;
uint16_t prev_alert_flg = 0;
static uint16_t loop_counter = 0;
static uint16_t prev_loop_counter = 0;


// Public Function Definition

/*
 * Get the Temperature Values
 * @return temperature value
 */
uint8_t get_temperature(void)
{
  return 10;
}

/*
 * Get the Humidity Values
 * @return humidity value
 */
uint8_t get_humidity(void)
{
  return 90;
}

// Private Function Prototypes
static void wifi_application_connected_events( void )
{
  ESP_LOGI(TAG, "WiFi Application Connected!");
  sntp_time_sync_task_start();
}


static void periodic_timer_callback(void* arg)
{
    loop_counter++;
    gpio_set_level( (gpio_num_t)CONFIG_EXAMPLE_LED_STATUS_PIN, 1);
    if(zone_raw_value[TOTAL_ZONE-2] != gpio_get_level((gpio_num_t)CONFIG_EXAMPLE_BUZZER_STATUS_PIN))
    {
        alert_flg |= 0x0100;
        zone_alert_state[TOTAL_ZONE-2] = 0x01;
    }
    else
    {
        alert_flg &= ~0x0100;
    }

    zone_raw_value[TOTAL_ZONE-1] = gpio_get_level((gpio_num_t)39);
    zone_raw_value[TOTAL_ZONE-2] = gpio_get_level((gpio_num_t)CONFIG_EXAMPLE_BUZZER_STATUS_PIN);
    for(uint8_t i = 0; i < (TOTAL_ZONE-2); i++)
    {
        zone_raw_value[i] = mcpReadData(&dev, i);
        
        uint16_t bitmask = 1 << i;
        if (zone_raw_value[i] < zone_lower_limit[i])
        {
            zone_alert_state[i] |= 0x01;
            alert_flg |= bitmask; 
        }
        else if (zone_raw_value[i] > zone_upper_limit[i])
        {
            zone_alert_state[i] |= 0x02;
            alert_flg |= bitmask; 
        }
        else
        {
            alert_flg &= ~bitmask;
        }
        // ESP_LOGI(TAG, " ARM/DISARM STATUS %d ", gpio_get_level((gpio_num_t)39));
        // ESP_LOGI(TAG, "%d %d %X %X %d %d %d %d", i, bitmask, alert_flg, prev_alert_flg, zone_lower_limit[i], zone_upper_limit[i], zone_raw_value[i], zone_alert_state[i]);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRId32, base, event_id);
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        topic_buff[0] = 0;
        sprintf(topic_buff,"/ZIGRON/%s/CLEAR",mac_string);
        msg_id = esp_mqtt_client_subscribe(client, topic_buff, 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        topic_buff[0] = 0;
        sprintf(topic_buff,"/ZIGRON/%s/COMMAND",mac_string);
        msg_id = esp_mqtt_client_subscribe(client, topic_buff, 0);
        ESP_LOGI(TAG, "subscribe successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        esp_restart();
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        // msg_id = esp_mqtt_client_publish(client, "/topic/esp-pppos", "esp32-pppos", 0, 0, 0);
        // ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
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
        // if(event->topic == )
        zone_alert_state[0] = 0;zone_alert_state[1] = 0;zone_alert_state[2] = 0;zone_alert_state[3] = 0;
        zone_alert_state[4] = 0;zone_alert_state[5] = 0;zone_alert_state[6] = 0;zone_alert_state[7] = 0;
        zone_alert_state[8] = 0;
        alert_flg = 0;
        loop_counter = PACKET_TIMEOUT-1;
        xEventGroupSetBits(event_group, GOT_DATA_BIT);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        esp_restart();
        break;
    default:
        ESP_LOGI(TAG, "MQTT other event id: %d", event->event_id);
        break;
    }
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
            break;
    }
    return ESP_OK;
}

extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if ( (ret == ESP_ERR_NVS_NO_FREE_PAGES) || (ret == ESP_ERR_NVS_NEW_VERSION_FOUND) )
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Start WiFi
    wifi_app_start();

    // Set Connected Event callback
    wifi_app_set_callback(&wifi_application_connected_events);
    // while(1)vTaskDelay(1);

    /* Init and register system/core components */
    // ESP_ERROR_CHECK(esp_netif_init());
    // ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_read_mac( mac_addr, ESP_MAC_EFUSE_FACTORY);
    sprintf(mac_string,"%02X%02X%02X%02X%02X%02X", mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    ESP_LOGI(TAG, "MAC address %s",mac_string);

    event_group = xEventGroupCreate();

    mcpInit(&dev, MCP3008, CONFIG_MISO_GPIO, CONFIG_MOSI_GPIO, CONFIG_SCLK_GPIO, CONFIG_CS_GPIO, MCP_SINGLE);

    gpio_set_direction( (gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction( (gpio_num_t)CONFIG_EXAMPLE_SIM_SELECT_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction( (gpio_num_t)CONFIG_EXAMPLE_BUZZER_STATUS_PIN, GPIO_MODE_INPUT);
    gpio_set_direction( (gpio_num_t)CONFIG_EXAMPLE_LED_STATUS_PIN, GPIO_MODE_OUTPUT);

    const esp_timer_create_args_t periodic_timer_args = {
        .callback = &periodic_timer_callback,
        /* name is optional, but may help identify the timer when debugging */
        .name = "periodic"
    };

    esp_timer_handle_t periodic_timer;
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
    /* The timer has been created but is not running yet */
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 100000));

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

    while (!dce->init(&modem_dna))
    {
        ESP_LOGE(TAG,  "Failed to setup network 1");
    }

    data_buff[0] = 0;
    topic_buff[0] = 0;
    sprintf(data_buff,"{\"DNA\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",%d,%s,%s]}%c",
    modem_dna.ip_address.c_str(),modem_dna.operator_name.c_str(),modem_dna.imsi.c_str(),modem_dna.imei.c_str(),modem_dna.module_name.c_str(),modem_dna.signal_quality,FW_VER,mac_string,0);
   
    // sprintf(topic_buff,"03345472486");
    // dce->alert_sms(topic_buff, data_buff);

    ESP_LOGI(TAG, "Got IP %s", modem_dna.ip_address.c_str());
    ESP_LOGI(TAG, "operater %s",modem_dna.operator_name.c_str());
    ESP_LOGI(TAG, "IMSI %s",modem_dna.imsi.c_str());
    ESP_LOGI(TAG, "IMEI %s",modem_dna.imei.c_str());
    ESP_LOGI(TAG, "module %s",modem_dna.module_name.c_str());
    ESP_LOGI(TAG, "CSQ %d %d",modem_dna.signal_quality, modem_dna.ber);
    

    esp_transport_handle_t at = esp_transport_at_init(dce.get());
    // esp_transport_handle_t ssl = esp_transport_tls_init(at);
    esp_http_client_config_t config = {       
        .url = "http://54.194.219.149:45056/firmware/MultiSerial.ino.bin",
        .port = 45056,
        .timeout_ms = 10000,  // Increased timeout
        .event_handler = http_event_handler,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .keep_alive_enable = true,
        .transport = at,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    // ESP_LOGI(TAG, "Free heap: %ld", esp_get_free_heap_size());
    // vTaskDelay(pdMS_TO_TICKS(500));
    // ret = esp_https_ota(&ota_config);
    // if (ret == ESP_OK) {
    //     esp_restart();
    // } else {
    //     ESP_LOGE(TAG, "OTA failed with error: %s", esp_err_to_name(ret));
    // }

    // esp_http_client_config_t test_config = {
    //     .url = "https://google.com",
    //     .timeout_ms = 10000,
    //     .event_handler = http_event_handler,
    //     .transport_type = HTTP_TRANSPORT_OVER_TCP,
    //     .transport = ssl,
    // };
    // ESP_LOGI(TAG, "Free heap: %ld", esp_get_free_heap_size());
    // vTaskDelay(pdMS_TO_TICKS(500));
    // esp_http_client_handle_t client = esp_http_client_init(&test_config);
    // esp_err_t err = esp_http_client_perform(client);

    esp_mqtt_client_config_t mqtt_config = {};
    mqtt_config.broker.address.port = 45055;
    mqtt_config.session.message_retransmit_timeout = 10000;
    mqtt_config.broker.address.uri = "mqtt://zigron:zigron123@54.194.219.149";
    mqtt_config.network.transport = at;

    esp_mqtt_client_handle_t mqtt_client = esp_mqtt_client_init(&mqtt_config);
    esp_mqtt_client_register_event(mqtt_client, static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID), mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);

    while (1) {
        if (loop_counter%PACKET_TIMEOUT == 0 || prev_alert_flg != alert_flg)
        {  
            data_buff[0] = 0;
            topic_buff[0] = 0;
            sprintf(data_buff,"{\"RAW\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],\"ALERT\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],\"DNA\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",%d,\"%s\",%lld]}%c",
            zone_raw_value[0],zone_raw_value[1],zone_raw_value[2],zone_raw_value[3],zone_raw_value[4],zone_raw_value[5],zone_raw_value[6],
            zone_raw_value[7],zone_raw_value[8],zone_raw_value[9],zone_alert_state[0],zone_alert_state[1],zone_alert_state[2],zone_alert_state[3],
            zone_alert_state[4],zone_alert_state[5],zone_alert_state[6],zone_alert_state[7],zone_alert_state[8],zone_alert_state[9],
            modem_dna.ip_address.c_str(),modem_dna.operator_name.c_str(),modem_dna.imsi.c_str(),modem_dna.imei.c_str(),modem_dna.module_name.c_str(),modem_dna.signal_quality,FW_VER,esp_timer_get_time()/1000000,0);
            
            if(prev_alert_flg != alert_flg)
            {
                sprintf(topic_buff,"/ZIGRON/%s/ALERT",mac_string);
            }
            else
            {
                sprintf(topic_buff,"/ZIGRON/%s/HB",mac_string);
            } 
            int publish_response = esp_mqtt_client_publish(mqtt_client, topic_buff, data_buff, 0, 0, 0);
            
            if(publish_response == 0xFFFFFFFF)
            {
                esp_restart();               
            }
            ESP_LOGI(TAG, "%X %X %s :%s\r\n", alert_flg, prev_alert_flg, topic_buff, data_buff);
        }
        prev_alert_flg = alert_flg;

        gpio_set_level( (gpio_num_t)CONFIG_EXAMPLE_LED_STATUS_PIN, 0);vTaskDelay(1);

        // gpio_set_direction( (gpio_num_t)40, GPIO_MODE_OUTPUT);
        // gpio_set_level( (gpio_num_t)40, 1);vTaskDelay(100);gpio_set_level( (gpio_num_t)40, 0); gpio_set_level( (gpio_num_t)CONFIG_EXAMPLE_LED_STATUS_PIN, 1);vTaskDelay(100);
    }
}
