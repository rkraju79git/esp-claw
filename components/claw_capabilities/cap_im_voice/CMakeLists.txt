idf_component_register(
    SRCS "src/cap_im_voice.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        cap_im_local
        esp_driver_i2s
        esp_driver_gpio
        esp_http_client
        esp-tls
        mbedtls
        json
        esp_timer
)
