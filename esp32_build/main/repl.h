// The REPL API. repl.c and repl.h provide a few things:
//   1) A mechanism for registering commands to be run from the serial repl
//   2) A place to put misc commands such as free (heap space free) that dont 
//      fit into other modules
//   3) A func to start a background task that facilitates the serial repl

// Usage)
//   - Write a func with the signature int f(int argc, char** argv)
//   - Register it with register_no_arg_cmd(...)
//   - From the main task run init_repl
//   - Register all your commands
//   - Launch the repl processor task with start_repl
//        - This replaces the current tasks image and will not return

#pragma once

#include <stdint.h>

struct repl_conf
{
    char* history_save_path;
    char* prompt; 
    uint16_t max_cmdline_length;
    uint32_t max_history_len;
} typedef repl_conf_t;

void init_repl(repl_conf_t* _conf);
void start_repl(void);
void register_no_arg_cmd(char* cmd_str, char* desc, void* func_ptr);
void register_misc_cmds(void);