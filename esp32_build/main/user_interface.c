#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "repl.h"
#include "user_interface.h"
#include "rom/ets_sys.h"
#include "lcd.h"

static const char* TAG = "UI";
static user_interface_conf_t conf;

#define UI_EVENT_HANDLER_TIMEOUT 300
#define UI_EVENT_HANDLER_PRIO 10
#define UI_EVENT_Q_SIZE 32
#define BUTTON_SHORT_PRESS_MAX_US 1000*1000
#define BUTTON_SHORT_PRESS_MIN_US 10*1000

static QueueHandle_t ui_event_q;
static uint8_t button_ISR_event = 0;
static uint8_t rot_a_ISR_event = 1;
static uint8_t rot_b_ISR_event = 2;

static void button_short_press(void);
static void button_long_press(void);
static void rot_left(void);
static void rot_right(void);

void update_display(void);

static char* cmd_list;
static command_cb_t* call_back_list;
static on_press_cb_t* on_press_cb_list;
static char* current_log;
static uint8_t num_cmds = 0;
static uint8_t in_menu = 1;
static uint8_t in_log_cmd_index = 0;
static uint8_t cursor_pos_on_screen = 0;
static uint8_t index_of_first_line = 0; 

static void IRAM_ATTR ui_isr_handler(void* arg)
{
    // passes along the io isr num through arg
    xQueueSendFromISR(ui_event_q, arg, NULL);
}

static void diff(struct timeval* tv_now, struct timeval* tv_then)
{
    tv_then->tv_sec  -= tv_now->tv_sec;
    tv_then->tv_usec -= tv_now->tv_usec;
    
    if(tv_then->tv_usec < 0)
    {
        tv_then->tv_usec += 1000*1000;
        tv_then->tv_sec -= 1;
    }

    tv_then->tv_usec += 1000*1000*tv_then->tv_sec;
}

static void ui_event_handler(void* arg)
{
    uint8_t io_num;
    struct timeval tv_now, tv_then;
    uint16_t button_presses_in_interval = 0;
    uint16_t rot_a_triggers_in_interval = 0;
    uint16_t rot_b_triggers_in_interval = 0;

    uint8_t rot_first = 0;
    uint8_t rot_invert = 1;

    for(;;)
    {
        if(xQueueReceive(ui_event_q, &io_num, UI_EVENT_HANDLER_TIMEOUT / portTICK_PERIOD_MS)) 
        {
            if(io_num == button_ISR_event)
            {
                if(button_presses_in_interval == 0)
                {
                    gettimeofday(&tv_now, NULL);
                }

                ++button_presses_in_interval;
            }
            else if(io_num == rot_a_ISR_event)
            {
                if(rot_first == 0) { rot_first = io_num; }
                ++rot_a_triggers_in_interval;
            }
            else if(io_num ==  rot_b_ISR_event)
            {
                if(rot_first == 0) { rot_first = io_num; }
                ++rot_b_triggers_in_interval;
            }
        }
        else
        {
            if(button_presses_in_interval != 0)
            {
                gettimeofday(&tv_then, NULL);
                diff(&tv_now, &tv_then);
                if(tv_then.tv_usec > BUTTON_SHORT_PRESS_MIN_US && tv_then.tv_usec < BUTTON_SHORT_PRESS_MAX_US)
                {
                    button_short_press();
                    ESP_LOGI(TAG, "Short Press (%ld ms)", tv_then.tv_usec / 1000);
                }
                else
                {
                    button_long_press();
                    ESP_LOGI(TAG, "Long Press (%ld ms)", tv_then.tv_usec / 1000);
                }
            
                button_presses_in_interval = 0;
            }
            
            if(rot_a_triggers_in_interval != 0 || rot_a_triggers_in_interval != 0)
            {
                // if only one triggered then its a likely a "bump" ignore
                if(!(rot_a_triggers_in_interval && rot_b_triggers_in_interval))
                {
                    rot_first = 0;
                    rot_a_triggers_in_interval = 0;
                    rot_b_triggers_in_interval = 0;
                    continue;
                }

                if(rot_first == rot_a_ISR_event)
                {
                    if(!rot_invert)
                    {
                        rot_left();
                    }
                    else
                    {
                        rot_right();
                    }
                    
                }
                else if(rot_first == rot_b_ISR_event)
                {
                    if(!rot_invert)
                    {
                        rot_right();
                    }
                    else
                    {
                        rot_left();
                    }
                }

                rot_first = 0;
                rot_a_triggers_in_interval = 0;
                rot_b_triggers_in_interval = 0;
            }
        }
    }
}

//*****************************************************************************
// BUTTON PRVATE
//*****************************************************************************

