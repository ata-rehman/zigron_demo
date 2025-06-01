/*
 * http_server.c
 *
 *  Created on: 17-Jul-2023
 *      Author: xpress_embedo
 */
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "sys/param.h"

#include "main.h"
#include "http_server.h"
#include "tasks_common.h"
#include "wifi_app.h"
#include "sntp_time_sync.h"

// Macros
#define HTTP_SERVER_MAX_URI_HANDLERS                    (20u)
#define HTTP_SERVER_RECEIVE_WAIT_TIMEOUT                (10u)   // in seconds
#define HTTP_SERVER_SEND_WAIT_TIMEOUT                   (10u)   // in seconds
#define HTTP_SERVER_MONITOR_QUEUE_LEN                   (3u)

// Private Variables
static const char TAG[] = "http_server";
// HTTP Server Task Handle
static httpd_handle_t http_server_handle = NULL;
// HTTP Server Monitor Task Handler
static TaskHandle_t task_http_server_monitor = NULL;
// Queue Handle used to manipulate the main queue of events
static QueueHandle_t http_server_monitor_q_handle;
// Firmware Update Status
static int fw_update_status = OTA_UPDATE_PENDING;
// Local Time Status
static bool g_is_local_time_set = false;

// ESP32 Timer Configuration Passed to esp_timer_create
static const esp_timer_create_args_t fw_update_reset_args =
{
  .callback = &http_server_fw_update_reset_cb,
  .arg = NULL,
  .dispatch_method = ESP_TIMER_TASK,
  .name = "fw_update_reset"
};
esp_timer_handle_t fw_update_reset;

// WiFi Connect Status
static http_server_wifi_connect_status_e g_wifi_connect_status = HTTP_WIFI_STATUS_CONNECT_NONE;

// Embedded Files: JQuery, index.html, app.css, app.js, and favicon.ico files
extern const uint8_t jquery_3_3_1_min_js_start[]      asm("_binary_jquery_3_3_1_min_js_start");
extern const uint8_t jquery_3_3_1_min_js_end[]        asm("_binary_jquery_3_3_1_min_js_end");
extern const uint8_t index_html_start[]               asm("_binary_index_html_start");
extern const uint8_t index_html_end[]                 asm("_binary_index_html_end");
extern const uint8_t app_css_start[]                  asm("_binary_app_css_start");
extern const uint8_t app_css_end[]                    asm("_binary_app_css_end");
extern const uint8_t app_js_start[]                   asm("_binary_app_js_start");
extern const uint8_t app_js_end[]                     asm("_binary_app_js_end");
extern const uint8_t favicon_ico_start[]              asm("_binary_favicon_ico_start");
extern const uint8_t favicon_ico_end[]                asm("_binary_favicon_ico_end");

// Private Function Prototypes
static void http_server_monitor(void *pvParameter);
static httpd_handle_t http_server_configure( void );
static esp_err_t http_server_j_query_handler(httpd_req_t *req);
static esp_err_t http_server_index_html_handler(httpd_req_t *req);
static esp_err_t http_server_app_css_handler(httpd_req_t *req);
static esp_err_t http_server_app_js_handler(httpd_req_t *req);
static esp_err_t http_server_favicon_handler(httpd_req_t *req);
static esp_err_t http_server_ota_update_handler(httpd_req_t *req);
static esp_err_t http_server_ota_status_handler(httpd_req_t *req);
static esp_err_t http_server_sensor_value_handler(httpd_req_t *req);
static esp_err_t http_server_wifi_connect_handler(httpd_req_t *req);
static esp_err_t http_server_wifi_connect_status_handler(httpd_req_t *req);
static esp_err_t http_server_get_wifi_connect_info_handler(httpd_req_t *req);
static esp_err_t http_server_wifi_disconnect_json_handler(httpd_req_t *req);
static esp_err_t http_server_get_local_time_handler(httpd_req_t *req);
static esp_err_t http_server_get_ap_ssid_handler(httpd_req_t *req);
static void http_server_fw_update_reset_timer(void);

// Public Function Definition
/*
 * Starts the HTTP Server
 */
void http_server_start(void)
{
  if( http_server_handle == NULL )
  {
    http_server_handle = http_server_configure();
  }
}

