// * Taken from [here](https://github.com/maxsydney/ESP32-HD44780)
// * Modifications from original:
//      * Made the I2C speed configurable
//      * Exported pin assignments to the Kconfig
//      * Removing pin assignments the init function
//      * removed referecences to `portTICK_RATE_MS` b/c its depreciated 
//      * replaced with `portTICK_PERIOD_MS`
// * NOTE - Not reentrant nor thread safe. All access in a concurrent 
//          environment needs to be guarded by CVs and Mutexes
// * Also 

#pragma once

void LCD_init();
void LCD_setCursor(uint8_t col, uint8_t row);
void LCD_home(void);
void LCD_clearScreen(void);
void LCD_writeChar(char c);
void LCD_writeStr(char* str); 