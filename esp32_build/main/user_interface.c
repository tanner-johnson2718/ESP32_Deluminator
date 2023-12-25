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

#define USE_LCD 1

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
static uint8_t rot_first = 0;
static uint8_t rot_invert = 1;

static void button_short_press(void);
static void button_long_press(void);
static void rot_left(void);
static void rot_right(void);

static void LCD_setCursor(uint8_t col, uint8_t row);
static void LCD_pulseEnable(uint8_t data);
static void LCD_writeByte(uint8_t data, uint8_t mode);
static void LCD_writeNibble(uint8_t nibble, uint8_t mode);
static void LCD_clearScreen(void);
static void LCD_home(void);
static void LCD_writeStr(char* str);
static void LCD_writeChar(char c);

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
                if(rot_first == 0)
                {
                    rot_first = io_num;
                }
                ++rot_a_triggers_in_interval;
            }
            else if(io_num ==  rot_b_ISR_event)
            {
                if(rot_first == 0)
                {
                    rot_first = io_num;
                }

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

static int do_toggle_button(int argc, char** argv)
{
    xQueueSend(ui_event_q, &button_ISR_event, 0);
    return 0;
}

//*****************************************************************************
// LCD PRIVATE
//*****************************************************************************

#if USE_LCD

// LCD module defines
#define LCD_LINEONE             0x00        // start of line 1
#define LCD_LINETWO             0x40        // start of line 2
#define LCD_LINETHREE           0x14        // start of line 3
#define LCD_LINEFOUR            0x54        // start of line 4

#define LCD_BACKLIGHT           0x08
#define LCD_ENABLE              0x04               
#define LCD_COMMAND             0x00
#define LCD_WRITE               0x01

#define LCD_SET_DDRAM_ADDR      0x80
#define LCD_READ_BF             0x40

// LCD instructions
#define LCD_CLEAR               0x01        // replace all characters with ASCII 'space'
#define LCD_HOME                0x02        // return cursor to first position on first line
#define LCD_ENTRY_MODE          0x06        // shift cursor from left to right on read/write
#define LCD_DISPLAY_OFF         0x08        // turn display off
#define LCD_DISPLAY_ON          0x0C        // display on, cursor off, don't blink character
#define LCD_FUNCTION_RESET      0x30        // reset the LCD
#define LCD_FUNCTION_SET_4BIT   0x28        // 4-bit data, 2-line display, 5 x 7 font
#define LCD_SET_CURSOR          0x80        // set cursor position

// Pin mappings
// P0 -> RS
// P1 -> RW
// P2 -> E
// P3 -> Backlight
// P4 -> D4
// P5 -> D5
// P6 -> D6
// P7 -> D7

static void LCD_setCursor(uint8_t col, uint8_t row)
{
    if (row > conf.lcd_num_row - 1) {
        ESP_LOGE(TAG, "Cannot write to row %d. Please select a row in the range (0, %d)", row, conf.lcd_num_row-1);
        row = conf.lcd_num_row - 1;
    }
    uint8_t row_offsets[] = {LCD_LINEONE, LCD_LINETWO, LCD_LINETHREE, LCD_LINEFOUR};
    LCD_writeByte(LCD_SET_DDRAM_ADDR | (col + row_offsets[row]), LCD_COMMAND);
}

static void LCD_writeChar(char c)
{
    LCD_writeByte(c, LCD_WRITE);                                        // Write data to DDRAM
}

static void LCD_writeStr(char* str)
{
    while (*str) {
        LCD_writeChar(*str++);
    }
}

static void LCD_home(void)
{
    LCD_writeByte(LCD_HOME, LCD_COMMAND);
    vTaskDelay(2 / portTICK_PERIOD_MS);                                   // This command takes a while to complete
}

static void LCD_clearScreen(void)
{
    LCD_writeByte(LCD_CLEAR, LCD_COMMAND);
    vTaskDelay(2 / portTICK_PERIOD_MS);                                   // This command takes a while to complete
}

static void LCD_writeNibble(uint8_t nibble, uint8_t mode)
{
    uint8_t data = (nibble & 0xF0) | mode | LCD_BACKLIGHT;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    ESP_ERROR_CHECK(i2c_master_start(cmd));
    ESP_ERROR_CHECK(i2c_master_write_byte(cmd, (conf.lcd_addr << 1) | I2C_MASTER_WRITE, 1));
    ESP_ERROR_CHECK(i2c_master_write_byte(cmd, data, 1));
    ESP_ERROR_CHECK(i2c_master_stop(cmd));
    ESP_ERROR_CHECK(i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000/portTICK_PERIOD_MS));
    i2c_cmd_link_delete(cmd);   

    LCD_pulseEnable(data);                                              // Clock data into LCD
}

