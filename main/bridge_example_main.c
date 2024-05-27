#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_netif.h"
#include "esp_netif_br_glue.h"
#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "ethernet_init.h"
#include "esp_wifi.h"
#include "esp_private/wifi.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "sdkconfig.h"
#include "esp_console.h"
#include "bridge_console_cmd.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "lwip/lwip_napt.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"  // Include for gpio_install_isr_service
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define EXAMPLE_ESP_WIFI_STA_PASSWD "12345678"
#define MIN(a, b) ((a) < (b) ? (a) : (b))
static const char *TAG_STA = "WiFi-Sta";
static const char *TAG_AP = "WiFi-AP";
static const char *TAG = "ETH";
static char wifi_ssid[32] = "";
static char wifi_password[64] = "";
static char ap_ssid[32] = "";
static char ap_password[64] = "";
static EventGroupHandle_t s_wifi_event_group;
static esp_eth_handle_t s_eth_handle = NULL;
static QueueHandle_t flow_control_queue = NULL;
static bool s_sta_is_connected = false;
static bool s_ethernet_is_connected = false;
static uint8_t common_mac_addr[6];
static httpd_handle_t s_web_server = NULL;

#define FLOW_CONTROL_QUEUE_TIMEOUT_MS (100)
#define FLOW_CONTROL_QUEUE_LENGTH (70)
#define FLOW_CONTROL_WIFI_SEND_TIMEOUT_MS (200)
typedef struct {
    void *packet;
    uint16_t length;
} flow_control_msg_t;


static esp_err_t pkt_wifi2eth(void *buffer, uint16_t len, void *eb)
{
    if (s_ethernet_is_connected) {
        if (esp_eth_transmit(s_eth_handle, buffer, len) != ESP_OK) {
            ESP_LOGE(TAG, "Ethernet send packet failed");
        }
    }
    esp_wifi_internal_free_rx_buffer(eb);
    return ESP_OK;
}
static esp_err_t pkt_eth2wifi(esp_eth_handle_t eth_handle, uint8_t *buffer, uint32_t len, void *priv)
{
    esp_err_t ret = ESP_OK;
    flow_control_msg_t msg = {
        .packet = buffer,
        .length = len
    };
    if (xQueueSend(flow_control_queue, &msg, pdMS_TO_TICKS(FLOW_CONTROL_QUEUE_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "send flow control message failed or timeout");
        free(buffer);
        ret = ESP_FAIL;
    }
    return ret;
}

static void eth2wifi_flow_control_task(void *args)
{
    flow_control_msg_t msg;
    int res = 0;
    uint32_t timeout = 0;
    while (1) {
        if (xQueueReceive(flow_control_queue, &msg, pdMS_TO_TICKS(FLOW_CONTROL_QUEUE_TIMEOUT_MS)) == pdTRUE) {
            timeout = 0;
            if (s_sta_is_connected && msg.length) {
                do {
                    vTaskDelay(pdMS_TO_TICKS(timeout));
                    timeout += 2;
                    res = esp_wifi_internal_tx(WIFI_IF_AP, msg.packet, msg.length);
                } while (res && timeout < FLOW_CONTROL_WIFI_SEND_TIMEOUT_MS);
                if (res != ESP_OK) {
                    ESP_LOGE(TAG, "WiFi send packet failed: %d", res);
                }
            }
            free(msg.packet);
        }
    }
    vTaskDelete(NULL);
}

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI("ETH", "Ethernet (%p) Link Up", eth_handle);
        ESP_LOGI("ETH", "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        esp_wifi_set_mac(WIFI_IF_AP, mac_addr);
        ESP_ERROR_CHECK(esp_wifi_start());
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI("ETH", "Ethernet (%p) Link Down", eth_handle);
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI("ETH", "Ethernet (%p) Started", eth_handle);
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI("ETH", "Ethernet (%p) Stopped", eth_handle);
        break;
    default:
        break;
    }
}
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
    	s_sta_is_connected = true;
    	ESP_LOGI("APSTA", "Pripojeno");
        esp_wifi_internal_reg_rxcb(WIFI_IF_AP, pkt_wifi2eth);
    }else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
    	s_sta_is_connected = false;
        esp_wifi_internal_reg_rxcb(WIFI_IF_AP, NULL);
    }

}
static void wifi_eventAP_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    static uint8_t s_con_cnt = 0;
    switch (event_id) {
    case WIFI_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "Wi-Fi AP got a station connected");
        if (!s_con_cnt) {
            s_sta_is_connected = true;
            esp_wifi_internal_reg_rxcb(WIFI_IF_AP, pkt_wifi2eth);
        }
        s_con_cnt++;
        break;
    case WIFI_EVENT_AP_STADISCONNECTED:
        ESP_LOGI(TAG, "Wi-Fi AP got a station disconnected");
        s_con_cnt--;
        if (!s_con_cnt) {
            s_sta_is_connected = false;
            esp_wifi_internal_reg_rxcb(WIFI_IF_AP, NULL);
        }
        break;
    default:
        break;
    }
}