/*
 * Stops the HTTP Server
 */
void http_server_stop(void)
{
  if( http_server_handle )
  {
    httpd_stop(http_server_handle);
    ESP_LOGI(TAG, "http_server_stop: stopping HTTP Server");
    http_server_handle = NULL;
  }

  if( task_http_server_monitor )
  {
    vTaskDelete(task_http_server_monitor);
    ESP_LOGI(TAG,"http_server_stop: stopping HTTP server monitor");
    task_http_server_monitor = NULL;
  }
}

/*
 * Timer Callback function which calls esp_restart function upon successful
 * firmware update
 */
void http_server_fw_update_reset_cb(void *arg)
{
  ESP_LOGI(TAG, "http_fw_update_reset_cb: Timer timed-out, restarting the device");
  esp_restart();
}

/*
 * Sends a message to the Queue
 * @param msg_id Message ID from the http_server_msg_e enum
 * @return pdTRUE if an item was successfully sent to the queue, otherwise pdFALSE
 */
BaseType_t http_server_monitor_send_msg(http_server_msg_e msg_id)
{
  http_server_q_msg_t msg;
  msg.msg_id = msg_id;
  return xQueueSend(http_server_monitor_q_handle, &msg, portMAX_DELAY );
}


// Private Function Definitions
/*
 * HTTP Server Monitor Task used to track events of the HTTP Server.
 * @param pvParameter parameters which can be passed to the task
 * @return http server instance handle if successful, NULL otherwise
 */
static void http_server_monitor( void *pvParameter )
{
  http_server_q_msg_t msg;
  for( ;; )
  {
    if( xQueueReceive(http_server_monitor_q_handle, &msg, portMAX_DELAY) )
    {
      switch (msg.msg_id)
      {
      case HTTP_MSG_WIFI_CONNECT_INIT:
        ESP_LOGI( TAG, "HTTP_MSG_WIFI_CONNECT_INIT");
        g_wifi_connect_status = HTTP_WIFI_STATUS_CONNECTING;
        break;
      case HTTP_MSG_WIFI_CONNECT_SUCCESS:
        ESP_LOGI( TAG, "HTTP_MSG_WIFI_CONNECT_SUCCESS");
        g_wifi_connect_status = HTTP_WIFI_STATUS_CONNECT_SUCCESS;
        break;
      case HTTP_MSG_WIFI_CONNECT_FAIL:
        ESP_LOGI( TAG, "HTTP_MSG_WIFI_CONNECT_FAIL");
        g_wifi_connect_status = HTTP_WIFI_STATUS_CONNECT_FAILED;
        break;
      case HTTP_MSG_WIFI_USER_DISCONNECT:
        ESP_LOGI( TAG, "HTTP_MSG_WIFI_USER_DISCONNECT");
        g_wifi_connect_status = HTTP_WIFI_STATUS_DISCONNECTED;
        break;
      case HTTP_MSG_WIFI_OTA_UPDATE_SUCCESSFUL:
        ESP_LOGI( TAG, "HTTP_MSG_OTA_UPDATE_SUCCESSFUL");
        fw_update_status = OTA_UPDATE_SUCCESSFUL;
        http_server_fw_update_reset_timer();
        break;
      case HTTP_MSG_WIFI_OTA_UPDATE_FAILED:
        ESP_LOGI( TAG, "HTTP_MSG_OTA_UPDATE_FAILED");
        fw_update_status = OTA_UPDATE_FAILED;
        break;
      case HTTP_MSG_TIME_SERVICE_INITIALIZED:
        ESP_LOGI( TAG, "HTTP_MSG_TIME_SERVICE_INITIALIZED");
        g_is_local_time_set = true;
        break;
      default:
        break;
      }
    }
  }
}

/*
 * Sets up the default httpd server configuration
 * @return http server instance handle if successful, NULL otherwise
 */
