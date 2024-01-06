/*
 * Copyright (c) 2019 Ruslan V. Uss <unclerus@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of itscontributors
 *    may be used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file encoder.h
 * @defgroup encoder encoder
 * @{
 *
 * ESP-IDF HW timer-based driver for rotary encoders
 *
 * Copyright (c) 2019 Ruslan V. Uss <unclerus@gmail.com>
 *
 * BSD Licensed as described in the file LICENSE
 */

// This file is kindly provided by the author above. We make no changes to the 
//code except we exported pin assignment, i2c clk speed and other consts to the 
// Kconfig. As mentioned above this is a polling timer based rotary encoder 
// driver. We make this comment just to  provide some basic theory and overview
// of this drivers implementation
//
// Button) A push button is a part of most rotary encoders and this allows the
//         user to "click" to trigger events. A button circiut looks something
//         like:
//
//     GPIO PIN -----------------
//                              |
//                              |
//                                /    <----- Cuts and makes connection
//                              |
//                              |
//                           |-----|   
//                           | Res |   <-------- 10Kohm 
//                           |-----|
//                              |
//                              |
//       GND --------------------
//
// A GPIO is internally "pulled" high meaning its resting logic is the high
// voltage or logical one. We connect the pin to ground through a button and
// a large resistor. When the button is pressed the pin is conected to ground
// changing its voltage 0, a logical 0. The chnage in state triggers an ISR,
// which in turn polls the state of the button and if the state is differenet
// we post a button pressed event to the event Q supplied (see ui component for
// more details on that). The large resistor limits the current involved with
// connection a pos directly to ground. 
//
// DeBouncing) Buttons have tendancy "bounce" as there is usually a spring like
//             mechanical component holding it up. You pressing it down and up
//             cause on big change in state preceeded and proceeded by little
//             bounces. The driver mitigates that by 1 polling at sufficently
//             large interval such that the little flucuations or spurios
//             presses are of high enough frequency to fall between poll 
//             intervals. There is also a configured "dead time" such that if
//             the button was pressed and "dead time" hasnt passed we assume
//             the button is still pressed and that polling time interval to 
//             the total button pressed time. This again helps smooth over 
//             those flucuations. All this just makes sure that when I 
//             press the button little flucuations dont trigger multiple button
//             presses.
//
// Rotary) The actual rotary part of the rotary encoder reads rotational change
//         of the spiny part of the device. How it does this is shown in the
//         diagram below.
// 
// Resting State                       Moving State  
//
//   |XXXXX|     |XXXXX|                 |XXXXX|     |XXXXX|       
//          ^ ^ ^                                   ^ ^ ^
//          | | |                                   | | |
//   -------- | --------                     -------- | --------     
//   |        |        |                     |        |        | 
// PIN A     GND     PIN B                 PIN A     GND     PIN B
// 
// The |XXXX| region represents conductive material the "wipers" are 
// mechanically connected to. The |    | region is non conductive. When the rot
// is in its resting state no connection is made. As it moves right or rotates
// clockwise PIN B gets pulled to ground triggering an ISR and like wise on the
// back end PIN A gets pulled to ground. With this information we can determine
// how many clicks the encoder got rotated and in which direction.
//
// Driver) The code in this module doing this logic does so by polling the state
//         and placing it in a stat vector such that we place the state in the
//         lower two bits. The as more rot events come in we shift the state left
//         and OR in the new state. Thus this gives us a state vector encoding
//         the previous values of pin A and B. If these values constittute
//         a state of moving the encoder, the driver triggers a rot event.

#ifndef __ENCODER_H__
#define __ENCODER_H__

#include <esp_err.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Button state
 */
typedef enum {
    RE_BTN_RELEASED = 0,      //!< Button currently released
    RE_BTN_PRESSED = 1,       //!< Button currently pressed
    RE_BTN_LONG_PRESSED = 2   //!< Button currently long pressed
} rotary_encoder_btn_state_t;

/**
 * Rotary encoder descriptor
 */
typedef struct
{
    gpio_num_t pin_a, pin_b, pin_btn; //!< Encoder pins. pin_btn can be >= GPIO_NUM_MAX if no button used
    uint8_t code;
    uint16_t store;
    size_t index;
    uint64_t btn_pressed_time_us;
    rotary_encoder_btn_state_t btn_state;
} rotary_encoder_t;

/**
 * Event type
 */
typedef enum {
    RE_ET_CHANGED = 0,      //!< Encoder turned
    RE_ET_BTN_RELEASED,     //!< Button released
    RE_ET_BTN_PRESSED,      //!< Button pressed
    RE_ET_BTN_LONG_PRESSED, //!< Button long pressed (press time (us) > RE_BTN_LONG_PRESS_TIME_US)
    RE_ET_BTN_CLICKED       //!< Button was clicked
} rotary_encoder_event_type_t;

/**
 * Event
 */
typedef struct
{
    rotary_encoder_event_type_t type;  //!< Event type
    rotary_encoder_t *sender;          //!< Pointer to descriptor
    int32_t diff;                      //!< Difference between new and old positions (only if type == RE_ET_CHANGED)
} rotary_encoder_event_t;

/**
 * @brief Initialize library - Create semaphore and timer.
 *
 * @param queue Event queue to send encoder events
 * @return `ESP_OK` on success
 */
esp_err_t rotary_encoder_init(QueueHandle_t queue);

/**
 * @brief Add new rotary encoder - inits and starts the gpio module underneath
 *                                 the driver.
 *
 * @param re Encoder descriptor
 * @return `ESP_OK` on success
 */
esp_err_t rotary_encoder_add(rotary_encoder_t *re);

/**
 * @brief Remove previously added rotary encoder
 *
 * @param re Encoder descriptor
 * @return `ESP_OK` on success
 */
esp_err_t rotary_encoder_remove(rotary_encoder_t *re);

#ifdef __cplusplus
}
#endif

/**@}*/

#endif /* __ENCODER_H__ */
