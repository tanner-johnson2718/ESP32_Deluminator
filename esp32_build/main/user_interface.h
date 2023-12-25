#pragma once

#include "esp_event.h"

ESP_EVENT_DECLARE_BASE(USER_INPUT_EVENT);

enum {                                       
    BUTTON_PUSHED
};

struct user_interface
{
    uint8_t lcd_addr;
    uint8_t lcd_sda_pin;
    uint8_t lcd_scl_pin;
    uint8_t lcd_num_row;
    uint8_t lcd_num_col;
    uint8_t button_pin;
    uint32_t i2c_clk_speed;
    uint8_t rot_a_pin;
    uint8_t rot_b_pin;
} typedef user_interface_conf_t;

void init_user_interface(user_interface_conf_t* conf);
void register_user_interface(void);