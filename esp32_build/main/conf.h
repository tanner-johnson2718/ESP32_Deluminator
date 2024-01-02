// Global User configuration. Place any module config here that 1) has a high
// likely hood of changing or 2) is of interest to a user of the system (not
// nes. a dev).

#pragma once

//*****************************************************************************
// FS Conf
//*****************************************************************************
#define MOUNT_PATH "/spiffs"
#define MAX_FILES 32

//*****************************************************************************
// REPL Conf
//*****************************************************************************

#define PROMPT_STR "$~>"
#define MAX_CMD_LINE_LEN 80
#define HISTORY_PATH MOUNT_PATH "/history.txt"
#define MAX_HISTORY_LEN 4096

//*****************************************************************************
// UI Conf
//*****************************************************************************

#define SDA_PIN  25
#define SCL_PIN  26
#define BUTTON_PIN 33
#define ROT_A_PIN 32
#define ROT_B_PIN 27
#define LCD_ADDR 0x27
#define LCD_COLS 20
#define LCD_ROWS 4
#define I2C_CLK_SPEED 50000
#define MAX_NUM_UI_CMDS 32
#define MAX_UI_LOG_LINES 128
#define UI_EVENT_Q_SIZE 32
#define UI_EVENT_HANDLER_PRIO 10

//*****************************************************************************
// Wifi Conf
//*****************************************************************************

#define TIMER_DELAY_MS 1000
#define DEFAULT_SCAN_LIST_SIZE 16
#define EXAMPLE_ESP_WIFI_SSID "Linksys-76fc"
#define EXAMPLE_ESP_WIFI_CHANNEL 1
#define EXAMPLE_ESP_WIFI_PASS "abcd1234"
#define EXAMPLE_MAX_STA_CONN 1
#define TCP_SERVER_PRIO 5
#define TCP_SERVER_PORT 420
#define TCP_SERVER_IP "192.168.4.1"