// Entry Point for the ESP32 Delum. Here all we do is the 4 main modules:
//
// * REPL
// * WIFI
// * UI
// * Flash
//
// Each of these modules export an init and register function for initing and
// registering repl commands. Some of them export UI commands and tasks to be
// started which get called here.

#include "repl.h"
#include "user_interface.h"
#include "flash_man.h"
#include "wifi.h"
#include "esp_event.h"
#include "conf.h"

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

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    init_flash(&flash_conf);
    init_repl(&repl_conf);
    init_user_interface(&ui_conf);
    init_wifi();

    /* Register repl commands */
    register_user_interface();
    register_misc_cmds();
    register_flash();
    register_wifi();

    // UI add cmds
    ui_add_wifi();

    start_ui();
    start_repl();  // no return
}