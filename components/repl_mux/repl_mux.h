#pragma once

// The repl mux overwrites the base logging function such that when ever any
// component logs our function gets called instead. We then copy each log 
// message to a queue to be sent out on different mediums. The two mediums we
// support at the moment are:
//
//    * UART
//    * Wifi / TCP
//
// Each medium also has an input handler that reads input. The input is
// compared to a table of registered commands. If the input matches then the
// command is run and the associated command line args are passed to the 
// command.
//
// The flow looks something like this)
//
//                                              
//             esp_log_set_vprintf
//                    |                     
//                    |                 |-------|    |--------------|
//                    V            |--->| Net Q |--->| Net Consumer |--> Sock Send
//             |---------------|---|    |-------|    |--------------|
// ESP_LOG --->| log_publisher |
//             |---------------|---|    |--------|    |---------------|
//                                 |--->| UART Q |--->| UART Consumer |--> Printf
//                                      |--------|    |---------------|
//
// 
// |------------|
// | Net Input  |---|
// |------------|   |
//                  |---> Command Table Look Up ---> Parse args ---> cmd(argc, argv)
// |------------|   |
// | UART Input |---|
// |------------|
//
// **NOTE**
//    - printf only sends traffic over UART
//    - ESP_LOGE, etc adds debug level, tag and time stamp and will send 
//      through the REPL MUX
//    - use esp_log_write for generic log messages that go through the mux
//      without adding all the extra stuff

#include "esp_err.h"

typedef int (*cmd_func_t)(int argc, char**argv);

typedef struct
{
    char name[CONFIG_REPL_MUX_NAME_LEN];
    char desc[CONFIG_REPL_MUX_DESC_LEN];
    cmd_func_t func;

} cmd_t;

//*****************************************************************************
// repl_mux_init) Create Qs for the UART and wifi mediums. Launch the consumer
//                tasks that push log messages over the UART and wifi mediums.
//                We overwrite the base logging function. The consumer tasks
//                are responible for initing the medium they wish to talk over.
//
// Returns) always ESP_OK or it crashes the system
//*****************************************************************************
esp_err_t repl_mux_init(void);


//*****************************************************************************
// repl_mux_register)  
//
//*****************************************************************************
esp_err_t repl_mux_register(char* name, char* desc, cmd_func_t func);