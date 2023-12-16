/* Basic console example (esp_console_repl API)

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "esp_system.h"
#include "esp_log.h"
#include "esp_console.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cmd_system.h"
#include "cmd_nvs.h"
#include "argtable3/argtable3.h"
#include "esp_partition.h"
#include "esp_spiffs.h"


static const char* TAG = "example";
#define PROMPT_STR CONFIG_IDF_TARGET

// FS defines
#define FS_PART_NAME "storage"
#define MOUNT_PATH "/" FS_PART_NAME
#define HISTORY_PATH MOUNT_PATH "/history.txt"
#define MAX_FILES 256

static struct 
{
    struct arg_end *end;
} no_args;

static void initialize_filesystem(void)
{
    ESP_LOGI(TAG, "Initializing SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
      .base_path = MOUNT_PATH,
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);

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

    ESP_LOGI(TAG, "Performing SPIFFS_check().");
    ret = esp_spiffs_check(conf.partition_label);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS_check() failed (%s)", esp_err_to_name(ret));
        return;
    } else {
        ESP_LOGI(TAG, "SPIFFS_check() successful");
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s). Formatting...", esp_err_to_name(ret));
        esp_spiffs_format(conf.partition_label);
        return;
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
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
    esp_partition_t* part;

    part_iter = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);

    while(part_iter != NULL)
    {
        part = esp_partition_get(part_iter);

        printf("%-16s  0x%08lx  0x%08lx\n", part->label, part->address, part->size);

        part_iter = esp_partition_next(part_iter);
    }

    return 0;
}

static void register_part_table(void)
{
    no_args.end = arg_end(1);
    const esp_console_cmd_t cmd = {
        .command = "part_table",
        .help = "Print the partition table",
        .hint = NULL,
        .func = &do_part_table,
        .argtable = &no_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

static struct 
{
    struct arg_str *path;
    struct arg_end *end;
} ls_args;

static void register_one_arg_path_cmd(char* cmd_str, char* desc, void* func_ptr)
{
    ls_args.end = arg_end(1);
    ls_args.path = arg_str0(NULL, NULL, "path",
                                      desc);
    const esp_console_cmd_t cmd = {
        .command = cmd_str,
        .help = "List contents of current dir",
        .hint = NULL,
        .func = func_ptr,
        .argtable = &ls_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

static char* parse_args_one_arg_path(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&ls_args);

    if (nerrors != 0) {
        arg_print_errors(stderr, ls_args.end, argv[0]);
        return 0;
    }

    return ls_args.path->sval[0];
}

static int do_ls(int argc, char** argv)
{ 
    
    DIR *d;
    struct dirent *dir;
    char* path = parse_args_one_arg_path(argc, argv);

    printf("%s\n", path);
    d = opendir(path);
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
    char* path = parse_args_one_arg_path(argc, argv);
    size_t total = 0, used = 0;
    esp_spiffs_info(path, &total, &used);
    printf("Partition size: total: %d, used: %d", total, used);

    return 0;
}


void app_main(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    /* Prompt to be printed before each line.
     * This can be customized, made dynamic, etc.
     */
    repl_config.prompt = PROMPT_STR ">";
    repl_config.max_cmdline_length = CONFIG_CONSOLE_MAX_COMMAND_LINE_LENGTH;

    initialize_nvs();

    initialize_filesystem();
    repl_config.history_save_path = HISTORY_PATH;

    /* Register commands */
    esp_console_register_help_command();
    register_system();
    register_nvs();
    register_part_table();
    register_one_arg_path_cmd("ls", "List files in a dir", &do_ls);
    register_one_arg_path_cmd("df", "Disk free on FS", &do_df);

    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
