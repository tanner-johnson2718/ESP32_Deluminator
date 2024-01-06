#include <driver/i2c.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include "sdkconfig.h"
#include "rom/ets_sys.h"
#include <esp_log.h>

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

static char TAG[] = "LCD Driver";
static uint8_t LCD_addr = CONFIG_LCD_I2C_ADDR;
static uint8_t SDA_pin = CONFIG_LCD_SDA_PIN;
static uint8_t SCL_pin = CONFIG_LCD_SCL_PIN;
static uint8_t LCD_cols = CONFIG_LCD_COLS;
static uint8_t LCD_rows = CONFIG_LCD_ROWS;

static esp_err_t LCD_writeNibble(uint8_t nibble, uint8_t mode);
static esp_err_t LCD_writeByte(uint8_t data, uint8_t mode);
static esp_err_t LCD_pulseEnable(uint8_t nibble);

uint8_t lcd_inited = 0;
#define CHECK(x) do { esp_err_t __; if ((__ = x) != ESP_OK) return __; } while (0)

static esp_err_t I2C_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = SDA_pin,
        .scl_io_num = SCL_pin,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = CONFIG_LCD_I2C_SPEED
    };
	esp_err_t e = i2c_param_config(I2C_NUM_0, &conf);
    if(e != ESP_OK)
    {
        return e;
    }
	e = i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
    return e;
}

esp_err_t LCD_init()
{
    CHECK(I2C_init());

    vTaskDelay(100 / portTICK_PERIOD_MS);                                 // Initial 40 mSec delay

    // Reset the LCD controller
    CHECK(LCD_writeNibble(LCD_FUNCTION_RESET, LCD_COMMAND));                   // First part of reset sequence
    vTaskDelay(10 / portTICK_PERIOD_MS);                                  // 4.1 mS delay (min)
    CHECK(LCD_writeNibble(LCD_FUNCTION_RESET, LCD_COMMAND));                   // second part of reset sequence
    ets_delay_us(200);                                                  // 100 uS delay (min)
    CHECK(LCD_writeNibble(LCD_FUNCTION_RESET, LCD_COMMAND));                   // Third time's a charm
    CHECK(LCD_writeNibble(LCD_FUNCTION_SET_4BIT, LCD_COMMAND));                // Activate 4-bit mode
    ets_delay_us(80);                                                   // 40 uS delay (min)

    // --- Busy flag now available ---
    // Function Set instruction
    CHECK(LCD_writeByte(LCD_FUNCTION_SET_4BIT, LCD_COMMAND));                  // Set mode, lines, and font
    ets_delay_us(80); 

    // Clear Display instruction
    CHECK(LCD_writeByte(LCD_CLEAR, LCD_COMMAND));                              // clear display RAM
    vTaskDelay(2 / portTICK_PERIOD_MS);                                   // Clearing memory takes a bit longer
    
    // Entry Mode Set instruction
    CHECK(LCD_writeByte(LCD_ENTRY_MODE, LCD_COMMAND));                         // Set desired shift characteristics
    ets_delay_us(80); 

    CHECK(LCD_writeByte(LCD_DISPLAY_ON, LCD_COMMAND));                         // Ensure LCD is set to on

    lcd_inited = 1;

    return ESP_OK;
}

esp_err_t LCD_setCursor(uint8_t col, uint8_t row)
{
    if(!lcd_inited)
    {
        ESP_LOGE(TAG, "Called set cursor but no inited");
        return ESP_ERR_INVALID_STATE;
    }

    if (row > LCD_rows - 1) {
        ESP_LOGE(TAG, "Cannot write to row %d. Please select a row in the range (0, %d)", row, LCD_rows-1);
        row = LCD_rows - 1;
    }

    if(col > LCD_cols -1)
    {
        ESP_LOGE(TAG, "Cannot write to col %d. Please select a col in the range (0, %d)", col, LCD_cols-1);
        col = 0;
    }
    uint8_t row_offsets[] = {LCD_LINEONE, LCD_LINETWO, LCD_LINETHREE, LCD_LINEFOUR};
    CHECK(LCD_writeByte(LCD_SET_DDRAM_ADDR | (col + row_offsets[row]), LCD_COMMAND));

    return ESP_OK;
}

