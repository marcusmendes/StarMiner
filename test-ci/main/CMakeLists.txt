idf_component_register(SRCS "unit_test_all.c" "../../test/main/test_log_buffer.c" "../../test/main/test_i2c_bitaxe.c" "../../test/main/test_websocket.c" "../../main/log_buffer.c" "../../main/i2c_bitaxe.c" "../../main/http_server/websocket.c"
                    INCLUDE_DIRS "." "../../main" "../../main/http_server"
                    PRIV_REQUIRES unity esp_http_server esp_mm esp_driver_i2c esp_psram json
                    WHOLE_ARCHIVE)