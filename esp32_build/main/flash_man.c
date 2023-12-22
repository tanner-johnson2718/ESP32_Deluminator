#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include "esp_system.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "esp_event.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "flash_man.h"
#include "cmd_nvs.h"
#include "repl.h"

static const char* TAG = "FLASH";
static flash_conf_t conf;

//*****************************************************************************
// PRIVATE
//*****************************************************************************

static void initialize_filesystem(void)
{
    ESP_LOGI(TAG, "Initializing SPIFFS -> %s", conf.mount_path);

    esp_vfs_spiffs_conf_t _conf = {
      .base_path = conf.mount_path,
      .partition_label = NULL,
      .max_files = conf.max_file,
      .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&_conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(_conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s). Formatting...", esp_err_to_name(ret));
        esp_spiffs_format(_conf.partition_label);
        return;
    } else {
        ESP_LOGI(TAG, "%s mounted on partition size: total: %d, used: %d",conf.mount_path, total, used);
    }
}

static void initialize_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK( nvs_flash_erase() );
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static int do_part_table(int argc, char** argv)
{
    esp_partition_iterator_t part_iter;
    const esp_partition_t* part;

    part_iter = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);

    while(part_iter != NULL)
    {
        part = esp_partition_get(part_iter);

        printf("%-16s  0x%08lx  0x%08lx\n", part->label, part->address, part->size);

        part_iter = esp_partition_next(part_iter);
    }

    return 0;
}

static int do_ls(int argc, char** argv)
{ 
    
    DIR *d;
    struct dirent *dir;

    printf("%s\n", conf.mount_path);
    d = opendir(conf.mount_path);
    if(d)
    {
        while((dir = readdir(d))!=NULL)
        {
            printf("   - %s\n", dir->d_name);
        }
        closedir(d);
    }
    return 0;
}

static int do_df(int argc, char **argv)
{
    size_t total = 0, used = 0;
    esp_spiffs_info(NULL, &total, &used);
    printf("Partition size: total: %d, used: %d\n", total, used);

    return 0;
}

static int do_cat(int argc, char **argv)
{
    if(argc != 2)
    {
        printf("Usage: cat <path>\n");
        return 1;
    }

    const char* path = argv[1];
    FILE* f = fopen(path, "r");
    char line[81];
    
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file");
        return 1;
    }
    
    while(fgets(line, 81, f))
    {
        printf("%s", line);
    }

    fclose(f);
    return 0;
}

static int do_dump_event_log(int argc, char** argv)
{
    if(argc != 2)
    {
        printf("Usage: dump_event_log <file>\n");
        return 1;
    }

    const char* path = argv[1];
    FILE* f = fopen(path, "w");

    ESP_ERROR_CHECK(esp_event_dump(f));

    fclose(f);
    return 0;
}

//*****************************************************************************
// PUBLIC
//*****************************************************************************

void init_flash(flash_conf_t* _conf)
{
    memcpy(&conf, _conf, sizeof(flash_conf_t));
    initialize_nvs();
    initialize_filesystem();
}

void register_flash(void)
{
    register_no_arg_cmd("part_table", "Print the partition table", &do_part_table);
    register_no_arg_cmd("ls", "List files on spiffs", &do_ls);
    register_no_arg_cmd("df", "Disk free on spiffs", &do_df);
    register_no_arg_cmd("cat", "cat contents of file", &do_cat);
    register_no_arg_cmd("dump_event_log", "Dump the event log to disk: dump_event_log <file>", &do_dump_event_log);
    register_nvs();
}