#include <stdio.h>
#include <string.h>

#include "esp_system.h"
#include "esp_log.h"
#include "esp_console.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "repl_mux.h"

#define TTL CONFIG_REPL_MUX_WAIT_MS / portTICK_PERIOD_MS
#define UART_Q 0
#define NET_Q 1

struct repl_mux_message
{
    char log_msg[CONFIG_REPL_MUX_MAX_LOG_MSG];
} typedef repl_mux_message_t;

static const char* TAG = "REPL MUX";

static uint8_t queue_active[CONFIG_REPL_MUX_N_QUEUES] = {0};
static QueueHandle_t qs[CONFIG_REPL_MUX_N_QUEUES];

//*****************************************************************************
// UART Q Consumer 
//*****************************************************************************

static void uart_consumer_task_func(void* args)
{
    repl_mux_message_t msg;

    queue_active[UART_Q] = 1;
    while(1)
    {
        if(xQueueReceive(qs[UART_Q], &msg, portMAX_DELAY))
        {
            printf(msg.log_msg);
        }
    }

    // Always alive
}


//*****************************************************************************
// Net Q Consumer
//*****************************************************************************

// Returns listneing soket
static int create_listening_socket()
{
    int listen_sock = -1;
    struct sockaddr_storage dest_addr;
    struct sockaddr_in *dest_addr_ip4;

    listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if(listen_sock < 0)
    {
        ESP_LOGE(TAG, "Failed to open listening socket");
        return -1;
    }

    dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
    dest_addr_ip4->sin_addr.s_addr = inet_addr(CONFIG_REPL_MUX_IP);
    dest_addr_ip4->sin_family = AF_INET;
    dest_addr_ip4->sin_port = htons(CONFIG_REPL_MUX_PORT);
    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    err |= listen(listen_sock, 1);
    
    if(err)
    {
        ESP_LOGE(TAG, "Failed to bind listening socket");
        return -1;;
    }

    ESP_LOGI(TAG, "Listening socket bound to %s:%d", CONFIG_REPL_MUX_IP, CONFIG_REPL_MUX_PORT);
    return listen_sock;
}

static int accept_client(int listen_sock)
{
    int client_socket;
    struct sockaddr_in source_addr;
    socklen_t addr_len = sizeof(source_addr);
    char ip_addr[16];

    client_socket = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
    if (client_socket < 0)
    {
        ESP_LOGE(TAG, "Unable to accept connection: %s", strerror(errno));
        return -1;
    }
    inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, ip_addr, 16);
    ESP_LOGI(TAG, "Client Connected %s - Starting Session", ip_addr);

    return client_socket;
}

static void net_consumer_task_func(void* args)
{
    repl_mux_message_t msg;
    int listen_sock = -1;
    int client_socket = -1;
    int recv_len;

    // This is considered early init task. If it fails blow everything up
    listen_sock = create_listening_socket();
    assert(listen_sock > -1);
    
    while(1)
    {
        client_socket = accept_client(listen_sock);
        if(client_socket == -1)
        {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        queue_active[NET_Q] = 1;
        while(1)
        {
            while(xQueueReceive(qs[NET_Q], &msg, 100 / portTICK_PERIOD_MS))
            {
                if(send(client_socket, msg.log_msg, strlen(msg.log_msg), 0) == 0)
                {
                    ESP_LOGI(TAG, "client disonnected");
                    break;
                }
            }

            recv_len = recv(client_socket, &msg, CONFIG_REPL_MUX_MAX_LOG_MSG, MSG_DONTWAIT);
            if(recv_len > 0)
            {
                msg.log_msg[recv_len-1] = 0;
                ESP_LOGI(TAG, "recieved cmd over net: %s", msg.log_msg);
                ESP_ERROR_CHECK_WITHOUT_ABORT(esp_console_run(msg.log_msg, &recv_len));
            }
            else if(recv_len == 0)
            {
                ESP_LOGI(TAG, "client disonnected");
                break;
            }

        }

        shutdown(client_socket, 0);
        close(client_socket);
        queue_active[NET_Q] = 0;
    }
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
            if(!xQueueSend(qs[i], (void*) &msg, TTL))
            {
                printf("REPL MUX QUEUE FULL!!\n");
            }
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
    }

    TaskHandle_t h;
    xTaskCreate(uart_consumer_task_func,
                "UART repl mux", 
                CONFIG_REPL_MUX_STACK_SIZE, 
                NULL, 
                CONFIG_REPL_MUX_CONSUMER_PRIO,
                &h);
    assert(h);

    xTaskCreate(net_consumer_task_func,
                "NET repl mux",
                CONFIG_REPL_MUX_STACK_SIZE, 
                NULL, 
                CONFIG_REPL_MUX_CONSUMER_PRIO,
                &h);
    assert(h);


    
    esp_log_set_vprintf(log_publisher);

    return ESP_OK;
}