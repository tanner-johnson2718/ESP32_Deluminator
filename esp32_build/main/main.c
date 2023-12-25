// Entry Point for the ESP32 Delum. Some Notes on design:
//
// * Should stick to an async / event oriented design
// * PINs, Max file size, etc should be preprocessor macros here
// * Every Module provides an init function that takes a module_conf_t type to pass defines
// * These conf structs get copied into the static global space of that module
// * Modules that wish to register repl commands export a register_module() func
// * This file should just serve as a configure and register of services

#include "repl.h"
#include "user_interface.h"
#include "flash_man.h"
#include "wifi.h"
#include "esp_event.h"

// FS defines
#define MOUNT_PATH "/spiffs"
#define MAX_FILES 32

// REPL defines
#define PROMPT_STR "$~>"
#define MAX_CMD_LINE_LEN 80
#define HISTORY_PATH MOUNT_PATH "/history.txt"
#define MAX_HISTORY_LEN 4096

// User Interface defines
#define SDA_PIN  25
#define SCL_PIN  26
#define BUTTON_PIN 33
#define ROT_A_PIN 32
#define ROT_B_PIN 27
#define LCD_ADDR 0x27
#define LCD_COLS 20
#define LCD_ROWS 4
#define I2C_CLK_SPEED 50000
#define MAX_NUM_UI_CMDS 32
#define MAX_UI_LOG_LINES 128

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
                                     I2C_CLK_SPEED,
                                     ROT_A_PIN,
                                     ROT_B_PIN,
                                     MAX_NUM_UI_CMDS,
                                     MAX_UI_LOG_LINES
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

    // UI add cmds
    ui_add_wifi();


    start_ui();
    start_repl();  // no return
}


// UI TEST
//uint8_t __nums[MAX_UI_LOG_LINES] = {0}; 

// void on_press_cb(uint8_t index)
// {
//     __nums[index] = (__nums[index] + 1) % 10;
//     char s[2];
//     s[1] = (char) 0;
//     s[0] = '0' + (char)__nums[index];
//     push_to_line_buffer(index, (char*) s);
// }

// void cmd_cb(void)
// {
//     uint8_t i;
//     char s[2];
//     s[1] = (char) 0;
    
//     for(i = 0; i < MAX_UI_LOG_LINES; ++i)
//     {
//         s[0] = '0' + (char)__nums[i];
//         push_to_line_buffer(i, (char*) s);
//     }
// }

// void ui_test(void)
// {
//     char s[2]; 
//     s[1] = (char) 0;
//     uint8_t i;
//     for(i = 0; i < 10; ++i)
//     {
//         s[0] = 'a' + (char) i;
//         add_ui_cmd((char*) s, cmd_cb, on_press_cb);
//     }
// }