static void button_long_press(void)
{
    in_menu = 1;
    cursor_pos_on_screen = 0;
    index_of_first_line = 0;
    in_log_cmd_index = 0;
    update_display();
}

static void button_short_press(void)
{
    if(in_menu)
    {
        in_menu = 0;
        cursor_pos_on_screen = 0;
        index_of_first_line = 0;

        in_log_cmd_index = index_of_first_line + cursor_pos_on_screen;
        command_cb_t cb = call_back_list[in_log_cmd_index];
        cb();
    }
    else
    {
        on_press_cb_t cb = on_press_cb_list[in_log_cmd_index];
        cb(index_of_first_line + cursor_pos_on_screen);
    }

    update_display();
}

static void init_button(void)
{
    gpio_isr_handler_add(conf.button_pin, ui_isr_handler, &button_ISR_event);
    ESP_LOGI(TAG, "GPIO PIN %d ISR registered for ui button", conf.button_pin);
}

//*****************************************************************************
// ROT PRIVATE
//*****************************************************************************

static void rot_left(void)
{
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

static void rot_right(void)
{
    uint8_t max = 0;
    if(in_menu)
    {
        max = num_cmds;
    }
    else
    {
        max = conf.max_log_lines;
    }

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

static void init_rot(void)
{
    gpio_isr_handler_add(conf.rot_a_pin, ui_isr_handler, &rot_a_ISR_event);
    ESP_LOGI(TAG, "GPIO PIN %d ISR registered for rot a", conf.rot_a_pin);

    gpio_isr_handler_add(conf.rot_b_pin, ui_isr_handler, &rot_b_ISR_event);
    ESP_LOGI(TAG, "GPIO PIN %d ISR registered for rot b", conf.rot_b_pin);

}

//*****************************************************************************
// UI Interaction Private functions
//*****************************************************************************

char* get_cmd_str(uint8_t n)
{
    if(n < num_cmds)
    {
        return &cmd_list[n*conf.lcd_num_col];
    }

    ESP_LOGE(TAG, "ERROR, tried to access cmd str that dont exist");
    return NULL;
}

// based on the current cursor pos, display
void update_display(void)
{
    LCD_home();
    LCD_clearScreen();

    uint8_t max = 0;
    if(in_menu)
    {
        max = num_cmds;
    }
    else
    {
        max = conf.max_log_lines;
    }

    // display cmds, all are gareneteed to fit on screen
    uint8_t i = index_of_first_line;
    uint8_t row = 0;
    char* line;
    for(; i < index_of_first_line + conf.lcd_num_row; ++i)
    {
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
            if(in_menu)
            {
                line = get_cmd_str(i);
            }
            else
            {
                line = get_from_line_buffer(i);
            }

            LCD_writeStr(line);
        }

        ++row;
    }        
}

//*****************************************************************************
// PUBLIC API
//*****************************************************************************

void init_user_interface(user_interface_conf_t* _conf)
{
    memcpy(&conf, _conf, sizeof(user_interface_conf_t));    
    
    ui_event_q = xQueueCreate(UI_EVENT_Q_SIZE, sizeof(uint8_t));
    assert(ui_event_q);
    
    xTaskCreate(ui_event_handler, "ui event handler", 4096, NULL, UI_EVENT_HANDLER_PRIO, NULL);
    ESP_LOGI(TAG, "UI Event Handler Launched");

    gpio_config_t gpio_conf = {};
    gpio_conf.mode = GPIO_MODE_INPUT;
    gpio_conf.pin_bit_mask = (1ULL << conf.button_pin) |
                             (1ULL << conf.rot_a_pin)  |
                             (1ULL << conf.rot_b_pin)  ;
    gpio_conf.pull_up_en = 1;
    gpio_conf.intr_type = GPIO_INTR_ANYEDGE;
    gpio_config(&gpio_conf);
    gpio_install_isr_service(0);

    init_button();
    init_rot();
    init_lcd(_conf);

    cmd_list = malloc(conf.max_num_cmds * conf.lcd_num_col);
    current_log = malloc(conf.max_log_lines * conf.lcd_num_col);
    call_back_list = malloc(conf.max_num_cmds * sizeof(command_cb_t));
    on_press_cb_list = malloc(conf.max_num_cmds * sizeof(on_press_cb_t));

    assert(cmd_list);
    assert(current_log);
    assert(call_back_list);
    assert(on_press_cb_list);

    memset(cmd_list, 0, conf.max_num_cmds * conf.lcd_num_col);
    memset(current_log, 0, conf.max_log_lines * conf.lcd_num_col);
}

void register_user_interface(void){}

void add_ui_cmd(char* name, command_cb_t cmd_cb, on_press_cb_t on_press_cb)
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
    call_back_list[num_cmds] = cmd_cb;
    on_press_cb_list[num_cmds] = on_press_cb;
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