static httpd_handle_t http_server_configure(void)
{
  // Generate the default configuration
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();

  // create HTTP Server Monitor Task
  xTaskCreate(&http_server_monitor, "http_server_monitor", \
              HTTP_SERVER_MONITOR_STACK_SIZE, NULL, \
              HTTP_SERVER_MONITOR_PRIORITY, &task_http_server_monitor);

  // create a message queue
  http_server_monitor_q_handle = xQueueCreate(HTTP_SERVER_MONITOR_QUEUE_LEN,\
                                              sizeof(http_server_q_msg_t));

  // No need to specify the core id as this is esp32s2 with single core

  // Adjust the default priority to 1 less than the wifi application task
  config.task_priority = HTTP_SERVER_TASK_PRIORITY;

  // Specify the Stack Size (default is 4096)
  config.stack_size = HTTP_SERVER_TASK_STACK_SIZE;

  // Increase our URI Handlers
  config.max_uri_handlers = HTTP_SERVER_MAX_URI_HANDLERS;

  // Increase the timeout limits
  config.recv_wait_timeout = HTTP_SERVER_RECEIVE_WAIT_TIMEOUT;
  config.send_wait_timeout = HTTP_SERVER_SEND_WAIT_TIMEOUT;

  ESP_LOGI(TAG,
           "http_server_configure: Starting Server on port: '%d' with task priority: '%d'",
           config.server_port, config.task_priority);

  // Start the httpd server port
  if( httpd_start(&http_server_handle, &config) == ESP_OK )
  {
    ESP_LOGI(TAG, "http_server_configure: Registering URI Handlers");
    // Register jQuery handler
    httpd_uri_t jquery_js =
    {
      .uri       = "/jquery-3.3.1.min.js",
      .method    = HTTP_GET,
      .handler   = http_server_j_query_handler,
      .user_ctx  = NULL
    };
    // Register index.html handler
    httpd_uri_t index_html =
    {
      .uri       = "/",
      .method    = HTTP_GET,
      .handler   = http_server_index_html_handler,
      .user_ctx  = NULL
    };
    // Register app.css handler
    httpd_uri_t app_css =
    {
      .uri       = "/app.css",
      .method    = HTTP_GET,
      .handler   = http_server_app_css_handler,
      .user_ctx  = NULL
    };
    // Register app.js handler
    httpd_uri_t app_js =
    {
      .uri       = "/app.js",
      .method    = HTTP_GET,
      .handler   = http_server_app_js_handler,
      .user_ctx  = NULL
    };
    // Register favicon.ico handler
    httpd_uri_t favicon_ico =
    {
      .uri       = "/favicon.ico",
      .method    = HTTP_GET,
      .handler   = http_server_favicon_handler,
      .user_ctx  = NULL
    };
    // Register OTA Update Handler
    httpd_uri_t ota_update =
    {
      .uri       = "/OTAupdate",
      .method    = HTTP_POST,
      .handler   = http_server_ota_update_handler,
      .user_ctx  = NULL
    };

    // Register OTA Status Handler
    httpd_uri_t ota_status =
    {
      .uri       = "/OTAstatus",
      .method    = HTTP_POST,
      .handler   = http_server_ota_status_handler,
      .user_ctx  = NULL
    };

    // Register Sensor.json handler
    httpd_uri_t sensor_json =
    {
      .uri = "/Sensor",
      .method    = HTTP_GET,
      .handler   = http_server_sensor_value_handler,
      .user_ctx  = NULL
    };

    // Register wifiConnect (.json) handler
    httpd_uri_t wifi_connect_json =
    {
      .uri = "/wifiConnect",
      .method    = HTTP_POST,
      .handler   = http_server_wifi_connect_handler,
      .user_ctx  = NULL
    };

    // Register wifiConnectStatus (.json) handler
    httpd_uri_t wifi_connect_status_json =
    {
      .uri = "/wifiConnectStatus",
      .method    = HTTP_POST,
      .handler   = http_server_wifi_connect_status_handler,
      .user_ctx  = NULL
    };

    // Register wifiConnectInfo (.json) handler
    httpd_uri_t wifi_connect_info_json =
    {
      .uri = "/wifiConnectInfo",
      .method    = HTTP_GET,
      .handler   = http_server_get_wifi_connect_info_handler,
      .user_ctx  = NULL
    };

    // Register wifiDisconnect (.json) handler
    httpd_uri_t wifi_disconnect_json =
    {
      .uri = "/wifiDisconnect",
      .method    = HTTP_DELETE,
      .handler   = http_server_wifi_disconnect_json_handler,
      .user_ctx  = NULL
    };

    // Register localTime.json handler
    httpd_uri_t local_time_json =
    {
      .uri = "/localTime",
      .method    = HTTP_GET,
      .handler   = http_server_get_local_time_handler,
      .user_ctx  = NULL
    };

    // Register apSSID.json handler
    httpd_uri_t ap_ssid_json =
    {
      .uri = "/apSSID",
      .method    = HTTP_GET,
      .handler   = http_server_get_ap_ssid_handler,
      .user_ctx  = NULL
    };

    // Register Query Handler
    httpd_register_uri_handler(http_server_handle, &jquery_js);
    httpd_register_uri_handler(http_server_handle, &index_html);
    httpd_register_uri_handler(http_server_handle, &app_css);
    httpd_register_uri_handler(http_server_handle, &app_js);
    httpd_register_uri_handler(http_server_handle, &favicon_ico);
    httpd_register_uri_handler(http_server_handle, &ota_update);
    httpd_register_uri_handler(http_server_handle, &ota_status);
    httpd_register_uri_handler(http_server_handle, &sensor_json);
    httpd_register_uri_handler(http_server_handle, &wifi_connect_json);
    httpd_register_uri_handler(http_server_handle, &wifi_connect_status_json);
    httpd_register_uri_handler(http_server_handle, &wifi_connect_info_json);
    httpd_register_uri_handler(http_server_handle, &wifi_disconnect_json);
    httpd_register_uri_handler(http_server_handle, &local_time_json);
    httpd_register_uri_handler(http_server_handle, &ap_ssid_json);

    return http_server_handle;
  }

  ESP_LOGI(TAG, "http_server_configure: Error starting server!");
  return NULL;
}

