// User interface. The user interface is composed of an 2004 LCD and a standard
// rotary encoder with a push button. The gives us a rather simple input model
//
// |----------------|  publish   |------------|  notifies   |------------------|
// | Rot State Poll |----------->| UI Event Q |------------>| UI Event handler |
// |----------------|            |------------|             |------------------|
//                                                            |    |    |    |
//      -------------------------------------------------------    |    |    |
//      |                 ------------------------------------------    |    |
//      |                 |                  ----------------------------    |
//      |                 |                  |                   -------------
//      |                 |                  |                   |
//      V                 V                  V                   V
// |----------|     |-----------|     |-------------|     |------------|
// | Rot Left |     | Rot Right |     | Short Press |     | Long Press |
// |----------|     |-----------|     |-------------|     |------------|
//
//
// Input is generated by polling the rotary encoder state at regular intervals.
// When it rotary encoder driver deems that an event has occured it publishes
// this event to the UI event Q which then wakes the UI event handler which
// calls the corresponding state function handle i.e. short_press(), etc..
// Now the input above drives the UI state as shown below:
//
// |---------|-------------------------|------------|-------------------------|
// | In Menu |                         | In Command |                         |
// |---------|                         |------------|                         |
// |                                   |                                      |
// |           CMD List                |                     Line Buff        |
// |          |---------|       |-------------|             |----------|      |                                             
// | Cursor-->|  cmd_0  |------>| Short Press |-- Cursor--->|  line_0  |---   |
// |    ^     |  cmd_1  |       |  (CMD INI)  |     ^       |  line_1  |  |   |
// |    |     |  cmd_2  |       |-------------|     |       |  line_2  |  |   |
// |    |     |  ...    |              |            |       |    ...   |  |   |
// |    |     |  cmd_n  |              |            |       |  line_n  |  |   |
// |    |     |---------|              |            |       |----------|  |   |
// |    |                        |------------|     |                     |   |
// |    |------------------------| Long Press |     |           -----------   |
// |                             | (CMD FINI) |     |           v             |
// |                             |------------|     |   |-------------|       |
// |                                   |            ----| Short Press |       |
// |                                   |                |   (CMD CB)  |       |
// |                                   |                |-------------|       |
// |-----------------------------------|--------------------------------------|
//
//
// The UI state is a set of different modes. In menu and in command. In the 
// "in menu" mode you see a list of commands to be executed. Rot Left and Rot
// Right move the cursor up and down the screen. Based on the cursor position
// and size of screen the display is updated in a logical manner. A
// short press on this mode executes a commands init function and switches the
// mode to in command where a line buffer is seen and scrolled through instead
// of a list of commands. Its on the command to clear the buffer before use. 
// Now subsequent short presses pass the index in the line buffer the cursor is
// pointing too and execute that commands call back function. Finally a long
// press brings you back to the in menu mode and calls the commands fini 
// function. API functions are provided for manipulating the UI state.
//
// NOTE access to the UI state via API functions are not guarded i.e. anyone
// or command can call them regardless of whether that function was executing
// it is up to the user to make sure that the commands fini functions kills all
// future access to the UI state.
//
// CONFIG) Dont forget to set the following in menuconfig. Mostly need to worry
//         if registering  too many commands, line buffer overflow, or the UI
//         event handler is taking up to much cpu time
//                * UI_NUM_CMDS
//                * UI_NUM_LINE_BUFF
//                * UI_EVENT_Q_SIZE
//                * CONFIG_UI_EVENT_HNDLR_PRIO
//

#pragma once

#include <stdint.h>

typedef void (*command_cb_t)(void);
typedef void (*on_press_cb_t)(uint8_t line_index);

