// Entry Point for the ESP32 Delum. Here all we do is the 4 main modules:
//
// * REPL
// * WIFI
// * UI
// * Flash
//
// Each of these modules export an init and register function for initing and
// registering repl commands. Some of them export UI commands and tasks to be
// started which get called here. All the main task does is call the
// module init, register and start commands and then the main app image is
// replaced with that of the serial console repl

#include "repl.h"
#include "user_interface.h"
#include "flash_man.h"
#include "wifi.h"
#include "esp_event.h"

void app_main(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    init_flash();
    init_user_interface();
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