esp_err_t LCD_writeChar(char c)
{
    if(!lcd_inited)
    {
        ESP_LOGE(TAG, "Called write char but no inited");
        return ESP_ERR_INVALID_STATE;
    }

    CHECK(LCD_writeByte(c, LCD_WRITE));                                        // Write data to DDRAM

    return ESP_OK;
}

esp_err_t LCD_writeStr(char* str)
{
    if(!lcd_inited)
    {
        ESP_LOGE(TAG, "Called write string but no inited");
        return ESP_ERR_INVALID_STATE;
    }

    while (*str) {
        CHECK(LCD_writeChar(*str++));
    }

    return ESP_OK;
}

esp_err_t LCD_home(void)
{
    if(!lcd_inited)
    {
        ESP_LOGE(TAG, "Called home but no inited");
        return ESP_ERR_INVALID_STATE;
    }
    
    CHECK(LCD_writeByte(LCD_HOME, LCD_COMMAND));
    vTaskDelay(2 / portTICK_PERIOD_MS);                                   // This command takes a while to complete

    return ESP_OK;
}

esp_err_t LCD_clearScreen(void)
{
    if(!lcd_inited)
    {
        ESP_LOGE(TAG, "Called clear screen but no inited");
        return ESP_ERR_INVALID_STATE;
    }

    CHECK(LCD_writeByte(LCD_CLEAR, LCD_COMMAND));
    vTaskDelay(2 / portTICK_PERIOD_MS);                                   // This command takes a while to complete

    return ESP_OK;
}

static esp_err_t LCD_writeNibble(uint8_t nibble, uint8_t mode)
{
    uint8_t data = (nibble & 0xF0) | mode | LCD_BACKLIGHT;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    CHECK(i2c_master_start(cmd));
    CHECK(i2c_master_write_byte(cmd, (LCD_addr << 1) | I2C_MASTER_WRITE, 1));
    CHECK(i2c_master_write_byte(cmd, data, 1));
    CHECK(i2c_master_stop(cmd));
    CHECK(i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000/portTICK_PERIOD_MS));
    i2c_cmd_link_delete(cmd); 

    CHECK(LCD_pulseEnable(data));                                              // Clock data into LCD

    return ESP_OK;
}

static esp_err_t LCD_writeByte(uint8_t data, uint8_t mode)
{
    CHECK(LCD_writeNibble(data & 0xF0, mode));
    CHECK(LCD_writeNibble((data << 4) & 0xF0, mode));

    return ESP_OK;
}

static esp_err_t LCD_pulseEnable(uint8_t data)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    CHECK(i2c_master_start(cmd));
    CHECK(i2c_master_write_byte(cmd, (LCD_addr << 1) | I2C_MASTER_WRITE, 1));
    CHECK(i2c_master_write_byte(cmd, data | LCD_ENABLE, 1));
    CHECK(i2c_master_stop(cmd));
    CHECK(i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000/portTICK_PERIOD_MS));
    i2c_cmd_link_delete(cmd);  
    ets_delay_us(1);

    cmd = i2c_cmd_link_create();
    CHECK(i2c_master_start(cmd));
    CHECK(i2c_master_write_byte(cmd, (LCD_addr << 1) | I2C_MASTER_WRITE, 1));
    CHECK(i2c_master_write_byte(cmd, (data & ~LCD_ENABLE), 1));
    CHECK(i2c_master_stop(cmd));
    CHECK(i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000/portTICK_PERIOD_MS));
    i2c_cmd_link_delete(cmd);
    ets_delay_us(500);

    return ESP_OK;
}