#include <sys/types.h>
#include "bike_common.h"
#include "esp_log.h"
#include "nvs_flash.h"

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

static bool nvs_inited = false;

esp_err_t common_init_nvs() {
    if (nvs_inited) {
        return ESP_OK;
    }

    nvs_inited = true;
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    return ret;
}

static void ngx_unescape_uri(u_char **dst, u_char **src, size_t size, unsigned int type) {
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

void uri_decode(char *dest, const char *src, size_t len) {
    if (!src || !dest) {
        return;
    }

    unsigned char *src_ptr = (unsigned char *) src;
    unsigned char *dst_ptr = (unsigned char *) dest;
    ngx_unescape_uri(&dst_ptr, &src_ptr, len, NGX_UNESCAPE_URI);
}

void print_bytes(const uint8_t *bytes, int len) {
    int i;

    for (i = 0; i < len; i++) {
        printf("%s0x%02x", i != 0 ? ":" : "", bytes[i]);
    }

    printf("\n");
}