/*
 * jQuery get handler requested when accessing the web page.
 * @param req HTTP request for which the uri needs to be handled
 * @return ESP_OK
 */
static esp_err_t http_server_j_query_handler(httpd_req_t *req)
{
  esp_err_t error;
  ESP_LOGI(TAG, "JQuery Requested");
  httpd_resp_set_type( req, "application/javascript");
  error = httpd_resp_send(req, (const char*)jquery_3_3_1_min_js_start, jquery_3_3_1_min_js_end-jquery_3_3_1_min_js_start );
  if( error != ESP_OK )
  {
    ESP_LOGI( TAG, "http_server_j_query_handler: Error %d while sending Response", error );
  }
  else
  {
    ESP_LOGI( TAG, "http_server_j_query_handler: Response Sent Successfully" );
  }
  return error;
}

/*
 * Send the index HTML page
 * @param req HTTP request for which the uri needs to be handled
 * @return ESP_OK
 */
static esp_err_t http_server_index_html_handler(httpd_req_t *req)
{
  esp_err_t error;
  ESP_LOGI(TAG, "Index HTML Requested");
  httpd_resp_set_type( req, "text/html");
  error = httpd_resp_send(req, (const char*)index_html_start, index_html_end-index_html_start );
  if( error != ESP_OK )
  {
    ESP_LOGI( TAG, "http_server_index_html_handler: Error %d while sending Response", error );
  }
  else
  {
    ESP_LOGI( TAG, "http_server_index_html_handler: Response Sent Successfully" );
  }
  return error;
}

/*
 * app.css get handler is requested when accessing the web page
 * @param req HTTP request for which the uri needs to be handled
 * @return ESP_OK
 */
static esp_err_t http_server_app_css_handler(httpd_req_t *req)
{
  esp_err_t error;
  ESP_LOGI(TAG, "APP CSS Requested");
  httpd_resp_set_type( req, "text/css");
  error = httpd_resp_send(req, (const char*)app_css_start, app_css_end-app_css_start );
  if( error != ESP_OK )
  {
    ESP_LOGI( TAG, "http_server_app_css_handler: Error %d while sending Response", error );
  }
  else
  {
    ESP_LOGI( TAG, "http_server_app_css_handler: Response Sent Successfully" );
  }
  return error;
}

