#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_ota_ops.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"

#include "esp_app_format.h"
#include "nvs_flash.h"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"
#include "esp_wifi.h"

#include "my_http_server.h"
#include "my_file_server_common.h"
#include "my_wsserver.h"
#include "bike_common.h"

static const char *TAG = "http_server";

#define BUFFSIZE 1024
char buff[BUFFSIZE + 1] = {0};

typedef struct {
    httpd_handle_t server_hdl;
} http_server_t;

http_server_t *my_http_server = NULL;

/* Handler to respond with an icon file embedded in flash.
 * Browsers expect to GET website icon at URI /favicon.ico.
 * This can be overridden by uploading file with same name */
static esp_err_t favicon_get_handler(httpd_req_t *req) {
    extern const unsigned char favicon_ico_start[] asm("_binary_favicon_ico_start");
    extern const unsigned char favicon_ico_end[]   asm("_binary_favicon_ico_end");
    const size_t favicon_ico_size = (favicon_ico_end - favicon_ico_start);
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_send(req, (const char *) favicon_ico_start, favicon_ico_size);
    return ESP_OK;
}



//static esp_err_t root_get_handler(httpd_req_t *req) {
//    httpd_resp_set_status(req, "307 Temporary Redirect");
//    httpd_resp_set_hdr(req, "Location", "/index.html");
//    httpd_resp_send(req, NULL, 0);  // Response body can be empty
//    return ESP_OK;
//}

//当前固件信息
esp_err_t current_version_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");        //跨域传输协议

    static char json_response[1024];

    esp_app_desc_t running_app_info;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_get_partition_description(running, &running_app_info);

    char *p = json_response;
    *p++ = '{';
    p += sprintf(p, "\"ota_subtype\":%d,", running->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN);     //OTA分区
    p += sprintf(p, "\"address\":%ld,", running->address);               //地址
    p += sprintf(p, "\"version\":\"%s\",", running_app_info.version);   //版本号
    p += sprintf(p, "\"date\":\"%s\",", running_app_info.date);         //日期
    p += sprintf(p, "\"time\":\"%s\"", running_app_info.time);          //时间
    *p++ = '}';
    *p++ = 0;

    httpd_resp_set_type(req, "application/json");       // 设置http响应类型
    return httpd_resp_send(req, json_response, strlen(json_response));
}

esp_err_t my_http_server_start() {
    if (my_http_server) {
        ESP_LOGE(TAG, "Http server already started");
        return ESP_ERR_INVALID_STATE;
    }

    /* Allocate memory for server data */
    my_http_server = calloc(1, sizeof(http_server_t));
    if (!my_http_server) {
        ESP_LOGE(TAG, "Failed to allocate memory for server data");
        return ESP_ERR_NO_MEM;
    }

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    /* Use the URI wildcard matching function in order to
     * allow the same handler to respond to multiple different
     * target URIs which match the wildcard scheme */
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(TAG, "Starting HTTP Server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start file server!");
        return ESP_FAIL;
    }

    my_http_server->server_hdl = server;

//    httpd_uri_t root = {
//            .uri       = "/",
//            .method    = HTTP_GET,
//            .handler   = root_get_handler,
//            .user_ctx  = my_http_server
//    };
//    httpd_register_uri_handler(server, &root);

    httpd_uri_t favicon = {
            .uri       = "/favicon.ico",
            .method    = HTTP_GET,
            .handler   = favicon_get_handler,
            .user_ctx  = my_http_server
    };
    httpd_register_uri_handler(server, &favicon);

    httpd_uri_t version = {
            .uri       = "/version",
            .method    = HTTP_GET,
            .handler   = current_version_handler,
            .user_ctx  = my_http_server
    };
    httpd_register_uri_handler(server, &version);

    register_ws_handler(server);

    ESP_ERROR_CHECK(mount_storage(FILE_SERVER_BASE_PATH, true));
    register_file_server(FILE_SERVER_BASE_PATH, server);

    return ESP_OK;
}

esp_err_t my_http_server_stop() {
    if (my_http_server == NULL) {
        ESP_LOGE(TAG, "Http server not started");
        return ESP_ERR_INVALID_STATE;
    }

    unregister_file_server(my_http_server->server_hdl);
    unmount_storage();

    httpd_stop(my_http_server->server_hdl);

    free(my_http_server);
    my_http_server = NULL;

    ESP_LOGI(TAG, "Http server stopped.");

    return ESP_OK;
}