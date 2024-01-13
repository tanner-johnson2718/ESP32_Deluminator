#pragma once

#include "esp_err.h"

struct repl_mux_message
{
    char log_msg[CONFIG_REPL_MUX_MAX_LOG_MSG];
} typedef repl_mux_message_t;

//*****************************************************************************
// blah blah
//*****************************************************************************
esp_err_t repl_mux_init(void);