/*
 * app.js get handler requested when accessing the web page.
 * @param req HTTP request for which the uri needs to be handled
 * @return ESP_OK
 */
static esp_err_t http_server_app_js_handler(httpd_req_t *req)
{
  esp_err_t error;
  ESP_LOGI(TAG, "APP JS Requested");
  httpd_resp_set_type( req, "application/javascript");
  error = httpd_resp_send(req, (const char*)app_js_start, app_js_end-app_js_start );
  if( error != ESP_OK )
  {
    ESP_LOGI( TAG, "http_server_app_js_handler: Error %d while sending Response", error );
  }
  else
  {
    ESP_LOGI( TAG, "http_server_app_js_handler: Response Sent Successfully" );
  }
  return error;
}

/*
 * Sends the .ico file when accessing the web page
 * @param req HTTP request for which the uri needs to be handled
 * @return ESP_OK
 */
static esp_err_t http_server_favicon_handler(httpd_req_t *req)
{
  esp_err_t error;
  ESP_LOGI(TAG, "Favicon.ico Requested");
  httpd_resp_set_type( req, "image/x-icon");
  error = httpd_resp_send(req, (const char*)favicon_ico_start, favicon_ico_end-favicon_ico_start );
  if( error != ESP_OK )
  {
    ESP_LOGI( TAG, "http_server_favicon_handler: Error %d while sending Response", error );
  }
  else
  {
    ESP_LOGI( TAG, "http_server_favicon_handler: Response Sent Successfully" );
  }
  return error;
}

/**
 * @brief Receives the *.bin file via the web page and handles the firmware update
 * @param req HTTP request for which the uri needs to be handled
 * @return ESP_OK, other ESP_FAIL if timeout occurs and the update canot be started
 */
