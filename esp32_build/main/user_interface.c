#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "repl.h"
#include "user_interface.h"
#include "lcd.h"
#include "encoder.h"

#define UI_EVENT_Q_SIZE 32
#define UI_EVENT_HANDLER_PRIO 10
static const char* TAG = "UI";
static QueueHandle_t ui_event_q;
static rotary_encoder_t re = {0};
static user_interface_conf_t conf;

static char* cmd_list;
static command_cb_t* init_cb_list;
static command_cb_t* fini_cb_list;
static on_press_cb_t* on_press_cb_list;
static char* current_log;
static uint8_t num_cmds = 0;
static uint8_t in_menu = 1;
static uint8_t in_log_cmd_index = 0;
static uint8_t cursor_pos_on_screen = 0;
static uint8_t index_of_first_line = 0;
static uint8_t cursor_locked = 0;

static void button_short_press(void);
static void button_long_press(void);
static void rot_left(void);
static void rot_right(void);

// Main UI Event Loop. Waits for the timer going off in the rotary code to 
// poll and post events to the ui event Q. Based on the event we get notified 
// of, call the appropriate call back i.e. short_press(), long_press, etc
static void ui_event_handler(void* arg)
{
    rotary_encoder_event_t e;
    for(;;)
    {
        xQueueReceive(ui_event_q, &e, portMAX_DELAY);
        switch (e.type)
        {
            case RE_ET_BTN_PRESSED:
                ESP_LOGI(TAG, "Button pressed");
                break;
            case RE_ET_BTN_RELEASED:
                ESP_LOGI(TAG, "Button released");
                break;
            case RE_ET_BTN_CLICKED:
                button_short_press();
                ESP_LOGI(TAG, "Button clicked");
                break;
            case RE_ET_BTN_LONG_PRESSED:
                button_long_press();
                ESP_LOGI(TAG, "Looooong pressed button");
                break;
            case RE_ET_CHANGED:
                if(e.diff > 0)
                {
                    rot_right();
                    ESP_LOGI(TAG, "Rot Right");
                }
                else
                {
                    rot_left();
                    ESP_LOGI(TAG, "Rot Left");
                }
                break;
            default:
                break;
        }
        
    }
}

//*****************************************************************************
// BUTTON PRVATE
//*****************************************************************************

// On short press, if are at the main menu, reset our position on the screen
// and in the buffer and call the clicked commands init func
static void button_short_press(void)
{
    if(in_menu)
    {
        in_menu = 0;

        in_log_cmd_index = index_of_first_line + cursor_pos_on_screen;
        command_cb_t cb = init_cb_list[in_log_cmd_index];

        cursor_pos_on_screen = 0;
        index_of_first_line = 0;
        cb();
    }
    else
    {
        on_press_cb_t cb = on_press_cb_list[in_log_cmd_index];
        cb(index_of_first_line + cursor_pos_on_screen);
    }

    update_display();
}

// On a long press we exit back out into the menu screen. Call the commands 
// fini and reset our pos on the screen.
static void button_long_press(void)
{
    if(!in_menu)
    {
        command_cb_t cb = fini_cb_list[in_log_cmd_index];
        cb();
    }

    in_menu = 1;
    cursor_pos_on_screen = 0;
    index_of_first_line = 0;
    in_log_cmd_index = 0;
    cursor_locked = 0;
    update_display();
}

//*****************************************************************************
// ROT PRIVATE
//*****************************************************************************

// Scroll up
static void rot_left(void)
{
    if(cursor_locked)
    {
        return;
    }
    // left = up
    if(cursor_pos_on_screen == 0 && index_of_first_line == 0)
    {
        return;
    }

    if(cursor_pos_on_screen == 0 && index_of_first_line > 0)
    {
        index_of_first_line--;
        update_display();
        return;
    }
    
    if(cursor_pos_on_screen > 0 && cursor_pos_on_screen < conf.lcd_num_row)
    {
        // no need to update whats on screen just move cursor
        cursor_pos_on_screen--;
        update_display();
        return;
    }
}

