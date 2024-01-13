#include <stdio.h>
#include <string.h>
#include <dirent.h>

#include "esp_system.h"
#include "esp_log.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tcp_file_server.h"

static const char* TAG = "TCP File Server";

static TaskHandle_t handler_task;
static int client_socket;
static int listen_sock;
static char ip_addr[16];
static int running = 0;
static char MOUNT_PATH[33];

//*****************************************************************************
// TCP Server Logic
//*****************************************************************************

// returns a 1 if the error caused should reset the tcp connection
static uint8_t handle_file_req(void)
{
    uint8_t rx_buffer[33];
    int len = recv(client_socket, rx_buffer, sizeof(rx_buffer) - 1, 0);
    
    if(len < 0)
    {
        ESP_LOGE(TAG, "In handle_file_req - error recv");
        return 1;
    }
    else if(len == 0)
    {
        ESP_LOGI(TAG, "In handle_file_req - session closed");
        return 1;
    }
    
    rx_buffer[len] = 0;
    if((char) rx_buffer[len-1] == '\n')
    {
        rx_buffer[len-1] = 0;
    }

    DIR *d;
    struct dirent *dir;

    d = opendir(MOUNT_PATH);
    if(d)
    {
        while((dir = readdir(d))!=NULL)
        {
            if(strcmp(dir->d_name, (char*) rx_buffer) == 0)
            {
                break;
            }
        }

        closedir(d);
    }
    else
    {
        ESP_LOGE(TAG, "In present_files - failed to open %s", MOUNT_PATH);
        return 0;
    }

    if(dir == NULL)
    {
        ESP_LOGE(TAG, "In handle_file_req - requested non existent file %s", rx_buffer);
        return 0;
    }

    // Send file
    ESP_LOGI(TAG, "File %s requested", rx_buffer);
    uint8_t tx_buffer[256];
    char path[33];
    snprintf(path,32, "%.8s/%.22s", MOUNT_PATH, rx_buffer);
    FILE* f = fopen(path, "r");

    if(!f)
    {
        ESP_LOGE(TAG, "In handle_file_req - Failed to open %s", path);
        return 0;
    }

    size_t num_read = 0;
    int still_sending = 1;
    while(still_sending)
    {
        num_read = fread(tx_buffer, 1, 256, f);
        if(num_read < 256)
        {
            still_sending = 0;
        }

        ESP_LOGI(TAG, "Sending %d bytes ...", num_read);
        if( send(client_socket, tx_buffer, num_read, 0) < 1)
        {
            ESP_LOGE(TAG, "In handle_file_req - Failed to send file data");
            break;
        }
    }

    fclose(f);
    return 0;
}

// Returns a 1 if the error caused should reset the tcp connection
static uint8_t present_files(void)
{
    DIR *d;
    struct dirent *dir;

    d = opendir(MOUNT_PATH);
    if(d)
    {
        while((dir = readdir(d))!=NULL)
        {
            uint8_t len = strlen(dir->d_name);
            int written =send(client_socket, dir->d_name, len, 0);
            if(written < 0)
            {
                ESP_LOGE(TAG, "In present Files - send error");
                return 1;
            }
            

            written = send(client_socket, "\n", 1, 0);
            if(written < 0)
            {
                ESP_LOGE(TAG, "In present Files - send error");
                return 1;
            }

            ESP_LOGI(TAG, "Successfully Presented file %s", dir->d_name);
        }
        closedir(d);
    }
    else
    {
        ESP_LOGE(TAG, "In present_files - failed to open %s", MOUNT_PATH);
        return 1;
    }

    return 0;
}

// Init the listening socket, bind it to our static IP and port
static void client_handler_task(void* args)
{
    running = 1;

    listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if(listen_sock < 0)
    {
        ESP_LOGE(TAG, "Failed to open listening socket");
        goto clean_up0;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ESP_LOGI(TAG, "Listening Socket Created");

    struct sockaddr_storage dest_addr;
    struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
    dest_addr_ip4->sin_addr.s_addr = inet_addr(CONFIG_TCP_SERVER_IP);
    dest_addr_ip4->sin_family = AF_INET;
    dest_addr_ip4->sin_port = htons(CONFIG_TCP_SERVER_PORT);
    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    err |= listen(listen_sock, 1);
    
    if(err)
    {
        ESP_LOGE(TAG, "Failed to bind listening socket");
        goto clean_up0;
    }

    ESP_LOGI(TAG, "Listening socket bound to %s:%d", CONFIG_TCP_SERVER_IP, CONFIG_TCP_SERVER_PORT);

    while(running)
    {
        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);

        client_socket = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (client_socket < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: %s", strerror(errno));
            continue;
        }

        inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, ip_addr, 16);
        ESP_LOGI(TAG, "Client Connected %s - Starting Session", ip_addr);

        while(running)
        {
            if(present_files())
            {
                break;
            }

            if(handle_file_req())   // Generally block here
            {
                break;
            }
        }

        // Clean up sesion resources
        shutdown(client_socket, 0);
        close(client_socket);
    }

    // Clean up listening socket resources including this task. We force the
    // client to disconnect from the Soc AP here (if it that isnt what caused)
    // us to get here.
clean_up0:
    running = 0;
    close(listen_sock);
    ESP_LOGI(TAG, "TCP File Server Task Exiting ...");
    vTaskDelete(NULL);
}

//*****************************************************************************
// Start and Stop API funcs
//*****************************************************************************

esp_err_t tcp_file_server_launch(char* mount_path)
{
    if(running)
    {
        ESP_LOGE(TAG, "already running");
        return ESP_ERR_INVALID_STATE;
    }
    if(strnlen(mount_path, 32) > 32)
    {
        ESP_LOGE(TAG, "File system mount path passed to long");
        return ESP_ERR_INVALID_ARG;
    }

    strcpy(MOUNT_PATH, mount_path);
    memset(&handler_task, 0, sizeof(TaskHandle_t));
    xTaskCreate(client_handler_task, "tcp_server", 4096, NULL, CONFIG_TCP_SERVER_PRIO, &handler_task);
    
    if(!handler_task)
    {
        ESP_LOGE(TAG,"Failed to start TCP File Server Task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "TCP File Server Task Launched");
    return ESP_OK;
}

esp_err_t tcp_file_server_kill(void)
{
    if(!running)
    {
        ESP_LOGE(TAG, "not running");
        return ESP_ERR_INVALID_STATE;
    }

    running = 0;
    close(client_socket);
    close(listen_sock);

    return ESP_OK;
}


//*****************************************************************************
// REPL test driver functions
//*****************************************************************************

int do_tcp_file_server_launch(int argc, char** argv)
{
    if(argc != 2)
    {
        printf("Usage) tcp_file_server_launch <file search path>");
        return 1;
    }

    ESP_ERROR_CHECK(tcp_file_server_launch(argv[1]));

    return 0;
}

int do_tcp_file_server_kill(int argc, char** argv)
{
    ESP_ERROR_CHECK(tcp_file_server_kill());
    return 0;
}