static esp_err_t http_server_ota_update_handler(httpd_req_t *req)
{
  esp_err_t error;
  esp_ota_handle_t ota_handle;
  char ota_buffer[1024];
  int content_len = req->content_len;   // total content length
  int content_received = 0;
  int recv_len = 0;
  bool is_req_body_started = false;
  bool flash_successful = false;

  // get the next OTA app partition which should be written with a new firmware
  const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

  // our ota_buffer is not sufficient to receive all data in a one go
  // hence we will read the data in chunks and write in chunks, read the below
  // mentioned comments for more information
  do
  {
    // The following is the API to read content of data from the HTTP request
    /* This API will read HTTP content data from the HTTP request into the
     * provided buffer. Use content_len provided in the httpd_req_t structure to
     *  know the length of the data to be fetched.
     *  If the content_len is to large for the buffer then the user may have to
     *  make multiple calls to this functions (as done here), each time fetching
     *  buf_len num of bytes (which is ota_buffer length here), while the pointer
     *  to content data is incremented internally by the same number
     *  This function returns
     *  Bytes: Number of bytes read into the buffer successfully
     *  0: Buffer length parameter is zero/connection closed by peer.
     *  HTTPD_SOCK_ERR_INVALID: Invalid Arguments
     *  HTTPD_SOCK_ERR_TIMEOUT: Timeout/Interrupted while calling socket recv()
     *  HTTPD_SOCK_ERR_FAIL: Unrecoverable error while calling socket recv()
     *  Parameters to this function are:
     *  req: The request being responded to
     *  ota_buffer: Pointer to a buffer that the data will be read into
     *  length: length of the buffer which ever is minimum (as we don't want to
     *          read more data which buffer can't handle)
     */
    recv_len = httpd_req_recv(req, ota_buffer, MIN(content_len, sizeof(ota_buffer)));
    // if recv_len is less than zero, it means some problem (but if timeout error, then try again)
    if( recv_len < 0 )
    {
      // Check if timeout occur, then we will retry again
      if( recv_len == HTTPD_SOCK_ERR_TIMEOUT )
      {
        ESP_LOGI(TAG, "http_server_ota_update_handler: Socket Timeout");
        continue;     // Retry Receiving if Timeout Occurred
      }
      // If there is some other error apart from Timeout, then exit with fail
      ESP_LOGI(TAG, "http_server_ota_update_handler: OTA Other Error, %d", recv_len);
      return ESP_FAIL;
    }
    ESP_LOGI(TAG, "http_server_ota_update_handler: OTA RX: %d of %d", content_received, content_len);

    // We are here which means that "recv_len" is positive, now we have to check
    // if this is the first data we are receiving or not, If so, it will have
    // the information in the header that we need
    if( !is_req_body_started )
    {
      is_req_body_started = true;
      // Now we have to identify from where the binary file content is starting
      // this can be done by actually checking the escape characters i.e. \r\n\r\n
      // Get the location of the *.bin file content (remove the web form data)
      // the strstr will return the pointer to the \r\n\r\n in the ota_buffer
      // and then by adding 4 we reach to the start of the binary content/start
      char *body_start_p = strstr(ota_buffer, "\r\n\r\n") + 4u;
      int body_part_len = recv_len - (body_start_p - ota_buffer);
      ESP_LOGI(TAG, "http_server_ota_update_handler: OTA File Size: %d", content_len);
      /*
       * esp_ota_begin function commence an OTA update writing to the specified
       * partition. The specified partition is erased to the specified image
       * size. If the image size is not yet known, OTA_SIZE_UNKNOWN is passed
       * which will cause the entire partition to be erased.
       * On Success this function allocates memory that remains in use until
       * esp_ota_end is called with the return handle.
       */
      error = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
      if( error != ESP_OK )
      {
        ESP_LOGI(TAG, "http_server_ota_update_handler: Error with OTA Begin, Canceling OTA");
        return ESP_FAIL;
      }
      else
      {
        ESP_LOGI(TAG, "http_server_ota_update_handler: Writing to partition subtype %d at offset 0x%lx", update_partition->subtype, update_partition->address);
      }
      /*
       * esp_ota_write function, writes the OTA update to the partition.
       * This function can be called multiple times as data is received during
       * the OTA operation. Data is written sequentially to the partition.
       * Here we are writing the body start for the first time.
       */
      esp_ota_write(ota_handle, body_start_p, body_part_len);
      content_received += body_part_len;
    }
    else
    {
      /* Continue to receive data above using httpd_req_recv function, and write
       * using esp_ota_write (below), until all the content is received. */
      esp_ota_write(ota_handle, ota_buffer, recv_len);
      content_received += recv_len;
    }

  } while ( (recv_len>0) && (content_received < content_len) );
  // till complete data is received and written or some error is there we will
  // remain in the above mentioned do-while loop

  /* Finish the OTA update and validate newly written app image.
   * After calling esp_ota_end, the handle is no longer valid and memory associated
   * with it is freed (regardless of the results).
   */
  if( esp_ota_end(ota_handle) == ESP_OK )
  {
    // let's update the partition i.e. configure OTA data for new boot partition
    if( esp_ota_set_boot_partition(update_partition) == ESP_OK )
    {
      const esp_partition_t *boot_partition = esp_ota_get_boot_partition();
      ESP_LOGI(TAG, "http_server_ota_update_handler: Next boot partition subtype %d at offset 0x%lx", boot_partition->subtype, boot_partition->address);
      flash_successful = true;
    }
    else
    {
      ESP_LOGI(TAG, "http_server_ota_update_handler: Flash Error");
    }
  }
  else
  {
    ESP_LOGI(TAG, "http_server_ota_update_handler: esp_ota_end Error");
  }

  // We won't update the global variables throughout the file, so send the message about the status
  if( flash_successful )
  {
    http_server_monitor_send_msg(HTTP_MSG_WIFI_OTA_UPDATE_SUCCESSFUL);
  }
  else
  {
    http_server_monitor_send_msg(HTTP_MSG_WIFI_OTA_UPDATE_FAILED);
  }
  return ESP_OK;
}

/*
 * OTA status handler responds with the firmware update status after the OTA
 * update is started and responds with the compile time & date when the page is
 * first requested
 * @param req HTTP request for which the URI needs to be handled
 * @return ESP_OK
 */
static esp_err_t http_server_ota_status_handler(httpd_req_t *req)
{
  char ota_JSON[100];
  ESP_LOGI(TAG, "OTA Status Requested");
  sprintf(ota_JSON, "{\"ota_update_status\":%d,\"compile_time\":\"%s\",\"compile_date\":\"%s\"}", fw_update_status, __TIME__, __DATE__);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, ota_JSON, strlen(ota_JSON));

  return ESP_OK;
}

