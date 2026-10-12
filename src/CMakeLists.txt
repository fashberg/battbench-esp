idf_component_register(SRCS "main.c" "config.c" "charger.c" "store.c" "ble.c" "net.c" "web.c" "led.c"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "web/index.html")
