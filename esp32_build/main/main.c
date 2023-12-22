// Entry Point for the ESP32 Delum. Some Notes on design:
//
// * Should stick to an async / event oriented design
// * Should always use the default event loop semantics for async funcs
// * PINs, Max file size, etc should be preprocessor macros here
// * Every Module provides an init function that takes a module_conf_t type to pass defines
// * These conf structs get copied into the static global space of that module
// * Modules that wish to register repl commands export a register_module() func
// * This file should just serve as a configure and register of services
// * Ideally every service would be set up as an event and would also have a repl commands

#include "repl.h"
#include "user_interface.h"
#include "flash_man.h"
#include "wifi.h"

// FS defines
#define MOUNT_PATH "/spiffs"
#define MAX_FILES 32

// REPL defines
#define PROMPT_STR "$~>"
#define MAX_CMD_LINE_LEN 80
#define HISTORY_PATH MOUNT_PATH "/history.txt"
#define MAX_HISTORY_LEN 4096

// User Interface defines
#define LCD_ADDR 0x27
#define SDA_PIN  19
#define SCL_PIN  18
#define LCD_COLS 20
#define LCD_ROWS 4
#define BUTTON_PIN 13
#define I2C_CLK_SPEED 10000

// Wifi defines
#define AP_POLL_PRIO tskIDLE_PRIORITY
#define AP_POLL_DELAY_MS 500
#define DEFAULT_SCAN_LIST_SIZE 16

void app_main(void)
{

    user_interface_conf_t ui_conf = {LCD_ADDR,
                                     SDA_PIN,
                                     SCL_PIN,
                                     LCD_ROWS,
                                     LCD_COLS,
                                     BUTTON_PIN,
                                     I2C_CLK_SPEED
                                    };

    repl_conf_t repl_conf = {HISTORY_PATH, PROMPT_STR, MAX_CMD_LINE_LEN, MAX_HISTORY_LEN};

    flash_conf_t flash_conf = {MOUNT_PATH, MAX_FILES};

    wifi_conf_t wifi_conf = {AP_POLL_PRIO, AP_POLL_DELAY_MS, DEFAULT_SCAN_LIST_SIZE};

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    init_flash(&flash_conf);
    init_repl(&repl_conf);
    init_user_interface(&ui_conf);
    init_wifi(&wifi_conf);

    /* Register commands */
    register_user_interface();
    register_misc_cmds();
    register_flash();
    register_wifi();

    start_repl();  // no return
}