/*
 * Sensor Readings JSON handler responds with the Sensor Data
 * @param req HTTP request for which the URI needs to be handled
 * @return ESP_OK
 */
static esp_err_t http_server_sensor_value_handler(httpd_req_t *req)
{
  char sensor_JSON[100];
  ESP_LOGI(TAG, "Sensor Readings Requested");
  sprintf(sensor_JSON, "{\"temp\":%d,\"humidity\":\"%d\"}", get_temperature(), get_humidity() );

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, sensor_JSON, strlen(sensor_JSON));

  return ESP_OK;
}

/*
 * wifiConnect.json handler is invoked after the connect button is pressed and
 * it handles the receiving the SSID and Password entered by the user
 * @param req HTTP request for which the URI needs to be handled
 * @return ESP_OK
 */
static esp_err_t http_server_wifi_connect_handler(httpd_req_t *req)
{
  ESP_LOGI(TAG, "/wifiConnect requested -> Connect button is pressed");

  size_t len_ssid = 0, len_password = 0;
  char *ssid_str = NULL, *password_str = NULL;

  // Get the SSID Header
  len_ssid = httpd_req_get_hdr_value_len(req, "my-connect-ssid") + 1;
  if( len_ssid )
  {
    // allocate memory, but make to free it when it is no longer needed
    ssid_str = (char*)malloc(len_ssid);
    if( httpd_req_get_hdr_value_str(req, "my-connect-ssid", ssid_str, len_ssid) == ESP_OK )
    {
      ESP_LOGI(TAG, "wifi_connect_handler: Found Header => my-connect-ssid: %s", ssid_str );
    }
  }

  // Get the Password Header
  len_password = httpd_req_get_hdr_value_len(req, "my-connect-pswd") + 1;
  if( len_password )
  {
    // allocate memory, but make to free it when it is no longer needed
    password_str = (char*)malloc(len_password);
    if( httpd_req_get_hdr_value_str(req, "my-connect-pswd", password_str, len_password) == ESP_OK )
    {
      ESP_LOGI(TAG, "wifi_connect_handler: Found Header => my-connect-pswd: %s", password_str );
    }
  }

  // Update the WiFi Network Configuration and let the WiFi Application Know
  wifi_config_t * wifi_config = wifi_app_get_wifi_config();
  memset( wifi_config, 0x00, sizeof(wifi_config_t));
  memcpy( wifi_config->sta.ssid, ssid_str, len_ssid );
  memcpy( wifi_config->sta.password, password_str, len_password );

  wifi_app_send_msg( WIFI_APP_MSG_CONNECTING_FROM_HTTP_SERVER );

  // free the allocated memory
  free(ssid_str);
  free(password_str);

  return ESP_OK;
}

/*
 * wifiConnectStatus handler updates the connection status for the web page
 * @param req HTTP request for which the URI needs to be handled
 * @return ESP_OK
 */
static esp_err_t http_server_wifi_connect_status_handler(httpd_req_t *req)
{
  ESP_LOGI(TAG, "/wifiConnectStatus.json requested");
  char status_JSON[100];

  sprintf( status_JSON, "{\"wifi_connect_status\":%d}", g_wifi_connect_status );

  httpd_resp_set_type(req, "application/json" );
  httpd_resp_send(req, status_JSON, strlen(status_JSON) );

  return ESP_OK;
}

/*
 * wifiConnectInfo handler updates the web page with the connection information
 * @param req HTTP request for which the URI needs to be handled
 * @return ESP_OK
 */
