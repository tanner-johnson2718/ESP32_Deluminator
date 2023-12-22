#include <stdint.h>
#include <string.h>
#include "HD44780.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_log.h"
#include "repl.h"
#include "esp_event.h"
#include "user_interface.h"

static const char* TAG = "UI";

#define USE_LCD 0

static user_interface_conf_t conf;

ESP_EVENT_DEFINE_BASE(USER_INPUT_EVENT);

//*****************************************************************************
// BUTTON PRVATE
//*****************************************************************************

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    ESP_ERROR_CHECK(esp_event_post(USER_INPUT_EVENT, BUTTON_PUSHED, NULL, 0, 0));
}

void button_push_handler(void* handler_args, esp_event_base_t base, int32_t id, void* event_data)
{
    printf("YO from button handler\n");
}

static void init_button(void)
{
    gpio_config_t gpio_conf = {};
    gpio_conf.mode = GPIO_MODE_INPUT;
    gpio_conf.pin_bit_mask = (1ULL << conf.button_pin);
    gpio_conf.pull_up_en = 1;
    gpio_conf.intr_type = GPIO_INTR_ANYEDGE;

    gpio_config(&gpio_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(conf.button_pin, gpio_isr_handler, NULL);
    ESP_LOGI(TAG, "GPIO PIN %d ISR registered for ui button", conf.button_pin);

    ESP_ERROR_CHECK(esp_event_handler_register(USER_INPUT_EVENT, BUTTON_PUSHED, button_push_handler, NULL));

}

static int do_toggle_button(int argc, char** argv)
{
    ESP_ERROR_CHECK(esp_event_post(USER_INPUT_EVENT, BUTTON_PUSHED, NULL, 0, 0));
    return 0;
}

//*****************************************************************************
// LCD PRIVATE
//*****************************************************************************

#if USE_LCD

    static void init_lcd(void)
    {
        LCD_init(conf.lcd_addr, conf.lcd_sda_pin, conf.lcd_scl_pin, conf.lcd_num_rows, conf.lcd_num_cols);
        LCD_home();
        LCD_clearScreen();
        LCD_writeStr("Hello World!!");
        ESP_LOGI(TAG, "%d by %d I2C LCD inited", conf.lcd_num_rows, conf.lcd_num_cols);
        ESP_LOGI(TAG, "SDA=%d   SCL=%d   ADDR=%x", conf.lcd_sda_pin, conf.lcd_scl_pin, conf.lcd_addr);
    }

#endif

//*****************************************************************************
// PUBLIC API
//*****************************************************************************

void init_user_interface(user_interface_conf_t* _conf)
{
    memcpy(&conf, _conf, sizeof(user_interface_conf_t));
    init_button();
    
    #if USE_LCD
        init_lcd();
    #endif
}

void register_user_interface(void)
{
    register_no_arg_cmd("toggle_button", "Registers change of state on main input button", &do_toggle_button);
}