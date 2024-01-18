//*****************************************************************************
// TCP File Server. The main state machine driving this component is rather 
// simple. When a station connects we launch a task that opens a tcp server
// listneing port. Upon connection of a tcp client we simply stop listening
// and handle that client. This implies we can have only a single client at
// a time. Now in TCP session the loop is simple. Send all the files on the
// device and wait for a response in the form of requested file. If the file
// exits send it and go back to sending the list of files.
//
// |-------------|    |----------------------------|    |---------------------|
// | API Launch  |--->| Launch Client Handler Task |--->| Open Listening Port |
// |-------------|    |----------------------------|    |---------------------|
//                                                              |
//                                                              |
//                                                              |
//                                                              V
// |-----------------|    |----------------------|    |---------------------|
// | Fufil File Reqs |<---| Present Stored Files |<---|    Accept Conn      |
// |-----------------|    |----------------------|    |---------------------|
//          |                          ^                         ^ 
//          |                          |                         |
//          |---------------------------                         |
//          V                                                    |
// |--------------------------|                                  |
// | Handle Client Disconnect |----------------------------------- 
// |--------------------------|
//
//
// Failures) If anything happens in the first row i.e. we cant launch the conn 
//           handler or can't open the listening port, this indicates very bad 
//           system / config failures beyond our control and we simply kill the
//           the tcp server task
//
//           Once the Listening port is opened, failures can be handled by the
//           catch all of "handle client disconnect" that frees up any client
//           resources and returns to the listening state.
//
//           A STA disconnect event will not interrupt the starting of the
//           client handler task or the starting of the listening port. Once
//           the listening port is open, a client disconnect will trigger a
//           graceful shutdown of the client handle thread and the listening
//           port.
//
// State) The state is rather small for this component:
//            -> Task and Task Handler
//            -> Two sockets, one listening one active client session
//            -> indicator variable "running"
//            -> IP Addr of conneted session
//            -> The path to search for files
//
// Assumptions) We assume the esp wifi module is properly inited. And we are
//              on the same network as a client wishing to connect.
//
// Comms Protocol After Client Socket Established)
//  
//  SERVER             Alive [0] (1sec loop)      CLIENT
//    --------------------------------------------->
//
//                     Alive [0]
//   <----------------------------------------------
//
//                    N Files [N]
//   ---------------------------------------------->
//
//                    N Files [N]
//   <----------------------------------------------
//
//           File Path 0 [0, d_0, ... , d_31]
//   ----------------------------------------------->
//
//                        ....
//
//          File Path N-1 [N-1, d_0, ... , d_31]
//   ----------------------------------------------->
//
//                      File i [i]
//   <-----------------------------------------------
//
//            File Path i [i, d_0, ... , d_31]
//   ----------------------------------------------->
//
//            File Data [d_0, ... , d_255]
//   ------------------------------------------------>
//
//                         ....
//
//            File Data [d_0, ... , d_n-1]
//   ------------------------------------------------>
//
// Messages)
//     - Alive     : One byte with 0 as its data
//     - N file    : One byte with Number of files server is presenting
//     - File Path : File path index of each file followed by exactly 32 bytes
//                   containing the full file path, padded with NULL chars to 
//                   fill up 32 byte. Note could have 32 bytes of all data and
//                   no NULL char. Note index is only valid for one transaction
//                   i.e. Alive -> CRC. 33 bytes always.
//    - File i     : One byte with file index.
//    - File Data  : 256 bytes of file data, up until last message which could 
//                   be 1 to 256 bytes.                        
//*****************************************************************************

#pragma once

//*****************************************************************************
// tcp_file_server) Creates the file server handler task. Assumes the wifi
//                  esp driver is inited.
//
// mount_path) Path to look for files to send over the network. Checks len.
//
// Returns ESP_OK if task created, mount path is good and could succesfully
//         start the server.
//*****************************************************************************
esp_err_t tcp_file_server_launch(char* mount_path);

//*****************************************************************************
// tcp_file_server_kill) Kills the file server gracefullt
//
// Returns) ESP_OK if it is running else error
//*****************************************************************************
esp_err_t tcp_file_server_kill(void);

//*****************************************************************************
// REPL Test Driver Functions
//*****************************************************************************
int do_tcp_file_server_kill(int argc, char** argv);
int do_tcp_file_server_launch(int argc, char** argv);

