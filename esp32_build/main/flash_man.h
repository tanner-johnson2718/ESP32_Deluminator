#pragma once

struct flash_conf
{
    char* mount_path;
    uint16_t max_file;
} typedef flash_conf_t;

void init_flash(flash_conf_t* _conf);
void register_flash();