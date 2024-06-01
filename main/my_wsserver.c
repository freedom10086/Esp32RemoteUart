//
// Created by yang on 2024/5/29.
//

#include "my_wsserver.h"

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include "esp_netif.h"
#include "esp_eth.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "my_file_server_common.h"

#include <esp_http_server.h>
#include <esp_check.h>
#include <esp_random.h>
#include <esp_vfs.h>

static const char *TAG = "ws_echo_server";

#define BUF_SIZE (1024)
static uint8_t uart_buff[BUF_SIZE] = {0};
static char uart_log_buff[BUF_SIZE * 3] = {0};

/* Type of Escape algorithms to be used */
#define NGX_ESCAPE_URI            (0)
#define NGX_ESCAPE_ARGS           (1)
#define NGX_ESCAPE_URI_COMPONENT  (2)
#define NGX_ESCAPE_HTML           (3)
#define NGX_ESCAPE_REFRESH        (4)
#define NGX_ESCAPE_MEMCACHED      (5)
#define NGX_ESCAPE_MAIL_AUTH      (6)

/* Type of Unescape algorithms to be used */
#define NGX_UNESCAPE_URI          (1)
#define NGX_UNESCAPE_REDIRECT     (2)

#define MY_HTTP_QUERY_KEY_MAX_LEN (64)

static char log_filepath[ESP_VFS_PATH_MAX + CONFIG_SPIFFS_OBJ_NAME_LEN];
static FILE *logfile_fd = NULL;
/*
 * Structure holding server handle
 * and internal socket fd in order
 * to use out of request send
 */
struct async_resp_arg {
    httpd_handle_t hd;
    int fd;
};

struct uart_task_arg {
    int baud_rate;
    int tx_io_num;
    int rx_io_num;
};

static TaskHandle_t uart_task_hdl2;

static esp_err_t open_log_file() {
    uint16_t rndId = esp_random() % 1000;
    struct stat file_stat;
    do {
        struct timeval tv;
        // 使用 gettimeofday 获取当前时间
        if (gettimeofday(&tv, NULL) == -1) {
            perror("gettimeofday");
            return EXIT_FAILURE;
        }

        struct tm *timeinfo;
        // 使用 localtime 将时间戳转换为当地时间表示形式
        timeinfo = localtime(&tv.tv_sec);

        // 打印年月日时分秒
        printf("Current time: %d-%02d-%02d %02d:%02d:%02d\n",
               timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
               timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);

        sprintf(log_filepath, "%s/%02d%02d%02d%02d%02d_%d.log", FILE_SERVER_BASE_PATH,
                timeinfo->tm_mon + 1, timeinfo->tm_mday,
                timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec, rndId);

        rndId += 1;
        ESP_LOGI(TAG, "log file path %s for file", log_filepath);
    } while (stat(log_filepath, &file_stat) == 0); // == 0  file exist

    logfile_fd = fopen(log_filepath, "w");
    if (logfile_fd == NULL) {
        ESP_LOGE(TAG, "Failed to create log file : %s", log_filepath);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void print_bytes(const uint8_t *bytes, int len) {
    int i;

    for (i = 0; i < len; i++) {
        printf("%s0x%02x", i != 0 ? ":" : "", bytes[i]);
    }

    printf("\n");
}

static void init_uart(void *args) {
    struct uart_task_arg *arg = args;
    /* Configure parameters of an UART driver,
     * communication pins and install the driver */
    uart_config_t uart_config = {
            .baud_rate = arg->baud_rate,
            .data_bits = UART_DATA_8_BITS,
            .parity    = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
    };
    int intr_alloc_flags = 0;

#if CONFIG_UART_ISR_IN_IRAM
    intr_alloc_flags = ESP_INTR_FLAG_IRAM;
#endif

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, arg->tx_io_num, arg->rx_io_num, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "start uart, speed:%d tx:%d, rx:%d", arg->baud_rate, arg->tx_io_num, arg->rx_io_num);
}

static void uart_task2(void *args) {
    // Configure a temporary buffer for the incoming data
    uint8_t *data = (uint8_t *) malloc(BUF_SIZE);
    uint32_t x = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));
        ((uint32_t *) data)[0] = x;
        x++;
        // Write data back to the UART
        uart_write_bytes(UART_NUM_1, data, 8);
        print_bytes(data, 8);
    }
}