//*****************************************************************************
// ui_init: To init the user interface a few high level tasks are
//                      done
//
//  1) Malloc CMD list, the Line Buffer, and the call back lists. Note this
//     could and ideally should statically allocated but one we are low on 
//     static memory and two this introduces the possibility of having this
//     component be nixed in the event we do not have an LCD or rotary
//
//  2) Create an even queue for rotoary encoder events and create a task to
//     handle them
//
//  3) Init the LCD and the rotary encoder components
//
//  4) Reset the screen, home and cursor positions, etc.
//
// Returns) Will always return ESP_OK. LCD and rotencoder failures fail 
//          gracefully such that execution can continue without them. Other
//          failures such as no mem or mutex failures simply crash device as
//          these can and should be fixed prior to code deployment.
//*****************************************************************************
esp_err_t ui_init(void);

//*****************************************************************************
// ui_add_cmd) Registered a UI command that will be populated on the menu 
//             screen and can be executed
//
// name) The name that appears on the menu screen. Must less than 19 chars or
//       LCD_cols -1 to fit on screen. Will genereate an ESP_INVALID_ARG 
//       failure if not
//
// cmd_init) Function to be called on fist execution of the command
//
// on_press_cb) Function to be called once in command and press is registered
//
// cmd_fini) Function called when long pressed and before returning back to 
//           the menu.
//
// Returns) ESP_OK on success otherwise, could be invalid name, no more room
//          for more cmds or an error passed up by updating the display.
//
//*****************************************************************************
esp_err_t ui_add_cmd(char* name, command_cb_t  cmd_init, 
                            on_press_cb_t on_press_cb, 
                            command_cb_t  cmd_fini);


//*****************************************************************************
// ui_push_to_line_buffer) insert a line at a specific index in the line buffer.
//                      Keep in mind this is not the index on screen but it is
//                      in the line buffer and this based on the current cursor
//                      may need be displayed.
//
// line_num) Index to insert line. Is checked to verify it fits in the line 
//           buffer
//
// line) String to insert. Checked that it is not longer than 19 or LCD_cols-1.
//
// Returns) ESP_OK on succes, else an input error.
//
//*****************************************************************************
esp_err_t ui_push_to_line_buffer(uint8_t line_num, char* line);


//*****************************************************************************
// ui_update_display) Home and clear screen. This just delets all the current text
//                 on the screen and makes it so thenext LCD write goes to the
//                 0,0 posistion. Then update each line according to whats in
//                 the line buff at the set cursor and screen starting index. 
//                 Access to the LCD is made atomic via semaphore. This is b/c 
//                 writing to LCD takes several writes spaced milliseconds apart 
//                 and we have async components. Thus this is a possible point 
//                 of failure
//
// Returns) ESP_OK, otherwise could fail to get mutex or the i2c write could
//          fail
//*****************************************************************************
esp_err_t ui_update_display(void);


//*****************************************************************************
// update_line) Use this to update just one line on the screen. The passed index
//              is the index in line buff. Must be on screen or will not do 
//              anything. Must also be called from not in_menu context.
//
// i) index in the line buff not the screen. Is checked to be valid or not.
//
// Returns) ESP_OK on success, else could be invalid index or failure to write
//          to LCD
//*****************************************************************************
esp_err_t ui_update_line(uint8_t i);


//*****************************************************************************
// home_screen_pos) Just sets the internal cursor and first line index to 0.
//                  Does not update the display.
//
// Returns) Always ESP_OK
//*****************************************************************************
esp_err_t ui_home_screen_pos(void);


//*****************************************************************************
// lock) Dont allow the cursor to move
//
// Returns) Always ESP_OK
//*****************************************************************************
esp_err_t ui_lock_cursor(void);


//*****************************************************************************
// lock) allow the cursor to move. Does not matter if not previously locked
//
// Returns) Always ESP_OK
//*****************************************************************************
esp_err_t ui_unlock_cursor(void);


//*****************************************************************************
// Test driver functions to export to the REPL. All take in no args and always
// succeed.
//*****************************************************************************

int do_rot_l(int argc, char** argv);      // Calls rot_left cb in ui driver
int do_rot_r(int argc, char** argv);      // Calls rot_right cb in ui driver
int do_press(int argc, char** argv);      // Calls short_press in ui driver
int do_long_press(int argc, char** argv); // Calls long_press in ui driver