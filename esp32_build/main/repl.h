// The REPL API. repl.c and repl.h provide a few things:
//   1) A mechanism for registering commands to be run from the serial repl
//   2) A place to put misc commands such as free (heap space free) that dont 
//      fit into other modules
//   3) A func to start a the repl, overtaking the main app's main task / thread

// Usage)
//   - Write a func with the signature int f(int argc, char** argv)
//   - Register it with register_no_arg_cmd(...)
//   - From the main task run init_repl
//   - Register all your commands via the register_no_arg_cmd.
//         - See some of the commands in repl.c for examples
//   - Launch the repl processor task with start_repl
//        - This replaces the current tasks image and will not return

// Notes)
//    - The built in esp_console module provides mechanisms for automatically
//      parsing cmd line args sent to your commands
//    - We enforce that all commands DO NOT use this
//    - You must parse your args yourself
//    - This is why the register function has the name register_no_arg_cmd 

#pragma once

void start_repl(void);
void register_no_arg_cmd(char* cmd_str, char* desc, void* func_ptr);
void register_misc_cmds(void);