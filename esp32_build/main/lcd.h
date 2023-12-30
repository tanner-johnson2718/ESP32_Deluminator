#pragma once


void LCD_setCursor(uint8_t col, uint8_t row);
void init_lcd(void);
void LCD_clearScreen(void);
void LCD_home(void);
void LCD_writeStr(char* str);
void LCD_writeChar(char c);