// Scroll down. When in menu use the list of commands to scroll through. When
// not in menu use the line buffer provided that the command is pushing to.
static void rot_right(void)
{
    if(cursor_locked)
    {
        return;
    }

    uint8_t max = 0;
    if(in_menu){ max = num_cmds; }
    else       { max = conf.max_log_lines; }

    // right = down
    if((cursor_pos_on_screen == (conf.lcd_num_row-1)) && ((index_of_first_line + conf.lcd_num_row) == (max-1)) )
    {
        return;
    }

    if((cursor_pos_on_screen == (conf.lcd_num_row-1)) && ((index_of_first_line + conf.lcd_num_row) < (max-1)))
    {
        ++index_of_first_line;
        update_display();
        return;
    }

    if((cursor_pos_on_screen < (conf.lcd_num_row-1)))
    {
        if(index_of_first_line + cursor_pos_on_screen == max - 1)
        {
            return;
        }

        ++cursor_pos_on_screen;
        update_display();
        return;
    }
}

//*****************************************************************************
// UI Interaction Private functions
//*****************************************************************************

static char* get_cmd_str(uint8_t n)
{
    if(n < num_cmds)
    {
        return &cmd_list[n*conf.lcd_num_col];
    }

    ESP_LOGE(TAG, "ERROR, tried to access cmd str that dont exist");
    return NULL;
}

static int do_rot_l(int argc, char** argv)
{
    rot_left();
    return 0;
}

static int do_rot_r(int argc, char** argv)
{
    rot_right();
    return 0;
}

static int do_press(int argc, char** argv)
{
    button_short_press();
    return 0;
}

static int do_long_press(int argc, char** argv)
{
    button_long_press();
    return 0;
}

//*****************************************************************************
// PUBLIC API
//*****************************************************************************

void init_user_interface(user_interface_conf_t* _conf)
{
    memcpy(&conf, _conf, sizeof(user_interface_conf_t));    
    
    ui_event_q = xQueueCreate(UI_EVENT_Q_SIZE, sizeof(rotary_encoder_event_t));
    xTaskCreate(ui_event_handler, "ui event handler", 4096, NULL, UI_EVENT_HANDLER_PRIO, NULL);
    assert(ui_event_q);
    ESP_LOGI(TAG, "UI Event Handler Launched");

    re.pin_a = conf.rot_a_pin;
    re.pin_b = conf.rot_b_pin;
    re.pin_btn = conf.button_pin;
    ESP_ERROR_CHECK(rotary_encoder_init(ui_event_q));
    ESP_ERROR_CHECK(rotary_encoder_add(&re));
    
    init_lcd(_conf);

    cmd_list = malloc(conf.max_num_cmds * conf.lcd_num_col);
    current_log = malloc(conf.max_log_lines * conf.lcd_num_col);
    init_cb_list = malloc(conf.max_num_cmds * sizeof(command_cb_t));
    on_press_cb_list = malloc(conf.max_num_cmds * sizeof(on_press_cb_t));
    fini_cb_list = malloc(conf.max_num_cmds * sizeof(command_cb_t));

    assert(cmd_list);
    assert(current_log);
    assert(init_cb_list);
    assert(on_press_cb_list);
    assert(fini_cb_list);

    memset(cmd_list, 0, conf.max_num_cmds * conf.lcd_num_col);
    memset(current_log, 0, conf.max_log_lines * conf.lcd_num_col);
}

void register_user_interface(void)
{
    register_no_arg_cmd("rotL", "Simulate rotating rotary left", &do_rot_l);
    register_no_arg_cmd("rotR", "Simulate rotating rotary right", &do_rot_r);
    register_no_arg_cmd("press", "Simulate short press", &do_press);
    register_no_arg_cmd("pressss", "Simulate long press", &do_long_press);
}

