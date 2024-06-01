idf_component_register(SRCS "main.c" "my_http_file_server.c" "my_http_server.c" "my_mount.c" "wifi_ap.c" "bike_common.c" "my_wsserver.c"
        EMBED_FILES "static/favicon.ico" "static/upload_script.html" "static/wsuart.html"
        INCLUDE_DIRS ".")
