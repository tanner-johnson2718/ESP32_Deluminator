// Pretty straight forward app. Just dump some stats on the FS and which files
// are present. Its main purpose is so one can see what APs have captured 
// handshakes and how much room is left on the device.

#include "user_interface.h"
#include "esp_spiffs.h"
#include <dirent.h>
#include <stdio.h>

#define MOUNT_PATH "/spiffs"

void lcd_fsexp_init(void)
{

    esp_err_t e = ESP_OK;
    size_t total = 0, used = 0;
    char line_buff[20];
    DIR *d;
    struct dirent *dir;
    uint8_t i;

    esp_spiffs_info(NULL, &total, &used);

    e |= ui_lock_cursor();
    e |= ui_home_screen_pos();
    snprintf(line_buff, 20, "Mount = %s", MOUNT_PATH);
    e |= ui_push_to_line_buffer(0, line_buff);
    snprintf(line_buff, 20, "%07d / %07d", used, total);
    e |= ui_push_to_line_buffer(1, line_buff);
    e |= ui_push_to_line_buffer(2, "");
    e |= ui_push_to_line_buffer(3, "");
    ESP_ERROR_CHECK_WITHOUT_ABORT(e);

    d = opendir(MOUNT_PATH);
    i = 0;
    if(d)
    {
        while((dir = readdir(d))!=NULL)
        {
           snprintf(line_buff, 20, "%.19s", dir->d_name);
           ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(2+i, line_buff));
           ++i;
        }
        closedir(d);
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_update_display());
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_unlock_cursor());
}

void lcd_fsexp_cb(uint8_t index) {}

void lcd_fsexp_fini(void){}

void lcd_wipefs_init(void)
{
    esp_err_t e = ESP_OK;
    DIR *d;
    struct dirent *dir;
    char path[33];
    char line_buff[20];

    e |= ui_lock_cursor();
    e |= ui_home_screen_pos();
    snprintf(line_buff, 20, "Mount = %s", MOUNT_PATH);
    e |= ui_push_to_line_buffer(0, line_buff);
    e |= ui_push_to_line_buffer(1, "Clearing ...");
    e |= ui_push_to_line_buffer(2, "");
    e |= ui_push_to_line_buffer(3, "");
    ESP_ERROR_CHECK_WITHOUT_ABORT(e);

    d = opendir(MOUNT_PATH);
    if(d)
    {
        while((dir = readdir(d))!=NULL)
        {
            snprintf(path, 33, "%s/%.22s", MOUNT_PATH, dir->d_name);
            remove(path);
            ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(2, "Cleared"));
        }
        closedir(d);
    }

}

void lcd_wipefs_cb(void){}
void lcd_wipefs_fini(void){}