static esp_err_t http_server_get_wifi_connect_info_handler(httpd_req_t *req)
{
  ESP_LOGI(TAG, "/wifiConnectInfo.json Requested");

  char ip_info_JSON[200] = { 0 };
  // to be on safer size let's clear the local array, to make sure it doesn't
  // contain any garbage data
  memset(ip_info_JSON, 0x00, sizeof(ip_info_JSON));

  // todo: this macro is already present in ip4_addr.h file, but somehow I am
  // not able to include it, I will investigate it later
  #define IP4ADDR_STRLEN_MAX  16

  char ip[IP4ADDR_STRLEN_MAX];
  char netmask[IP4ADDR_STRLEN_MAX];
  char gateway[IP4ADDR_STRLEN_MAX];

  if( g_wifi_connect_status == HTTP_WIFI_STATUS_CONNECT_SUCCESS )
  {
    wifi_ap_record_t wifi_data;
    ESP_ERROR_CHECK( esp_wifi_sta_get_ap_info(&wifi_data) );
    char *ssid = (char*)wifi_data.ssid;

    esp_netif_ip_info_t ip_info;
    // get interface's IP address information
    ESP_ERROR_CHECK( esp_netif_get_ip_info(esp_netif_sta, &ip_info) );
    // convert this to human readable form
    esp_ip4addr_ntoa( &ip_info.ip, ip, IP4ADDR_STRLEN_MAX );
    esp_ip4addr_ntoa( &ip_info.netmask, netmask, IP4ADDR_STRLEN_MAX );
    esp_ip4addr_ntoa( &ip_info.gw, gateway, IP4ADDR_STRLEN_MAX );

    sprintf(ip_info_JSON, "{\"ip\":\"%s\",\"netmask\":\"%s\",\"gw\":\"%s\",\"ap\":\"%s\"}", \
            ip, netmask, gateway, ssid );
  }
  else
  {
    ESP_LOGI(TAG, "WiFi Connection is not Successful");
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, ip_info_JSON, strlen(ip_info_JSON));

  return ESP_OK;
}

/*
 * wifiDisconnect handler responds by sending a message to the WiFi Application
 * to disconnect
 * @param req HTTP request for which the URI needs to be handled
 * @return ESP_OK
 */
static esp_err_t http_server_wifi_disconnect_json_handler(httpd_req_t *req)
{
  ESP_LOGI(TAG, "wifiDisconnect.json requested");

  wifi_app_send_msg(WIFI_APP_MSG_USR_REQUESTED_STA_DISCONNECT);
  return ESP_OK;
}

/*
 * localTime handler responds by sending the local time
 * @param req HTTP request for which the URI needs to be handled
 * @return ESP_OK
 */
static esp_err_t http_server_get_local_time_handler(httpd_req_t *req)
{
  ESP_LOGI(TAG, "localTime.json requested");

  char local_time_JSON[100] = { 0 };

  if( g_is_local_time_set )
  {
    sprintf(local_time_JSON, "{\"time\":\"%s\"}", sntp_time_sync_get_time());
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, local_time_JSON, strlen(local_time_JSON));

  return ESP_OK;
}

/*
 * apSSID handler responds by sending the ESP32 Access Point SSID
 * @param req HTTP request for which the URI needs to be handled
 * @return ESP_OK
 */
static esp_err_t http_server_get_ap_ssid_handler(httpd_req_t *req)
{
  ESP_LOGI(TAG, "apSSID.json requested");

  char ap_ssid_JSON[50] = { 0 };

  wifi_config_t *wifi_config = wifi_app_get_wifi_config();
  esp_wifi_get_config(WIFI_IF_AP, wifi_config);
  char *ssid = (char*)wifi_config->ap.ssid;

  sprintf(ap_ssid_JSON, "{\"ssid\":\"%s\"}", ssid);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, ap_ssid_JSON, strlen(ap_ssid_JSON));

  return ESP_OK;
}

/*
 * Check the fw_update_status and creates the fw_update_reset time if the
 * fw_update_status is true
 */
static void http_server_fw_update_reset_timer(void)
{
  if( fw_update_status == OTA_UPDATE_SUCCESSFUL )
  {
    ESP_LOGI(TAG, "http_server_fw_update_reset_timer: FW Update successful starting FW update reset timer");
    // Give the web page a chance to receive an acknowledge back and initialize the timer
    ESP_ERROR_CHECK(esp_timer_create(&fw_update_reset_args, &fw_update_reset));
    ESP_ERROR_CHECK(esp_timer_start_once(fw_update_reset, 8*1000*1000));
  }
  else
  {
    ESP_LOGI(TAG, "http_server_fw_update_reset_timer: FW Update unsuccessful");
  }
}
