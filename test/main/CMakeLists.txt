idf_component_register(SRCS "unit_test_all.c" "test_log_buffer.c" "../../main/log_buffer.c"
                    INCLUDE_DIRS "." "../../main" "../../main/http_server"
                    PRIV_REQUIRES unity esp_http_server esp_mm
                    WHOLE_ARCHIVE)