#ifndef EPD_COMMON_H
#define EPD_COMMON_H

#include "esp_types.h"
#include "esp_event.h"
#include "esp_event_base.h"

#define max(a, b)             \
({                           \
    __typeof__ (a) _a = (a); \
    __typeof__ (b) _b = (b); \
    _a > _b ? _a : _b;       \
})

#define min(a, b)             \
({                           \
    __typeof__ (a) _a = (a); \
    __typeof__ (b) _b = (b); \
    _a < _b ? _a : _b;       \
})

esp_err_t common_init_nvs();

#endif