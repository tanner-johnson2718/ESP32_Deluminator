#include <stdio.h>
#include <string.h>

#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "repl_mux.h"

#define TTL CONFIG_REPL_MUX_WAIT_MS / portTICK_PERIOD_MS
#define UART_Q 0
#define NET_Q 1

static uint8_t queue_active[CONFIG_REPL_MUX_N_QUEUES] = {0};
static QueueHandle_t qs[CONFIG_REPL_MUX_N_QUEUES];

//*****************************************************************************
// UART Q Consumer 
//*****************************************************************************

static void uart_consumer_task_func(void* args)
{
    repl_mux_message_t msg;
    while(1)
    {
        if(xQueueReceive(qs[UART_Q], &msg, portMAX_DELAY))
        {
            printf(msg.log_msg);;
        }
    }

    // Always alive
}


//*****************************************************************************
// Net Q Consumer
//*****************************************************************************

static void net_consumer_task_func(void* args)
{
    repl_mux_message_t msg;
    while(1)
    {
        if(xQueueReceive(qs[NET_Q], &msg, portMAX_DELAY))
        {
            printf(msg.log_msg);
        }
    }

    // Always alive
}

//*****************************************************************************
// REPL MUX Q Publisher
//*****************************************************************************

static int log_publisher(const char* string, va_list arg_list)
{
    uint8_t i;
    repl_mux_message_t msg;
    for(i = 0; i < CONFIG_REPL_MUX_N_QUEUES; ++i)
    {
        if(queue_active[i])
        {
            vsnprintf(msg.log_msg, CONFIG_REPL_MUX_MAX_LOG_MSG, string, arg_list);
            xQueueSend(qs[i], (void*) &msg, TTL);
        }
    }

    return 0;
}

//*****************************************************************************
// API Funcs
//*****************************************************************************

esp_err_t repl_mux_init(void)
{
    uint8_t i;
    for(i = 0; i < CONFIG_REPL_MUX_N_QUEUES; ++i)
    {
        qs[i] = xQueueCreate(CONFIG_REPL_MUX_Q_SIZE, sizeof(repl_mux_message_t));
        if(qs[i] == 0)
        {
            return ESP_ERR_NO_MEM;
        }

        queue_active[i] = 1;
    }

    TaskHandle_t h;
    xTaskCreate(uart_consumer_task_func,
                "UART repl mux consumer", 
                CONFIG_REPL_MUX_STACK_SIZE, 
                NULL, 
                CONFIG_REPL_MUX_CONSUMER_PRIO,
                &h);
    assert(h);

    xTaskCreate(net_consumer_task_func,
                "NET repl mux consumer",
                CONFIG_REPL_MUX_STACK_SIZE, 
                NULL, 
                CONFIG_REPL_MUX_CONSUMER_PRIO,
                &h);
    assert(h);


    
    esp_log_set_vprintf(log_publisher);

    return ESP_OK;
}