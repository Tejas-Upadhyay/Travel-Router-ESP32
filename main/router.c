#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static esp_netif_t *ap, *sta;
static EventGroupHandle_t events;
static nvs_handle_t storage;
static wifi_config_t upstream;
static esp_netif_ip_info_t local_ip;
static const char *TAG = "router";
#define ONLINE BIT0
extern const char page_start[] asm("_binary_index_html_start");
extern const char page_end[] asm("_binary_index_html_end");

// Serve management only on the personal-network interface, never the hotel's IP.
static bool allowed(httpd_req_t *req, bool mutation)
{
    struct sockaddr_in local = {0};
    socklen_t len = sizeof(local);
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&local, &len) != 0 ||
        local.sin_family != AF_INET || local.sin_addr.s_addr != local_ip.ip.addr) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Use the personal Wi-Fi");
        return false;
    }
    // Cross-origin browsers cannot send this header without a CORS preflight.
    char header[8];
    if (mutation && (httpd_req_get_hdr_value_str(req, "X-Router-Setup", header, sizeof(header)) != ESP_OK || strcmp(header, "1"))) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Missing setup header");
        return false;
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char host[40];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK ||
        (strcmp(host, "192.168.77.1") && strcmp(host, "192.168.77.1:80"))) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Open http://192.168.77.1");
        return false;
    }
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "frame-ancestors 'none'");
    return true;
}

static esp_err_t root(httpd_req_t *req)
{
    if (!allowed(req, false)) return ESP_OK;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page_start, page_end - page_start - 1);
}

static esp_err_t status(httpd_req_t *req)
{
    if (!allowed(req, false)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, (xEventGroupGetBits(events) & ONLINE) ?
        "{\"connected\":true}" : "{\"connected\":false}");
}

static esp_err_t scan(httpd_req_t *req)
{
    if (!allowed(req, true)) return ESP_OK;
    wifi_scan_config_t config = {0};
    if (esp_wifi_scan_start(&config, true) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Busy connecting; try again");
    wifi_ap_record_t records[20];
    uint16_t count = 20;
    esp_err_t err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK) {
        esp_wifi_clear_ap_list();
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
    }
    cJSON *list = cJSON_CreateArray();
    if (!list) return ESP_ERR_NO_MEM;
    for (int i = 0; i < count; i++) {
        char name[33] = {0};
        memcpy(name, records[i].ssid, 32);
        if (name[0]) cJSON_AddItemToArray(list, cJSON_CreateString(name));
    }
    char *json = cJSON_PrintUnformatted(list);
    cJSON_Delete(list);
    if (!json) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return err;
}

static void reboot(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t connect_network(httpd_req_t *req)
{
    if (!allowed(req, true)) return ESP_OK;
    char body[512];
    if (req->content_len <= 0 || req->content_len >= sizeof(body))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request length");
    int received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) return ESP_FAIL;
        received += n;
    }
    body[received] = 0;
    cJSON *json = cJSON_Parse(body);
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    cJSON *password = cJSON_GetObjectItemCaseSensitive(json, "password");
    bool valid = cJSON_IsString(ssid) && cJSON_IsString(password);
    size_t sl = valid ? strlen(ssid->valuestring) : 0;
    size_t pl = valid ? strlen(password->valuestring) : 0;
    if (!valid || sl < 1 || sl > 32 || pl > 63 || (pl > 0 && pl < 8)) {
        cJSON_Delete(json);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Wi-Fi name must be 1–32 bytes; password empty or 8–63 bytes");
    }
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid->valuestring, sl);
    memcpy(config.sta.password, password->valuestring, pl);
    cJSON_Delete(json);
    esp_err_t err = nvs_set_blob(storage, "upstream", &config, sizeof(config));
    if (err == ESP_OK) err = nvs_commit(storage);
    if (err != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save settings");
    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_sendstr(req, "{\"saved\":true}");
    if (xTaskCreate(reboot, "restart", 2048, NULL, 5, NULL) != pdPASS)
        ESP_LOGE(TAG, "Settings saved; manually restart router to apply");
    return err;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        ESP_ERROR_CHECK(esp_netif_napt_enable(ap));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(events, ONLINE);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
            // New DHCP leases receive the hotel's DNS, including captive-portal DNS.
            ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap));
            uint8_t offer_dns = 2;
            ESP_ERROR_CHECK(esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET,
                ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns)));
            ESP_ERROR_CHECK(esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns));
            ESP_ERROR_CHECK(esp_netif_dhcps_start(ap));
        }
        xEventGroupSetBits(events, ONLINE);
        ESP_LOGI(TAG, "Hotel network connected (portal login may still be required)");
    }
}

static void reconnect(void *arg)
{
    while (true) {
        if (upstream.sta.ssid[0] && !(xEventGroupGetBits(events) & ONLINE))
            esp_wifi_connect();
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}

void app_main(void)
{
    // Preserve existing data on NVS errors instead of silently erasing credentials.
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nvs_open("travel", NVS_READWRITE, &storage));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    events = xEventGroupCreate();
    assert(events);
    ap = esp_netif_create_default_wifi_ap();
    sta = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap));
    // Use a less common subnet to reduce hotel subnet collisions.
    esp_netif_set_ip4_addr(&local_ip.ip, 192, 168, 77, 1);
    esp_netif_set_ip4_addr(&local_ip.gw, 192, 168, 77, 1);
    esp_netif_set_ip4_addr(&local_ip.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap, &local_ip));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
    wifi_config_t personal = {0};
    snprintf((char *)personal.ap.ssid, sizeof(personal.ap.ssid), "Travel-%02X%02X%02X", mac[3], mac[4], mac[5]);
    const char *secret = "travel123";
    memcpy(personal.ap.password, secret, strlen(secret));
    personal.ap.ssid_len = strlen((char *)personal.ap.ssid);
    personal.ap.authmode = WIFI_AUTH_WPA2_PSK;
    personal.ap.max_connection = 4;
    personal.ap.channel = 1;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &personal));
    size_t len = sizeof(upstream);
    esp_err_t err = nvs_get_blob(storage, "upstream", &upstream, &len);
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_ERROR_CHECK(err);
        assert(len == sizeof(upstream));
    }
    if (upstream.sta.ssid[0]) ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &upstream));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_netif_set_default_netif(sta));
    httpd_handle_t server;
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.stack_size = 8192;
    ESP_ERROR_CHECK(httpd_start(&server, &http));
    httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = scan},
        {.uri = "/api/connect", .method = HTTP_POST, .handler = connect_network},
    };
    for (int i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    if (xTaskCreate(reconnect, "reconnect", 3072, NULL, 5, NULL) != pdPASS)
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    ESP_LOGI(TAG, "Personal Wi-Fi: %s", personal.ap.ssid);
    // Printed locally over USB so a first-time owner can join; never served over HTTP.
    ESP_LOGI(TAG, "Personal Wi-Fi password: %s", secret);
    ESP_LOGI(TAG, "Setup: http://192.168.77.1");
}
