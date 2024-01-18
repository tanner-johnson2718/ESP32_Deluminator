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


#define MAX_FILES 32
#define MAX_PATH_LEN 32

static TaskHandle_t handler_task;
static int client_socket;
static int listen_sock;
static char ip_addr[16];
static int running = 0;
static char MOUNT_PATH[MAX_PATH_LEN + 1];
static char path_index[MAX_FILES * (MAX_PATH_LEN+1)];
static uint8_t num_paths = 0;
static uint8_t file_to_send = 0;

//*****************************************************************************
// TCP File Server Comm Protocol helpers. Return values are 1 if bad and need
// to reset client socket connection 0 good.
//*****************************************************************************

static uint8_t send_N()
{
    // ESP_LOGI(TAG, "send_N  N=%d", num_paths);
    if(send(client_socket, &num_paths, 1, 0) != 1)
    {
        return 1;
    }

    return 0;    
}

static uint8_t get_N()
{
    uint8_t x;
    size_t num_recv = recv(client_socket, &x, 1, 0);
    if(num_recv != 1 || x != num_paths)
    {
        return 1;
    }

    return 0;
}

static uint8_t send_indexed_path(uint8_t i)
{
    uint8_t buff[33] = {0};
    buff[0] = i;
    uint8_t len = strlen(path_index + i*(MAX_PATH_LEN+1));
    memcpy(buff + 1, path_index + i*(MAX_PATH_LEN+1), len);

    if(send(client_socket, buff, 33, 0) != 33)
    {
        return 1;
    }

    return 0;
}

static uint8_t send_file_paths()
{
    uint8_t i;
    for(i = 0; i < num_paths; ++i)
    {
        if(send_indexed_path(i)){ return 1; }
    }

    return 0;
}

static uint8_t get_file_index()
{
    uint8_t i;
    uint8_t num_recv = recv(client_socket, &i, 1, 0);
    if(num_recv != 1 || i >= num_paths)
    {
        file_to_send = 0;
        return 1;
    }

    file_to_send = i;
    return 0;
}

// returns a 1 if the error caused should reset the tcp connection
static void send_data(void)
{
    uint8_t tx_buffer[256];
    char path[33];
    FILE* f = fopen(path_index+file_to_send*(MAX_PATH_LEN+1), "r");

    if(!f)
    {
        ESP_LOGE(TAG, "In send_data - Failed to open %s", path);
        return;
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
}

// Returns a 1 if the error caused should reset the tcp connection
static uint8_t index_files(void)
{
    DIR *d;
    struct dirent *dir;
    num_paths = 0;

    d = opendir(MOUNT_PATH);
    if(d)
    {
        while((dir = readdir(d))!=NULL)
        {
            sprintf( path_index +num_paths*(MAX_PATH_LEN+1),  
                      "%s/%s", 
                      MOUNT_PATH, 
                      dir->d_name );
            ++num_paths;
            // ESP_LOGI(TAG, "Indexed file %s", path_index + (num_paths-1)*(MAX_PATH_LEN+1));
            if(num_paths == MAX_FILES)
            {
                num_paths = 0;
                return 1;
            }
        }
        closedir(d);
    }
    else
    {
        return 1;
    }

    return 0;
}

static uint8_t create_listening_socket()
{
    listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if(listen_sock < 0)
    {
        ESP_LOGE(TAG, "Failed to open listening socket");
        return 1;
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
        return 1;
    }

    ESP_LOGI(TAG, "Listening socket bound to %s:%d", CONFIG_TCP_SERVER_IP, CONFIG_TCP_SERVER_PORT);
    return 0;
}

static uint8_t accept_client_connection(void)
{
    struct sockaddr_in source_addr;
    socklen_t addr_len = sizeof(source_addr);

    client_socket = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
    if (client_socket < 0) {
        ESP_LOGE(TAG, "Unable to accept connection: %s", strerror(errno));
        return 1;
    }

    inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, ip_addr, 16);
    ESP_LOGI(TAG, "Client Connected %s - Starting Session", ip_addr);
    return 0;
}

// Init the listening socket, bind it to our static IP and port
static void client_handler_task(void* args)
{
    running = 1;

    if(create_listening_socket()) {running = 0;}
    
    while(running)
    {
        if( accept_client_connection() ) {continue;}


        if( index_files() ) { goto cleanup; }
        if( send_N() )      { goto cleanup; }
        if( get_N()  )      { goto cleanup; }

        ESP_LOGI(TAG, "Files Indexed, Client Synced - Presenting Files");

        if( send_file_paths() )               { goto cleanup; }
        if( get_file_index()  )               { goto cleanup; }
        if( send_indexed_path(file_to_send) ) { goto cleanup; }
            
        ESP_LOGI(TAG, "(%d) %s requested ... sending", file_to_send, path_index + file_to_send*(MAX_PATH_LEN+1));

        send_data();

        // Clean up sesion resources
        cleanup:
        ESP_LOGI(TAG, "Client Connection Reset");
        shutdown(client_socket, 0);
        close(client_socket);
    }

    // Clean up listening socket resources including this task. We force the
    // client to disconnect from the Soc AP here (if it that isnt what caused)
    // us to get here.
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