static void LCD_writeByte(uint8_t data, uint8_t mode)
{
    LCD_writeNibble(data & 0xF0, mode);
    LCD_writeNibble((data << 4) & 0xF0, mode);
}

static void LCD_pulseEnable(uint8_t data)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    ESP_ERROR_CHECK(i2c_master_start(cmd));
    ESP_ERROR_CHECK(i2c_master_write_byte(cmd, (conf.lcd_addr << 1) | I2C_MASTER_WRITE, 1));
    ESP_ERROR_CHECK(i2c_master_write_byte(cmd, data | LCD_ENABLE, 1));
    ESP_ERROR_CHECK(i2c_master_stop(cmd));
    ESP_ERROR_CHECK(i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000/portTICK_PERIOD_MS));
    i2c_cmd_link_delete(cmd);  
    ets_delay_us(1);

    cmd = i2c_cmd_link_create();
    ESP_ERROR_CHECK(i2c_master_start(cmd));
    ESP_ERROR_CHECK(i2c_master_write_byte(cmd, (conf.lcd_addr << 1) | I2C_MASTER_WRITE, 1));
    ESP_ERROR_CHECK(i2c_master_write_byte(cmd, (data & ~LCD_ENABLE), 1));
    ESP_ERROR_CHECK(i2c_master_stop(cmd));
    ESP_ERROR_CHECK(i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000/portTICK_PERIOD_MS));
    i2c_cmd_link_delete(cmd);
    ets_delay_us(500);
}

static esp_err_t I2C_init(void)
{
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = conf.lcd_sda_pin,
        .scl_io_num = conf.lcd_scl_pin,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = conf.i2c_clk_speed
    };
	i2c_param_config(I2C_NUM_0, &i2c_conf);
	i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);

    return ESP_OK;
}

void LCD_init()
{
    I2C_init();
    vTaskDelay(100 / portTICK_PERIOD_MS);                                 // Initial 40 mSec delay

    // Reset the LCD controller
    LCD_writeNibble(LCD_FUNCTION_RESET, LCD_COMMAND);                   // First part of reset sequence
    vTaskDelay(10 / portTICK_PERIOD_MS);                                  // 4.1 mS delay (min)
    LCD_writeNibble(LCD_FUNCTION_RESET, LCD_COMMAND);                   // second part of reset sequence
    ets_delay_us(200);                                                  // 100 uS delay (min)
    LCD_writeNibble(LCD_FUNCTION_RESET, LCD_COMMAND);                   // Third time's a charm
    LCD_writeNibble(LCD_FUNCTION_SET_4BIT, LCD_COMMAND);                // Activate 4-bit mode
    ets_delay_us(80);                                                   // 40 uS delay (min)

    // --- Busy flag now available ---
    // Function Set instruction
    LCD_writeByte(LCD_FUNCTION_SET_4BIT, LCD_COMMAND);                  // Set mode, lines, and font
    ets_delay_us(80); 

    // Clear Display instruction
    LCD_writeByte(LCD_CLEAR, LCD_COMMAND);                              // clear display RAM
    vTaskDelay(2 / portTICK_PERIOD_MS);                                   // Clearing memory takes a bit longer
    
    // Entry Mode Set instruction
    LCD_writeByte(LCD_ENTRY_MODE, LCD_COMMAND);                         // Set desired shift characteristics
    ets_delay_us(80); 

    LCD_writeByte(LCD_DISPLAY_ON, LCD_COMMAND);                         // Ensure LCD is set to on
}

static void init_lcd(void)
{
    LCD_init(conf.lcd_addr, conf.lcd_sda_pin, conf.lcd_scl_pin, conf.lcd_num_row, conf.lcd_num_col); 
    ESP_LOGI(TAG, "%d by %d I2C LCD inited", conf.lcd_num_row, conf.lcd_num_col);
    ESP_LOGI(TAG, "SDA=%d   SCL=%d   ADDR=%x", conf.lcd_sda_pin, conf.lcd_scl_pin, conf.lcd_addr);
}

#endif

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
        if(i < num_cmds)
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
    
    xTaskCreate(ui_event_handler, "ui event handler", 2048, NULL, UI_EVENT_HANDLER_PRIO, NULL);
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
    
    #if USE_LCD
        init_lcd();
    #endif

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

void register_user_interface(void)
{
    register_no_arg_cmd("toggle_button", "Registers change of state on main input button", &do_toggle_button);
}

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

    strcpy(current_log + (line_num*conf.lcd_num_col), line);
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