static void stop_uart_task() {
    if (uart_task_hdl2) {
        vTaskDelete(uart_task_hdl2);
    }
    uart_task_hdl2 = NULL;

    uart_driver_delete(UART_NUM_1);

    if (logfile_fd != NULL) {
        fclose(logfile_fd);
        logfile_fd = NULL;
    }
}

static void start_uart_task(int baud_rate, int tx_io_num, int rx_io_num) {
    stop_uart_task();

    open_log_file();

    struct uart_task_arg *arg = malloc(sizeof(struct uart_task_arg));
    arg->baud_rate = baud_rate;
    arg->rx_io_num = rx_io_num;
    arg->tx_io_num = tx_io_num;

    init_uart(arg);
    free(arg);

    // for test
    xTaskCreate(uart_task2, "uart_task2", 8192, NULL, 10, &uart_task_hdl2);
}

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "Handshake done, the new connection was opened");
        return ESP_OK;
    }
    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    /* Set max_len = 0 to get the frame len */
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
        return ret;
    }
    if (ws_pkt.len) {
        /* ws_pkt.len + 1 is for NULL termination as we are expecting a string */
        buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to calloc memory for buf");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        /* Set max_len = ws_pkt.len to get the frame payload */
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
            free(buf);
            return ret;
        }

        ESP_LOGI(TAG, "frame len is %d, packet type: %d message:%s", ws_pkt.len, ws_pkt.type, ws_pkt.payload);
    }

#ifdef USE_ASYNC_SEND
    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT &&
        strcmp((char *) ws_pkt.payload, "Trigger async") == 0) {
        free(buf);
        return trigger_async_send(req->handle, req);
    }
#endif

    int len;
    do {
        len = uart_read_bytes(UART_NUM_1, uart_buff, (BUF_SIZE - 1), 20 / portTICK_PERIOD_MS);
        if (len) {
            memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
            ws_pkt.type = HTTPD_WS_TYPE_BINARY;
            ws_pkt.payload = uart_buff;
            ws_pkt.len = len;

            print_bytes(uart_buff, len);

            if (logfile_fd != NULL) {
                //fwrite(uart_buff, 1, len + 1, logfile_fd);
                int i;

                struct timeval tv;
                gettimeofday(&tv, NULL);

                struct tm *timeinfo;
                timeinfo = localtime(&tv.tv_sec);

                sprintf(uart_log_buff, "\n%02d:%02d:%02d.%03ld: ", timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec,
                        tv.tv_usec / 1000);
                fwrite(uart_log_buff, sizeof(uart_log_buff[0]), strlen(uart_log_buff), logfile_fd);

                for (i = 0; i < len; i++) {
                    sprintf(uart_log_buff, "%s%02x", i != 0 ? " " : "", uart_buff[i]);
                    fwrite(uart_log_buff, sizeof(uart_log_buff[0]), strlen(uart_log_buff), logfile_fd);
                }
            }

            ret = httpd_ws_send_frame(req, &ws_pkt);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "httpd_ws_send_frame failed with %d", ret);
            }
        }
    } while (len > 0);

    free(buf);
    return ret;
}

static const httpd_uri_t ws = {
        .uri        = "/ws",
        .method     = HTTP_GET,
        .handler    = ws_handler,
        .user_ctx   = NULL,
        .is_websocket = true
};

static esp_err_t ws_uart_get_handler(httpd_req_t *req) {
    extern const unsigned char wsuart_html_start[] asm("_binary_wsuart_html_start");
    extern const unsigned char wsuart_html_end[]   asm("_binary_wsuart_html_end");
    const size_t wsuart_html_size = (wsuart_html_end - wsuart_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *) wsuart_html_start, wsuart_html_size);
    return ESP_OK;
}

