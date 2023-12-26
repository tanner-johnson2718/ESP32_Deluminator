// User interface. The user interface is composed of an 2004 LCD and a standard
// rotary encoder with a push button. The UI can be in one of two global state.
// That is in_menu or not. in_menu is the init state when booting. In this 
// state we see a list commands or programs that we can run. When we click on a
// command, we are not in the in_menu state. In this state we maintain a line
// buffer and API functions for the called command to update whats on the 
// screen. When a command is added to the ui it provides an ini, fini and cb
// function. Ini inits and is called when the command is first launched. Every
// Subsequent press of the button calls the provided cb. Holding the button
// will bring you back to the menu and will call the commands fini function.

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

// Based on the current cursor pos, line index and buffer contents update the 
// lcd
void update_display(void);

// Just zeros out the cursor pos and index within the line buffer
void home_screen_pos(void);
