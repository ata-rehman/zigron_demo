if (CONFIG_EXAMPLE_MODEM_DEVICE_BG96)
    set(device_srcs sock_commands_bg96.cpp)
elseif(CONFIG_EXAMPLE_MODEM_DEVICE_SIM7600)
    set(device_srcs sock_commands_sim7600.cpp)
endif()

idf_component_register(SRCS "sock_commands_bg96.cpp" "modem_client.cpp"
                            "led_mng.cpp"
                            "wifi_app.cpp"
                            "http_server.cpp"
                            "app_nvs.cpp"
                            "sntp_time_sync.cpp"
                            "sock_dce.cpp"
                            "tcp_transport_at.cpp"
                            "${device_srcs}"
                            "${component_srcs}"
                            EMBED_FILES webpage/app.css webpage/app.js webpage/favicon.ico webpage/index.html webpage/jquery-3.3.1.min.js
                        INCLUDE_DIRS ".")
