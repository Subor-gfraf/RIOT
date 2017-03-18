/*
 * Copyright (C) 2016 Unwired Devices [info@unwds.com]
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @defgroup
 * @ingroup
 * @brief
 * @{
 * @file	    umdk-counter.c
 * @brief       umdk-counter module implementation
 * @author      Mikhail Perkov
 * @author      Oleg Artamonov
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>

#include "periph/gpio.h"
#include "periph/rtc.h"
#include "lpm.h"

#include "board.h"

#include "unwds-common.h"
#include "umdk-counter.h"

#include "thread.h"
#include "rtctimers.h"

static kernel_pid_t handler_pid;

static uwnds_cb_t *callback;
static rtctimer_t publishing_timer;

static uint8_t ignore_irq[UMDK_COUNTER_NUM_SENS] = { };
static uint8_t last_value[UMDK_COUNTER_NUM_SENS] = { };

static msg_t publishing_msg = { };


static struct  {
    uint8_t is_valid;
    uint32_t count_value[UMDK_COUNTER_NUM_SENS];
    uint8_t publish_period;
} conf_counter;

static gpio_t pins_sens[UMDK_COUNTER_NUM_SENS] = { UMDK_COUNTER_1, UMDK_COUNTER_2, UMDK_COUNTER_3, UMDK_COUNTER_4 };

static void counter_poll(void *arg)
{
    int i = 0;
    bool wakeup = false;
    
    for (i = 0; i < UMDK_COUNTER_NUM_SENS; i++) {
        if (ignore_irq[i]) {
            gpio_init(pins_sens[i], GPIO_IN_PU);
            uint32_t value = gpio_read(pins_sens[i]);
            gpio_init(pins_sens[i], GPIO_AIN);
            
            if (value == last_value[i]) {
                if (value) {
                    ignore_irq[i] = 0;
                    gpio_init(pins_sens[i], GPIO_IN_PU);
                    gpio_irq_enable(pins_sens[i]);
                } else {
                    wakeup = true;
                }
            } else {
                last_value[i] = value;
            }
        }
    }
    
    /* All counters in IRQ mode */
    if (!wakeup) {
        rtc_clear_wakeup();
    }
}

static void counter_irq(void* arg)
{
    int num = (int)arg;
    if (ignore_irq[num]) {
        return;
    }
    ignore_irq[num] = 1;
    
    gpio_irq_disable(pins_sens[num]);
    gpio_init(pins_sens[num], GPIO_AIN);
    
    conf_counter.count_value[num]++;
    /* Start periodic check every 100 ms */
    last_value[num] = 0;
    rtc_set_wakeup(UMDK_COUNTER_SLEEP_TIME_MS*1e3, &counter_poll, NULL);
}

static inline void save_config(void)
{
    conf_counter.is_valid = 1;
    unwds_write_nvram_config(UNWDS_COUNTER_MODULE_ID, (uint8_t *) &conf_counter, sizeof(conf_counter));
}

static void *handler(void *arg)
{
    msg_t msg;
    msg_t msg_queue[4];
    msg_init_queue(msg_queue, 4);

    while (1) {
        msg_receive(&msg);

        module_data_t data;
        data.length = 1 + 4 * UMDK_COUNTER_NUM_SENS;

        /* Write module ID */
        data.data[0] = UNWDS_COUNTER_MODULE_ID;

        /* Write four counter values */
        uint32_t *tmp = (uint32_t *)(&data.data[1]);

        /* Compress 4 values to 12 bytes total */
        *(tmp + 0)  = conf_counter.count_value[0] << 8;
        *(tmp + 0) |= (conf_counter.count_value[1] >> 16) & 0xFF;
        
        *(tmp + 1) = conf_counter.count_value[1] << 16;
        *(tmp + 1) |= (conf_counter.count_value[2] >> 8) & 0xFFFF;
        
        *(tmp + 2) = (conf_counter.count_value[2] << 24);
        *(tmp + 2) |= conf_counter.count_value[3] & 0xFFFFFF;

        save_config(); /* Save values into NVRAM */

        callback(&data);

        /* Restart timer */
        if (conf_counter.publish_period) {
            rtctimers_set_msg(&publishing_timer, \
                              UMDK_COUNTER_VALUE_PERIOD_PER_SEC * conf_counter.publish_period, \
                              &publishing_msg, handler_pid);
        }
        gpio_irq_enable(UMDK_COUNTER_BTN);
    }
    return NULL;
}

static void btn_connect(void* arg) {
    /* connect button pressed — publish to LoRa in 1 second */
    gpio_irq_disable(UMDK_COUNTER_BTN);
    rtctimers_set_msg(&publishing_timer, 1, &publishing_msg, handler_pid);
}

static void reset_config(void) {
	conf_counter.is_valid = 0;
	memset(&conf_counter.count_value[0], 0, sizeof(conf_counter.count_value));
	conf_counter.publish_period = UMDK_COUNTER_PUBLISH_PERIOD_MIN;
}

