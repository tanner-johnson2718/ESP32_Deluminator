// Nohting fancy here. init the NVS subsystem for wifi and if interfaces. 
// Init the spiffs file system to enable posix complaiant interface to fs
// Register some commands for interfacing with the fs
// Steal the cmd_nvs repl commands from the console/advanced esp-idf example

#pragma once

struct flash_conf
{
    char* mount_path;
    uint16_t max_file;
} typedef flash_conf_t;

void init_flash(void);
void register_flash(void);