void ngx_unescape_uri(u_char **dst, u_char **src, size_t size, unsigned int type) {
    u_char *d, *s, ch, c, decoded;
    enum {
        sw_usual = 0,
        sw_quoted,
        sw_quoted_second
    } state;

    d = *dst;
    s = *src;

    state = 0;
    decoded = 0;

    while (size--) {

        ch = *s++;

        switch (state) {
            case sw_usual:
                if (ch == '?'
                    && (type & (NGX_UNESCAPE_URI | NGX_UNESCAPE_REDIRECT))) {
                    *d++ = ch;
                    goto done;
                }

                if (ch == '%') {
                    state = sw_quoted;
                    break;
                }

                *d++ = ch;
                break;

            case sw_quoted:

                if (ch >= '0' && ch <= '9') {
                    decoded = (u_char) (ch - '0');
                    state = sw_quoted_second;
                    break;
                }

                c = (u_char) (ch | 0x20);
                if (c >= 'a' && c <= 'f') {
                    decoded = (u_char) (c - 'a' + 10);
                    state = sw_quoted_second;
                    break;
                }

                /* the invalid quoted character */

                state = sw_usual;

                *d++ = ch;

                break;

            case sw_quoted_second:

                state = sw_usual;

                if (ch >= '0' && ch <= '9') {
                    ch = (u_char) ((decoded << 4) + (ch - '0'));

                    if (type & NGX_UNESCAPE_REDIRECT) {
                        if (ch > '%' && ch < 0x7f) {
                            *d++ = ch;
                            break;
                        }

                        *d++ = '%';
                        *d++ = *(s - 2);
                        *d++ = *(s - 1);

                        break;
                    }

                    *d++ = ch;

                    break;
                }

                c = (u_char) (ch | 0x20);
                if (c >= 'a' && c <= 'f') {
                    ch = (u_char) ((decoded << 4) + (c - 'a') + 10);

                    if (type & NGX_UNESCAPE_URI) {
                        if (ch == '?') {
                            *d++ = ch;
                            goto done;
                        }

                        *d++ = ch;
                        break;
                    }

                    if (type & NGX_UNESCAPE_REDIRECT) {
                        if (ch == '?') {
                            *d++ = ch;
                            goto done;
                        }

                        if (ch > '%' && ch < 0x7f) {
                            *d++ = ch;
                            break;
                        }

                        *d++ = '%';
                        *d++ = *(s - 2);
                        *d++ = *(s - 1);
                        break;
                    }

                    *d++ = ch;

                    break;
                }

                /* the invalid quoted character */

                break;
        }
    }

    done:

    *dst = d;
    *src = s;
}

static void uri_decode(char *dest, const char *src, size_t len) {
    if (!src || !dest) {
        return;
    }

    unsigned char *src_ptr = (unsigned char *) src;
    unsigned char *dst_ptr = (unsigned char *) dest;
    ngx_unescape_uri(&dst_ptr, &src_ptr, len, NGX_UNESCAPE_URI);
}

