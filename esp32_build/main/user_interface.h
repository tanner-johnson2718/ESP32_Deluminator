#pragma once

typedef void (*command_cb_t)(void);
typedef void (*on_press_cb_t)(uint8_t line_index);

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
    uint8_t max_num_cmds;
    uint8_t max_log_lines;
} typedef user_interface_conf_t;

void init_user_interface(user_interface_conf_t* conf);
void register_user_interface(void);
void start_ui(void);
void add_ui_cmd(char* name, command_cb_t cmd_init, on_press_cb_t on_press_cb, command_cb_t cmd_fini);
char* get_from_line_buffer(uint8_t line_num);
void push_to_line_buffer(uint8_t line_num, char* line);
void update_display(void);
void home_screen_pos(void);
