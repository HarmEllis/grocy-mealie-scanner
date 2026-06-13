# Demo builds swap the real HTTP client for a fixture-backed one. Exactly one
# of api_client.c / api_client_demo.c compiles, so demo fixtures never ship in
# production firmware (see CONFIG_GMS_DEMO_MODE in Kconfig.projbuild).
if(CONFIG_GMS_DEMO_MODE)
    set(API_CLIENT_SRC api_client_demo.c)
else()
    set(API_CLIENT_SRC api_client.c)
endif()

idf_component_register(
    SRCS
        main.c
        display.c
        ui.c
        ui_fonts.c
        i18n.c
        gm67.c
        gm67_proto.c
        status_led.c
        storage.c
        wifi_conn.c
        captive_dns.c
        ${API_CLIENT_SRC}
    INCLUDE_DIRS .
    REQUIRES
        driver
        esp_event
        esp_http_client
        esp_http_server
        esp_netif
        esp_timer
        esp_wifi
        app_update
        json
        lwip
        mbedtls
        nvs_flash
    EMBED_FILES
        assets/Montserrat-Medium.ttf
)
