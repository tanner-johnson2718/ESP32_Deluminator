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
// | STA Connect |--->| Launch Client Handler Task |--->| Open Listening Port |
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
//*****************************************************************************

#pragma once

uint8_t launch_tcp_file_server(char* mount_path);
void kill_tcp_file_server(void);