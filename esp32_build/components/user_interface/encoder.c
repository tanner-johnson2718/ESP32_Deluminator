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
 * @file encoder.c
 *
 * ESP-IDF HW timer-based driver for rotary encoders
 *
 * Copyright (c) 2019 Ruslan V. Uss <unclerus@gmail.com>
 *
 * BSD Licensed as described in the file LICENSE
 */
#include "encoder.h"
#include <esp_log.h>
#include <string.h>
#include <freertos/semphr.h>
#include <esp_timer.h>

#define MUTEX_TIMEOUT 10

#define BTN_PRESSED_LEVEL 0

static const char *TAG = "encoder";
static rotary_encoder_t *encs[1] = { 0 };
static const int8_t valid_states[] = { 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0 };
static SemaphoreHandle_t mutex;
static QueueHandle_t _queue;

#define GPIO_BIT(x) ((x) < 32 ? BIT(x) : ((uint64_t)(((uint64_t)1)<<(x))))
#define CHECK(x) do { esp_err_t __; if ((__ = x) != ESP_OK) return __; } while (0)
#define CHECK_ARG(VAL) do { if (!(VAL)) return ESP_ERR_INVALID_ARG; } while (0)

inline static void read_encoder(rotary_encoder_t *re)
{
    rotary_encoder_event_t ev = {
        .sender = re
    };

    if (re->pin_btn < GPIO_NUM_MAX)
    do
    {
        if (re->btn_state == RE_BTN_PRESSED && re->btn_pressed_time_us < 10000)
        {
            // Dead time
            re->btn_pressed_time_us += 1000;
            break;
        }

        // read button state
        if (gpio_get_level(re->pin_btn) == BTN_PRESSED_LEVEL)
        {
            if (re->btn_state == RE_BTN_RELEASED)
            {
                // first press
                re->btn_state = RE_BTN_PRESSED;
                re->btn_pressed_time_us = 0;
                ev.type = RE_ET_BTN_PRESSED;
                xQueueSendToBack(_queue, &ev, 0);
                break;
            }

            re->btn_pressed_time_us += 1000;

            if (re->btn_state == RE_BTN_PRESSED && re->btn_pressed_time_us >= 1000000)
            {
                // Long press
                re->btn_state = RE_BTN_LONG_PRESSED;
                ev.type = RE_ET_BTN_LONG_PRESSED;
                xQueueSendToBack(_queue, &ev, 0);
            }
        }
        else if (re->btn_state != RE_BTN_RELEASED)
        {
            bool clicked = re->btn_state == RE_BTN_PRESSED;
            // released
            re->btn_state = RE_BTN_RELEASED;
            ev.type = RE_ET_BTN_RELEASED;
            xQueueSendToBack(_queue, &ev, 0);
            if (clicked)
            {
                ev.type = RE_ET_BTN_CLICKED;
                xQueueSendToBack(_queue, &ev, 0);
            }
        }
    } while(0);

    re->code <<= 2;
    re->code |= gpio_get_level(re->pin_a);
    re->code |= gpio_get_level(re->pin_b) << 1;
    re->code &= 0xf;

    if (!valid_states[re->code])
        return;

    int8_t inc = 0;

    re->store = (re->store << 4) | re->code;

    if (re->store == 0xe817) inc = 1;
    if (re->store == 0xd42b) inc = -1;

    if (inc)
    {
        ev.type = RE_ET_CHANGED;
        ev.diff = inc;
        xQueueSendToBack(_queue, &ev, 0);
    }
}

static void timer_handler(void *arg)
{
    if (!xSemaphoreTake(mutex, 0))
        return;

    for (size_t i = 0; i < 1; i++)
        if (encs[i])
            read_encoder(encs[i]);

    xSemaphoreGive(mutex);
}

static const esp_timer_create_args_t timer_args = {
        .name = "__encoder__",
        .arg = NULL,
        .callback = timer_handler,
        .dispatch_method = ESP_TIMER_TASK
};

static esp_timer_handle_t timer;

esp_err_t rotary_encoder_init(QueueHandle_t queue)
{
    CHECK_ARG(queue);
    _queue = queue;

    mutex = xSemaphoreCreateMutex();
    if (!mutex)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    CHECK(esp_timer_create(&timer_args, &timer));
    CHECK(esp_timer_start_periodic(timer, 1000));

    ESP_LOGI(TAG, "Initialization complete, timer interval: %dms", 1000 / 1000);
    return ESP_OK;
}

esp_err_t rotary_encoder_add(rotary_encoder_t *re)
{
    CHECK_ARG(re);
    if (!xSemaphoreTake(mutex, MUTEX_TIMEOUT))
    {
        ESP_LOGE(TAG, "Failed to take mutex");
        return ESP_ERR_INVALID_STATE;
    }

    bool ok = false;
    for (size_t i = 0; i < 1; i++)
        if (!encs[i])
        {
            re->index = i;
            encs[i] = re;
            ok = true;
            break;
        }
    if (!ok)
    {
        ESP_LOGE(TAG, "Too many encoders");
        xSemaphoreGive(mutex);
        return ESP_ERR_NO_MEM;
    }

    // setup GPIO
    gpio_config_t io_conf;
    memset(&io_conf, 0, sizeof(gpio_config_t));
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = 1;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pin_bit_mask   = (1ULL << re->pin_a)  |
                             (1ULL << re->pin_b)  |
                             (1ULL << re->pin_btn);
    CHECK(gpio_config(&io_conf));

    re->btn_state = RE_BTN_RELEASED;
    re->btn_pressed_time_us = 0;

    xSemaphoreGive(mutex);

    ESP_LOGI(TAG, "Added rotary encoder %d, A: %d, B: %d, BTN: %d", re->index, re->pin_a, re->pin_b, re->pin_btn);
    return ESP_OK;
}

esp_err_t rotary_encoder_remove(rotary_encoder_t *re)
{
    CHECK_ARG(re);
    if (!xSemaphoreTake(mutex, MUTEX_TIMEOUT))
    {
        ESP_LOGE(TAG, "Failed to take mutex");
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t i = 0; i < 1; i++)
        if (encs[i] == re)
        {
            encs[i] = NULL;
            ESP_LOGI(TAG, "Removed rotary encoder %d", i);
            xSemaphoreGive(mutex);
            return ESP_OK;
        }

    ESP_LOGE(TAG, "Unknown encoder");
    xSemaphoreGive(mutex);
    return ESP_ERR_NOT_FOUND;
}