/** Event handler for IP_EVENT_ETH_GOT_IP */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI("ETH", "Ethernet Got IP Address");
    ESP_LOGI("ETH", "~~~~~~~~~~~");
    ESP_LOGI("ETH", "ETHIP:" IPSTR "\r", IP2STR(&ip_info->ip));
    ESP_LOGI("ETH", "ETHMASK:" IPSTR "\r", IP2STR(&ip_info->netmask));
    ESP_LOGI("ETH", "ETHGW:" IPSTR "\r", IP2STR(&ip_info->gw));
    ESP_LOGI("ETH", "~~~~~~~~~~~");
}
// Funkce pro zpracování GET požadavku
static esp_err_t get_handler(httpd_req_t *req)
{
	const char* resp = "<!DOCTYPE html>\
	<html>\
	<head>\
	  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\
	  <style>\
	    body {\
	      font-family: Arial, sans-serif;\
	      margin: 0;\
	      padding: 0;\
	      display: flex;\
	      justify-content: center;\
	      align-items: center;\
	      height: 100vh;\
	      background-color: #f2f2f2;\
	    }\
	    .container {\
	      background-color: white;\
	      padding: 20px;\
	      box-shadow: 0px 0px 10px rgba(0, 0, 0, 0.1);\
	      border-radius: 10px;\
	      max-width: 400px;\
	      width: 100%;\
	    }\
	    h2 {\
	      text-align: center;\
	    }\
	    label {\
	      font-size: 18px;\
	      margin-bottom: 10px;\
	      display: block;\
	    }\
	    input[type=text], input[type=password] {\
	      width: 100%;\
	      padding: 10px;\
	      margin: 10px 0;\
	      display: inline-block;\
	      border: 1px solid #ccc;\
	      border-radius: 5px;\
	      box-sizing: border-box;\
	    }\
	    input[type=submit] {\
	      width: 100%;\
	      background-color: #4CAF50;\
	      color: white;\
	      padding: 14px 20px;\
	      margin: 8px 0;\
	      border: none;\
	      border-radius: 5px;\
	      cursor: pointer;\
	    }\
	    input[type=submit]:hover {\
	      background-color: #45a049;\
	    }\
	    .tab {\
	      overflow: hidden;\
	      border-bottom: 1px solid #ccc;\
	      background-color: #f1f1f1;\
	      margin-bottom: 20px;\
	    }\
	    .tab button {\
	      background-color: inherit;\
	      float: left;\
	      border: none;\
	      outline: none;\
	      cursor: pointer;\
	      padding: 14px 16px;\
	      transition: 0.3s;\
	      font-size: 17px;\
	    }\
	    .tab button:hover {\
	      background-color: #ddd;\
	    }\
	    .tab button.active {\
	      background-color: #ccc;\
	    }\
	    .tabcontent {\
	      display: none;\
	    }\
	    .tabcontent.active {\
	      display: block;\
	    }\
	  </style>\
	  <script>\
	    function openTab(evt, tabName) {\
	      var i, tabcontent, tablinks;\
	      tabcontent = document.getElementsByClassName('tabcontent');\
	      for (i = 0; i < tabcontent.length; i++) {\
	        tabcontent[i].style.display = 'none';\
	      }\
	      tablinks = document.getElementsByClassName('tablinks');\
	      for (i = 0; i < tablinks.length; i++) {\
	        tablinks[i].className = tablinks[i].className.replace(' active', '');\
	      }\
	      document.getElementById(tabName).style.display = 'block';\
	      evt.currentTarget.className += ' active';\
	    }\
	    function validateForm() {\
	      var ssid = document.forms['wifiForm']['ssid'].value;\
	      var password = document.forms['wifiForm']['password'].value;\
	      var ap_ssid = document.forms['wifiForm']['ap_ssid'].value;\
	      var ap_password = document.forms['wifiForm']['ap_password'].value;\
	      if (ssid == '' || password == '' || ap_ssid == '' || ap_password == '') {\
	        alert('All fields must be filled out');\
	        return false;\
	      }\
	      return true;\
	    }\
	    function togglePasswordVisibility() {\
	      var passwordFields = document.querySelectorAll('input[type=password]');\
	      passwordFields.forEach(field => {\
	        if (field.type === 'password') {\
	          field.type = 'text';\
	        } else {\
	          field.type = 'password';\
	        }\
	      });\
	    }\
	  </script>\
	</head>\
	<body>\
	  <div class=\"container\">\
	    <h2>WiFi Configuration</h2>\
	    <div class=\"tab\">\
	      <button class=\"tablinks\" onclick=\"openTab(event, 'STA')\" id=\"defaultOpen\">STA</button>\
	      <button class=\"tablinks\" onclick=\"openTab(event, 'AP')\">AP</button>\
	    </div>\
	    <form name=\"wifiForm\" action=\"/post\" method=\"post\" onsubmit=\"return validateForm()\">\
	      <div id=\"STA\" class=\"tabcontent\">\
	        <label for=\"ssid\">SSID:</label><br>\
	        <input type=\"text\" id=\"ssid\" name=\"ssid\"><br>\
	        <label for=\"password\">Password:</label><br>\
	        <input type=\"password\" id=\"password\" name=\"password\"><br><br>\
	        <input type=\"checkbox\" onclick=\"togglePasswordVisibility()\"> Show Password<br><br>\
	      </div>\
	      <div id=\"AP\" class=\"tabcontent\">\
	        <label for=\"ap_ssid\">AP SSID:</label><br>\
	        <input type=\"text\" id=\"ap_ssid\" name=\"ap_ssid\"><br>\
	        <label for=\"ap_password\">AP Password:</label><br>\
	        <input type=\"password\" id=\"ap_password\" name=\"ap_password\"><br><br>\
	        <input type=\"checkbox\" onclick=\"togglePasswordVisibility()\"> Show Password<br><br>\
	      </div>\
	      <input type=\"submit\" value=\"Submit\">\
	    </form>\
	  </div>\
	  <script>\
	    document.getElementById('defaultOpen').click();\
	  </script>\
	</body>\
	</html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
static void send_data_to_sta(void)
{
    esp_http_client_config_t config = {
        .url = "http://192.168.4.2/data",
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    char post_data[200];
    snprintf(post_data, sizeof(post_data), "ssid=%s&password=%s", ap_ssid, ap_password);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %lld",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}
static void restart_wifiAP(void){
	ESP_ERROR_CHECK(esp_wifi_stop());
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_eventAP_handler, NULL));
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	wifi_config_t ap_config = {
		.ap = {
			.ssid = "",
			.ssid_len = strlen(ap_ssid),
			.password = "",
			.max_connection = 4,
			.authmode = WIFI_AUTH_WPA_WPA2_PSK
		},
	};
	strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
	strncpy((char *)ap_config.ap.password, ap_password, sizeof(ap_config.ap.password));

	if (strlen((const char *)ap_config.ap.password) == 0) {
		ap_config.ap.authmode = WIFI_AUTH_OPEN;
	}
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
}
static void restart_wifi(void)
{
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
        ESP_ERROR_CHECK(esp_wifi_start() );

        wifi_config_t wifi_sta_config = {};

        strncpy((char *)wifi_sta_config.sta.ssid, wifi_ssid, sizeof(wifi_sta_config.sta.ssid) - 1);
		wifi_sta_config.sta.ssid[sizeof(wifi_sta_config.sta.ssid) - 1] = '\0';
		strncpy((char *)wifi_sta_config.sta.password, wifi_password, sizeof(wifi_sta_config.sta.password) - 1);
		wifi_sta_config.sta.password[sizeof(wifi_sta_config.sta.password) - 1] = '\0';
		wifi_sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
		wifi_sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config) );

        esp_wifi_connect();
        EventBits_t status = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, 0, 1, 10000 / portTICK_PERIOD_MS);
        if (status & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "WiFi station connected successfully");
            display_arp_table();
        }
        ESP_LOGE(TAG, "WiFi station connected failed");
}
// Funkce pro zpracování POST požadavku
static esp_err_t post_handler(httpd_req_t *req)
{
    char buf[100];
    int ret, remaining = req->content_len;
    ESP_LOGI("HTTP", "remaining %d", remaining);
    while (remaining > 0) {
        if ((ret = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)))) <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }
        remaining -= ret;
        buf[ret] = '\0';

        char *ssid_start = strstr(buf, "ssid=");
        char *password_start = strstr(buf, "password=");
        char *ap_ssid_start = strstr(buf, "ap_ssid=");
        char *ap_password_start = strstr(buf, "ap_password=");
        if (ssid_start && password_start) {
            sscanf(ssid_start, "ssid=%31[^&]", wifi_ssid);
            sscanf(password_start, "password=%63[^&]", wifi_password);
            sscanf(ap_ssid_start, "ap_ssid=%31[^&]", ap_ssid);
            sscanf(ap_password_start, "ap_password=%63s", ap_password);
        }
    }

    httpd_resp_sendstr(req, "Settings updated. Restarting the device...");
    ESP_LOGI("HTTP", "SSID %s", wifi_ssid);
    ESP_LOGI("HTTP", "PASS: %s", wifi_password);
    ESP_LOGI("HTTP", "AP_SSID %s", ap_ssid);
    ESP_LOGI("HTTP", "AP_PASS: %s", ap_password);
    ESP_LOGI("HTTP", "SSID %s , PASS: %s, AP_SSID %s, AP_PASS: %s", wifi_ssid, wifi_password,ap_ssid,ap_password);

    vTaskDelay(2000 / portTICK_PERIOD_MS);

    send_data_to_sta();
    restart_wifi();

    return ESP_OK;
}
static esp_err_t data_post_handler(httpd_req_t *req) {
    char buf[100];
    int ret, remaining = req->content_len;

    while (remaining > 0) {
        if ((ret = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)))) <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }
        remaining -= ret;
        buf[ret] = '\0';
        ESP_LOGI(TAG, "Received data: %s", buf);

        char *ssid_start = strstr(buf, "ssid=");
        char *password_start = strstr(buf, "password=");
        if (ssid_start && password_start) {
            sscanf(ssid_start, "ssid=%31[^&]", ap_ssid);
            sscanf(password_start, "password=%63s", ap_password);
            ESP_LOGI(TAG, "SSID: %s, Password: %s", ap_ssid, ap_password);
        }
    }

    httpd_resp_sendstr(req, "Data received");

    restart_wifiAP();

    return ESP_OK;
}

