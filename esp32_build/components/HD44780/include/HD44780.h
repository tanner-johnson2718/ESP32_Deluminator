// * Taken from [here](https://github.com/maxsydney/ESP32-HD44780)
// * Modifications from original:
//      * Made the I2C speed configurable
//      * Exported pin assignments to the Kconfig
//      * Removing pin assignments the init function
//      * removed referecences to `portTICK_RATE_MS` b/c its depreciated 
//      * replaced with `portTICK_PERIOD_MS`
// * NOTE - Not reentrant nor thread safe. All access in a concurrent 
//          environment needs to be guarded by CVs and Mutexes
//
// * Config - Besure to set these in menuconfig based on LCD set up
//    * LCD_COLS
//    * LCD_ROWS
//    * LCD_I2C_SPEED
//    * LCD_I2C_ADDR
//    * LCD_SDA_PIN
//    * LCD_SCL_PIN

#pragma once

//*****************************************************************************
// LCD_init) Does a reset sequence, inits the IO expander giving is I2C access
//           to the display. If this fails, other API functions will not be
//           accessible.
//
// Returns) ESP_OK on sucess else not. Error code is passed up from i2c init
//          and i2c writes.
//
//***************************************************************************** 
esp_err_t LCD_init(void);

//*****************************************************************************
// LCD_setCursor) Sets pos in RAM such that subsequent writes will go to the 
//                passed row and col.
//
// Input col) Must be between 0 and the configured max col -1. If outside this
//            range it will not fail, but just reset col to 0 and procede.
//
// Input row_ Same as col but for row and if out of range will set to max row-1
//
// Returns) ESP_OK on success or the ret code from the i2c write.
//
//*****************************************************************************
esp_err_t LCD_setCursor(uint8_t col, uint8_t row);

//*****************************************************************************
// LCD_home) Brings cursor back to 0,0
//
// Returns) ESP_OK on success or the ret code from the i2c write.
//
//*****************************************************************************
esp_err_t LCD_home(void);

//*****************************************************************************
// LCD_clearScreen) Delete all text / chars on the lcd
//
// Returns) ESP_OK on success or the ret code from the i2c write.
//
//*****************************************************************************
esp_err_t LCD_clearScreen(void);

//*****************************************************************************
// LCD_writeChar) Write a char to the screen and currently set row and col
//
// Input c) No checks really, assumes all values will no break the device,
//          garbage may appear on screen.
//
// Returns) ESP_OK on success or the ret code from the i2c write.
//
//*****************************************************************************
esp_err_t LCD_writeChar(char c);

//*****************************************************************************
// LCD_writeStr) Repreadtly calls write char, writing the string to the row
//
// Input str) Does no checks on str. Is up to user to validate string.
//
// Returns) ESP_OK on success or the ret code from the i2c write.
//
//*****************************************************************************
esp_err_t LCD_writeStr(char* str); 