static int set_period(int period) {
    if (!period || (period > UMDK_COUNTER_PUBLISH_PERIOD_MAX)) {
            return 0;
        }
            
    conf_counter.publish_period = period;
    save_config();

    rtctimers_set_msg(&publishing_timer,
                      UMDK_COUNTER_VALUE_PERIOD_PER_SEC * conf_counter.publish_period,
                      &publishing_msg, handler_pid);
    printf("[umdk-counter] Period set to %d hour (s)\n", conf_counter.publish_period);
    
    return 1;
}

int umdk_counter_shell_cmd(int argc, char **argv) {
    if (argc == 1) {
        puts ("counter get - get results now");
        puts ("counter send - get and send results now");
        puts ("counter period <N> - set period to N minutes");
        puts ("counter reset - reset settings to default, counter to zero");
        return 0;
    }
    
    char *cmd = argv[1];
	
    if (strcmp(cmd, "get") == 0) {
        int i = 0;
        for (i = 0; i < UMDK_COUNTER_NUM_SENS; i++) {
            printf("Counter %d: %" PRIu32 "\n", i, conf_counter.count_value[i]);
        }
    }
    
    if (strcmp(cmd, "send") == 0) {
        msg_send(&publishing_msg, handler_pid);
    }
    
    if (strcmp(cmd, "period") == 0) {
        char *val = argv[2];
        return set_period(atoi(val));
    }
    
    if (strcmp(cmd, "reset") == 0) {
        reset_config();
        save_config();
    }
    
    return 1;
}

void umdk_counter_init(uint32_t *non_gpio_pin_map, uwnds_cb_t *event_callback)
{
    (void) non_gpio_pin_map;

    conf_counter.publish_period = UMDK_COUNTER_PUBLISH_PERIOD_MIN;

    callback = event_callback;

    for (int i = 0; i < UMDK_COUNTER_NUM_SENS; i++) {
        gpio_init_int(pins_sens[i], GPIO_IN_PU, GPIO_FALLING, counter_irq, (void *) i);
        ignore_irq[i] = 0;
    }
    
    gpio_init_int(UMDK_COUNTER_BTN, GPIO_IN_PU, GPIO_FALLING, btn_connect, NULL);

    /* Create handler thread */
    char *stack = (char *) allocate_stack();
    if (!stack) {
        puts("umdk-counter: unable to allocate memory. Is too many modules enabled?");
        return;
    }

    /* Load config from NVRAM */
    if (!unwds_read_nvram_config(UNWDS_COUNTER_MODULE_ID, (uint8_t *) &conf_counter, sizeof(conf_counter))) {
        return;
    }
    
    if ((conf_counter.is_valid == 0xFF) || (conf_counter.is_valid == 0))  {
		reset_config();
	}

    printf("[umdk-counter] Current publish period: %d hour(s)\n", conf_counter.publish_period);
    
    unwds_add_shell_command("counter", "type 'counter' for commands list", umdk_counter_shell_cmd);

    handler_pid = thread_create(stack, UNWDS_STACK_SIZE_BYTES, THREAD_PRIORITY_MAIN - 1, \
                                THREAD_CREATE_STACKTEST, handler, NULL, "counter thread");

    /* Start publishing timer */
    rtctimers_set_msg(&publishing_timer, \
                      UMDK_COUNTER_VALUE_PERIOD_PER_SEC * conf_counter.publish_period, \
                      &publishing_msg, handler_pid);
}


bool umdk_counter_cmd(module_data_t *cmd, module_data_t *reply)
{
    if (cmd->length < 1) {
        return false;
    }

    umdk_counter_cmd_t c = cmd->data[0];
    switch (c) {
        case UMDK_COUNTER_CMD_SET_PERIOD: {
            if (cmd->length != 2) {
                return false;
            }

            uint8_t period = cmd->data[1];
            /* do not change period if new one is 0 or > max */
            
            reply->length = 2;
            reply->data[0] = UNWDS_COUNTER_MODULE_ID;
            
            if (!set_period(period)) {           
                reply->data[1] = 253;
            } else {
                reply->data[1] = 0;
            }

            return true; /* Allow reply */
        }

        case UMDK_COUNTER_CMD_POLL: {
            /* Send values to publisher thread */
            msg_send(&publishing_msg, handler_pid);
            return false; /* Don't reply */
        }
        case UMDK_COUNTER_CMD_RESET: {
            memset(&conf_counter.count_value[0], 0, sizeof(conf_counter.count_value));
            save_config();
            
            reply->length = 2;
            reply->data[0] = UNWDS_COUNTER_MODULE_ID;
            reply->data[1] = 0;

            return true;
        }
        default:
            break;
    }

    /* Don't reply by default */
    return false;
}

#ifdef __cplusplus
}
#endif