static void start_webserverSTA(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        ESP_LOGI(TAG, "Registering URI handlers");

        httpd_uri_t data_post = {
            .uri       = "/data",
            .method    = HTTP_POST,
            .handler   = data_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &data_post);
       }
    	ESP_LOGI(TAG, "Error starting server!");
    }
static void start_webserverAP(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 16384;
	config.max_resp_headers = 20;
	config.recv_wait_timeout = 10;
	config.send_wait_timeout = 10;

    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&s_web_server, &config) == ESP_OK) {
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_uri_t uri_get = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = get_handler,
            .user_ctx  = NULL
        };

        httpd_uri_t uri_post = {
            .uri       = "/post",
            .method    = HTTP_POST,
            .handler   = post_handler,
            .user_ctx  = NULL
        };
	   httpd_register_uri_handler(s_web_server, &uri_get);
	   httpd_register_uri_handler(s_web_server, &uri_post);
    }
}
esp_eth_handle_t spi_init() {
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CONFIG_EXAMPLE_ETH_SPI_PHY_ADDR0;
    phy_config.reset_gpio_num = CONFIG_EXAMPLE_ETH_SPI_PHY_RST0_GPIO;

    gpio_install_isr_service(0);


    spi_device_handle_t spi_handle = NULL;
    spi_bus_config_t buscfg = {
        .miso_io_num = CONFIG_EXAMPLE_ETH_SPI_MISO_GPIO,
        .mosi_io_num = CONFIG_EXAMPLE_ETH_SPI_MOSI_GPIO,
        .sclk_io_num = CONFIG_EXAMPLE_ETH_SPI_SCLK_GPIO,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(CONFIG_EXAMPLE_ETH_SPI_HOST, &buscfg, 1));
    ESP_LOGI("SPI_BUS_INIT", "Ethernet spi bus done");
    spi_device_interface_config_t spi_devcfg = {
        .mode = 0,
        .clock_speed_hz = CONFIG_EXAMPLE_ETH_SPI_CLOCK_MHZ * 1000 * 1000,
        .spics_io_num = CONFIG_EXAMPLE_ETH_SPI_CS0_GPIO,
        .queue_size = 20
    };

    eth_w5500_config_t W5500_config = ETH_W5500_DEFAULT_CONFIG(CONFIG_EXAMPLE_ETH_SPI_HOST, &spi_devcfg);
    W5500_config.int_gpio_num = CONFIG_EXAMPLE_ETH_SPI_INT0_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&W5500_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);


    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    esp_eth_driver_install(&config, &eth_handle);

    ESP_ERROR_CHECK(esp_read_mac(common_mac_addr, ESP_MAC_ETH));
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, common_mac_addr));
    ESP_LOGI("ETH_INIT", "Ethernet driver installed.");
    return eth_handle;
}
esp_netif_t *wifi_init_sta(void)
{
    esp_netif_t *esp_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_config_t wifi_sta_config = {
        .sta = {
            .ssid = "ESP_CONFIG",
            .password = "12345678",
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config) );

    ESP_LOGI("STA", "wifi_init_sta finished.");

    return esp_netif_sta;
}

