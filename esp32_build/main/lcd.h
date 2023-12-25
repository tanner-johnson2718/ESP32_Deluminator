#pragma once

void LCD_setCursor(uint8_t col, uint8_t row);
void LCD_pulseEnable(uint8_t data);
void LCD_writeByte(uint8_t data, uint8_t mode);
void LCD_writeNibble(uint8_t nibble, uint8_t mode);
void LCD_clearScreen(void);
void LCD_home(void);
void LCD_writeStr(char* str);
void LCD_writeChar(char c);
void init_lcd(user_interface_conf_t* _conf);