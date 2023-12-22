#pragma once

struct repl_conf
{
    char* history_save_path;
    char* prompt; 
    uint16_t max_cmdline_length;
} typedef repl_conf_t;

void register_no_arg_cmd(char* cmd_str, char* desc, void* func_ptr);
void init_repl(repl_conf_t* _conf);
void start_repl(void);
void register_misc_cmds(void);

