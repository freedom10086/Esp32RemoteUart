//
// Created by yang on 2024/5/29.
//

#ifndef WS_ECHO_SERVER_MY_WSSERVER_H
#define WS_ECHO_SERVER_MY_WSSERVER_H

#include <esp_http_server.h>

esp_err_t register_ws_handler(httpd_handle_t server);

#endif //WS_ECHO_SERVER_MY_WSSERVER_H