void add_ui_cmd(char* name, command_cb_t cmd_init, on_press_cb_t on_press_cb, command_cb_t cmd_fini)
{
    // Check len of name if its more than 19 char kick that shit back
    if(strnlen(name, conf.lcd_num_col - 1) > conf.lcd_num_col - 1)
    {
        ESP_LOGE(TAG, "UI add of cmd %s failed, too long", name);
        return;
    }

    // Check to see if we are registering too many cmds
    if(num_cmds == conf.max_num_cmds)
    {
        ESP_LOGE(TAG, "UI add of cmd %s failed, too many cmds", name);
        return;
    }

    strcpy(cmd_list + (num_cmds * conf.lcd_num_col), name);
    init_cb_list[num_cmds] = cmd_init;
    on_press_cb_list[num_cmds] = on_press_cb;
    fini_cb_list[num_cmds] = cmd_fini;
    num_cmds++;
}

// Call this once  all ui cmds have been added
void start_ui(void)
{
    in_menu = 1;
    in_log_cmd_index = 0;
    cursor_pos_on_screen = 0;
    index_of_first_line = 0;
    update_display();
}

void push_to_line_buffer(uint8_t line_num, char* line)
{
    if(line_num >= conf.max_log_lines)
    {
        ESP_LOGE(TAG, "Tried to put line outside of line buffer range");
        return;
    }

    if(strnlen(line, conf.lcd_num_col - 1) > conf.lcd_num_col - 1)
    {
        ESP_LOGE(TAG, "Log line %s too long", line);
        return;
    }

    strncpy(current_log + (line_num*conf.lcd_num_col), line, conf.lcd_num_col - 1);
}

char* get_from_line_buffer(uint8_t line_num)
{
    if(line_num >= conf.max_log_lines)
    {
        ESP_LOGE(TAG, "Tried to get line outside of line buffer range");
        return NULL;
    }
    return current_log + (line_num*conf.lcd_num_col);
}

void home_screen_pos(void)
{
    cursor_pos_on_screen = 0;
    index_of_first_line = 0;
}

static void _update_line(uint8_t row, uint8_t i, uint8_t max)
{
    char* line;

    LCD_setCursor(0, row);
    if(i == cursor_pos_on_screen + index_of_first_line)
    {
        LCD_writeChar('>');
    }
    else
    {
        LCD_writeChar(' ');
    }

    LCD_setCursor(1, row);
    if(i < max)
    {
        if(in_menu) { line = get_cmd_str(i); }
        else        { line = get_from_line_buffer(i); }

        LCD_writeStr(line);
    }
}

// based on the current cursor pos and index in either the menu or line buff,
// dump contents of the buffer to the screen
void update_display(void)
{
    LCD_home();
    LCD_clearScreen();

    uint8_t max = 0;
    if(in_menu){ max = num_cmds; }
    else { max = conf.max_log_lines; }

    // display cmds, all are gareneteed to fit on screen
    uint8_t i = index_of_first_line;
    uint8_t row = 0;
    
    for(; i < index_of_first_line + conf.lcd_num_row; ++i)
    {
        _update_line(row, i, max);
        ++row;
    }        
}

void update_line(uint8_t i)
{
    if(in_menu)
    {
        ESP_LOGE(TAG, "Called update_line from menu context");
        return;
    }

    if(i >= conf.max_log_lines)
    {
        ESP_LOGE(TAG, "Called update line with out of bounds index");
        return;
    }

    if(i < index_of_first_line || i >= index_of_first_line + 4)
    {
        ESP_LOGE(TAG, "Called update line on line not on screen");
        return;
    }

    _update_line(i - index_of_first_line, i, conf.max_log_lines);
}

void lock_cursor(void)
{
    cursor_locked = 1;
}

void unlock_cursor(void)
{
    cursor_locked = 0;
}