esp_netif_t *wifi_init_softap(void)
{
	 ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    esp_netif_t *esp_netif_ap = esp_netif_create_default_wifi_ap();

    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid = "ESP_CONFIG",
            .ssid_len = strlen("ESP_CONFIG"),
            .password = EXAMPLE_ESP_WIFI_STA_PASSWD,
            .authmode = WIFI_AUTH_WPA2_PSK,
			.max_connection = 4,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    if (strlen(EXAMPLE_ESP_WIFI_STA_PASSWD) == 0) {
        wifi_ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));

    ESP_LOGI("AP-TEST", "wifi_init_ap finished. SSID: ESP_CONFIG");

    return esp_netif_ap;
}

static void create_eth_netif(void) {
	 uint8_t eth_port_cnt = 0;
	esp_eth_handle_t *eth_handles;
	ESP_ERROR_CHECK(example_eth_init(&eth_handles, &eth_port_cnt));
	if (eth_port_cnt > 1) {
		ESP_LOGW(TAG, "multiple Ethernet devices detected, the first initialized is to be used!");
	}
	s_eth_handle = eth_handles[0];
	free(eth_handles);
	ESP_ERROR_CHECK(esp_eth_update_input_path(s_eth_handle, pkt_eth2wifi, NULL));
	bool eth_promiscuous = true;
	ESP_ERROR_CHECK(esp_eth_ioctl(s_eth_handle, ETH_CMD_S_PROMISCUOUS, &eth_promiscuous));
	ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL));
	ESP_ERROR_CHECK(esp_eth_start(s_eth_handle));

}

