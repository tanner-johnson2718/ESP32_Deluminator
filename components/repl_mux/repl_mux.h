#pragma once

// The repl mux overwrites the base logging function such that when ever any
// component logs our function gets called instead. We then copy each log 
// message to a queue to be sent out on different mediums. The two mediums we
// support at the moment are:
//
//    * UART
//    * Wifi / TCP
//
// Each medium also has an input handler that reads input and passes it to the
// console module where all registered commands are. If the input is a valid
// command then the command is ran.
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
// WARNING) Unlike other modules we do not have a global lock as it assumed
// that we are not processing input in a multithreaded environment. All 
// register calls should be done prior to even calling repl_mux_init. Moreover
// we shouldnt be recieving input from both the uart and the tcp server at the
// same time.
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