esp_err_t ws_uart_config_handler(httpd_req_t *req) {

    char *buf;
    size_t buf_len;
    int speed = 9600;
    int tx = 4;
    int rx = 5;
    int stop = 0;
    int time = 0;

    /* Read URL query string length and allocate memory for length + 1,
     * extra byte for null termination */
    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "buffer alloc failed");
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            ESP_LOGI(TAG, "Found URL query => %s", buf);
            char param[MY_HTTP_QUERY_KEY_MAX_LEN], dec_param[MY_HTTP_QUERY_KEY_MAX_LEN] = {0};
            /* Get value of expected key from query string */
            if (httpd_query_key_value(buf, "speed", param, sizeof(param)) == ESP_OK) {
                ESP_LOGI(TAG, "Found URL query parameter => speed=%s", param);
                uri_decode(dec_param, param, strnlen(param, MY_HTTP_QUERY_KEY_MAX_LEN));
                ESP_LOGI(TAG, "Decoded query parameter => %s", dec_param);
                speed = atoi(dec_param);
                memset(dec_param, 0, MY_HTTP_QUERY_KEY_MAX_LEN);
            }
            if (httpd_query_key_value(buf, "tx", param, sizeof(param)) == ESP_OK) {
                ESP_LOGI(TAG, "Found URL query parameter => tx=%s", param);
                uri_decode(dec_param, param, strnlen(param, MY_HTTP_QUERY_KEY_MAX_LEN));
                ESP_LOGI(TAG, "Decoded query parameter => %s", dec_param);
                tx = atoi(dec_param);
                memset(dec_param, 0, MY_HTTP_QUERY_KEY_MAX_LEN);
            }
            if (httpd_query_key_value(buf, "rx", param, sizeof(param)) == ESP_OK) {
                ESP_LOGI(TAG, "Found URL query parameter => rx=%s", param);
                uri_decode(dec_param, param, strnlen(param, MY_HTTP_QUERY_KEY_MAX_LEN));
                ESP_LOGI(TAG, "Decoded query parameter => %s", dec_param);
                rx = atoi(dec_param);
                memset(dec_param, 0, MY_HTTP_QUERY_KEY_MAX_LEN);
            }
            if (httpd_query_key_value(buf, "stop", param, sizeof(param)) == ESP_OK) {
                ESP_LOGI(TAG, "Found URL query parameter => stop=%s", param);
                uri_decode(dec_param, param, strnlen(param, MY_HTTP_QUERY_KEY_MAX_LEN));
                ESP_LOGI(TAG, "Decoded query parameter => %s", dec_param);
                stop = atoi(dec_param);
                memset(dec_param, 0, MY_HTTP_QUERY_KEY_MAX_LEN);
            }
            if (httpd_query_key_value(buf, "time", param, sizeof(param)) == ESP_OK) {
                ESP_LOGI(TAG, "Found URL query parameter => time=%s", param);
                uri_decode(dec_param, param, strnlen(param, MY_HTTP_QUERY_KEY_MAX_LEN));
                ESP_LOGI(TAG, "Decoded query parameter => %s", dec_param);
                time = atoi(dec_param);
                memset(dec_param, 0, MY_HTTP_QUERY_KEY_MAX_LEN);

                struct timeval tv;
                // 将时间戳转换为 struct timeval 结构体
                tv.tv_sec = time;
                tv.tv_usec = 0;

                setenv("TZ", "CST-8", 1);
                tzset();

                settimeofday(&tv, NULL);
            }
        }
        free(buf);
    }

    static char json_response[128];
    char *p = json_response;
    if (stop == 0) {
        start_uart_task(speed, tx, rx);
        *p++ = '{';
        p += sprintf(p, "\"speed\":%d,", speed);
        p += sprintf(p, "\"tx\":%d,", tx);
        p += sprintf(p, "\"rx\":%d", rx);
        *p++ = '}';
        *p++ = 0;
    } else {
        stop_uart_task();
        *p++ = '{';
        p += sprintf(p, "\"stop\":%d", 1);
        *p++ = '}';
        *p++ = 0;
    }

    httpd_resp_set_type(req, "application/json");       // 设置http响应类型
    return httpd_resp_send(req, json_response, strlen(json_response));
}

httpd_uri_t uart_page_server = {
        .uri       = "/uart",
        .method    = HTTP_GET,
        .handler   = ws_uart_get_handler,
        .user_ctx  = NULL,
};

httpd_uri_t uart_config_server = {
        .uri       = "/uartconfig",
        .method    = HTTP_GET,
        .handler   = ws_uart_config_handler,
        .user_ctx  = NULL,
};

esp_err_t register_ws_handler(httpd_handle_t server) {

    httpd_register_uri_handler(server, &uart_page_server);

    httpd_register_uri_handler(server, &uart_config_server);

    ESP_LOGI(TAG, "Ws server register successful!");
    return httpd_register_uri_handler(server, &ws);
}