static esp_err_t initialize_flow_control(void)
{
    flow_control_queue = xQueueCreate(FLOW_CONTROL_QUEUE_LENGTH, sizeof(flow_control_msg_t));
    if (!flow_control_queue) {
        ESP_LOGE(TAG, "create flow control queue failed");
        return ESP_FAIL;
    }
    BaseType_t ret = xTaskCreate(eth2wifi_flow_control_task, "flow_ctl", 2048, NULL, (tskIDLE_PRIORITY + 2), NULL);
    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "create flow control task failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}


void app_main(void)
{
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());

	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	s_wifi_event_group = xEventGroupCreate();
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
					ESP_EVENT_ANY_ID,
					&wifi_event_handler,
					NULL,
					NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
					IP_EVENT_STA_GOT_IP,
					&wifi_event_handler,
					NULL));
	//Initialize WiFi
	wifi_init_config_t cfgW = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfgW));


#if CONFIG_EXAMPLE_BR_MODE_STA
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	esp_netif_t *esp_netif_sta = wifi_init_sta();
	start_webserverSTA();
    ESP_LOGI(TAG, "Configured as STA mode");
#elif CONFIG_EXAMPLE_BR_MODE_AP
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_LOGI(TAG_AP, "ESP_WIFI_MODE_AP");
    esp_netif_t *esp_netif_ap = wifi_init_softap();

    ESP_LOGI(TAG, "Configured as AP mode");
    wifi_init_ap();
#else
    ESP_LOGE(TAG, "No valid mode selected");
#endif


    ESP_ERROR_CHECK(initialize_flow_control());

	ESP_ERROR_CHECK(esp_wifi_start());
	create_eth_netif();


}
