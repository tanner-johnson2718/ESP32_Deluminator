#pragma once

// The repl mux overwrites the base logging function such that when ever any
// component logs our function gets called instead. We then copy each log 
// message to a queue to be sent out on different mediums. The two mediums we
// support at the moment are:
//
//    * UART
//    * Wifi / TCP
//
// The flow looks something like this)
//
//                                                   socket   esp_conole_run
//             esp_log_set_vprintf                    |  ^          ^
//                    |                               V  |          |
//                    |                 |-------|    |-----------------|
//                    V            |--->| Net Q |--->| REPL TCP Server |
//             |---------------|---|    |-------|    |-----------------|
// ESP_LOG --->| log_publisher |
//             |---------------|---|    |--------|    |--------|
//                                 |--->| UART Q |--->| printf |
//                                      |--------|    |--------|
#include "esp_err.h"

//*****************************************************************************
// repl_mux_init) Create Qs for the UART and wifi mediums. Launch the consumer
//                tasks that push log messages over the UART and wifi mediums.
//                We overwrite the base logging function
//
// Returns) always ESP_OK or it crashes the system
//*****************************************************************************
esp_err_t repl_mux_init(void);