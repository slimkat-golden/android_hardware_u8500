/*
 * Copyright (C) 2008 The Android Open Source Project
 * Copyright (C) 2013 The CyanogenMod Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


// #define LOG_NDEBUG 0

#include <cutils/log.h>

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>

#include <sys/ioctl.h>
#include <sys/types.h>

#include <hardware/lights.h>

/******************************************************************************/

static pthread_once_t g_init = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

char const*const LCD_FILE
        = "/sys/class/backlight/panel/brightness";

char const*const BUTTON_FILE
        = "/sys/class/leds/button-backlight/brightness";

#ifdef GENERIC_BLN
char const*const NOTIFICATION_FILE
        = "/sys/class/misc/backlightnotification/notification_led";
#endif

#ifdef MULTI_COLOR_LED
char const*const LED_BLINK
        = "/sys/class/sec/led/led_blink";

struct led_config {
    unsigned int color;
    int delay_on, delay_off;
};

static struct led_config g_leds[3]; // For battery, notifications, and attention.
static int g_cur_led = -1;          // Presently showing LED of the above.
#endif

/**
 * device methods
 */

void init_globals(void)
{
    // init the mutex
    pthread_mutex_init(&g_lock, NULL);
}

static int
write_int(char const* path, int value)
{
    int fd;
    static int already_warned = 0;

    fd = open(path, O_RDWR);
    if (fd >= 0) {
        char buffer[20];
        int bytes = sprintf(buffer, "%d\n", value);
        int amt = write(fd, buffer, bytes);
        close(fd);
        return amt == -1 ? -errno : 0;
    } else {
        if (already_warned == 0) {
            ALOGE("write_int failed to open %s\n", path);
            already_warned = 1;
        }
        return -errno;
    }
}

#ifdef MULTI_COLOR_LED
static int
write_str(char const *path, const char* value)
{
    int fd;
    static int already_warned;

    already_warned = 0;

    ALOGV("write_str: path %s, value %s", path, value);
    fd = open(path, O_RDWR);

    if (fd >= 0) {
        int amt = write(fd, value, strlen(value));
        close(fd);
        return amt == -1 ? -errno : 0;
    } else {
        if (already_warned == 0) {
            ALOGE("write_str failed to open %s\n", path);
            already_warned = 1;
        }
        return -errno;
    }
}
#endif

static int
is_lit(struct light_state_t const* state)
{
    return state->color & 0x00ffffff;
}

static int
rgb_to_brightness(struct light_state_t const* state)
{
    int color = state->color & 0x00ffffff;
    return ((77*((color>>16)&0x00ff))
            + (150*((color>>8)&0x00ff)) + (29*(color&0x00ff))) >> 8;
}

#ifdef MULTI_COLOR_LED
/* LEDs */
static int
write_leds(const struct led_config *led)
{
    static const struct led_config led_off = {0, 0, 0};

    char blink[32];
    int count, err;

    if (led == NULL)
        led = &led_off;

    if ((count = snprintf(blink, sizeof(blink)-1, "0x%08x %d %d", led->color,
                          led->delay_on, led->delay_off)) < 0) {
        return -errno;
    } else if ((unsigned int)count >= sizeof(blink)-1) {
        ALOGE("%s: Truncated string: blink=\"%s\".", __func__, blink);
        return -EINVAL;
    }

    ALOGD("%s: color=0x%08x, delay_on=%d, delay_off=%d, blink=\"%s\".",
          __func__, led->color, led->delay_on, led->delay_off, blink);

    /* Add '\n' here to make the above log message clean. */
    blink[count]   = '\n';
    blink[count+1] = '\0';

    pthread_mutex_lock(&g_lock);
    err = write_str(LED_BLINK, blink);
    pthread_mutex_unlock(&g_lock);

    return err;
}

static int
set_light_leds(struct light_state_t const *state, int type)
{
    struct led_config *led;
    int err = 0;

    ALOGD("%s: type=%d, color=0x%010x, fM=%d, fOnMS=%d, fOffMs=%d.", __func__,
          type, state->color,state->flashMode, state->flashOnMS, state->flashOffMS);

    if (type < 0 || (unsigned int)type >= sizeof(g_leds)/sizeof(g_leds[0]))
        return -EINVAL;

    /* type is one of:
     *   0. battery
     *   1. notifications
     *   2. attention
     * which are multiplexed onto the same physical LED in the above order. */
    led = &g_leds[type];

    switch (state->flashMode) {
    case LIGHT_FLASH_NONE:
        /* Set LED to a solid color, spec is unclear on the exact behavior here. */
        led->delay_on = led->delay_off = 0;
        break;
    case LIGHT_FLASH_TIMED:
    case LIGHT_FLASH_HARDWARE:
        led->delay_on  = state->flashOnMS;
        led->delay_off = state->flashOffMS;
        break;
    default:
        return -EINVAL;
    }

    led->color = state->color & 0x00ffffff;

    if (led->color > 0) {
        /* This LED is lit. */
        if (type >= g_cur_led) {
            /* And it has the highest priority, so show it. */
            err = write_leds(led);
            g_cur_led = type;
        }
    } else {
        /* This LED is not (any longer) lit. */
        if (type == g_cur_led) {
            /* But it is currently showing, switch to a lower-priority LED. */
            int i;

            for (i = type-1; i >= 0; i--) {
                if (g_leds[i].color > 0) {
                    /* Found a lower-priority LED to switch to. */
                    err = write_leds(&g_leds[i]);
                    goto switched;
                }
            }

            /* No LEDs are lit, turn off. */
            err = write_leds(NULL);
switched:
            g_cur_led = i;
        }
    }

    return err;
}

static int
set_light_leds_battery(struct light_device_t *dev,
        struct light_state_t const *state)
{
    return set_light_leds(state, 0);
}

static int
set_light_leds_notifications(struct light_device_t *dev,
        struct light_state_t const *state)
{
    return set_light_leds(state, 1);
}

static int
set_light_leds_attention(struct light_device_t *dev,
        struct light_state_t const *state)
{
    struct light_state_t fixed;

    memcpy(&fixed, state, sizeof(fixed));

    /* The framework does odd things with the attention lights, fix them up to
     * do something sensible here. */
    switch (fixed.flashMode) {
    case LIGHT_FLASH_NONE:
        /* LightsService.Light::stopFlashing calls with non-zero color. */
        fixed.color = 0;
        break;
    case LIGHT_FLASH_HARDWARE:
        /* PowerManagerService::setAttentionLight calls with onMS=3, offMS=0, which
         * just makes for a slightly-dimmer LED. */
        if (fixed.flashOnMS > 0 && fixed.flashOffMS == 0)
            fixed.flashMode = LIGHT_FLASH_NONE;
        break;
    }

    return set_light_leds(&fixed, 2);
}
#endif

#ifdef GENERIC_BLN
static int
set_light_leds_notifications(struct light_device_t* dev,
        struct light_state_t const* state)
{
    int err = 0;
    int on = is_lit(state);
    pthread_mutex_lock(&g_lock);
    err = write_int(NOTIFICATION_FILE, on?1:0);
    pthread_mutex_unlock(&g_lock);
    return err;
}
#endif

static int
set_light_backlight(struct light_device_t* dev,
        struct light_state_t const* state)
{
    int err = 0;
    int brightness = rgb_to_brightness(state);
    pthread_mutex_lock(&g_lock);
    err = write_int(LCD_FILE, brightness);
    pthread_mutex_unlock(&g_lock);
    return err;
}

static int
set_light_buttons(struct light_device_t* dev,
        struct light_state_t const* state)
{
    int err = 0;
    pthread_mutex_lock(&g_lock);
    err = write_int(BUTTON_FILE, state->color & 0xFF);
    pthread_mutex_unlock(&g_lock);
    return err;
}

/** Close the lights device */
static int
close_lights(struct light_device_t *dev)
{
    if (dev) {
        free(dev);
    }
    return 0;
}

/******************************************************************************/

/**
 * module methods
 */

/** Open a new instance of a lights device using name */
static int open_lights(const struct hw_module_t* module, char const* name,
        struct hw_device_t** device)
{
    int (*set_light)(struct light_device_t* dev,
            struct light_state_t const* state);

    if (0 == strcmp(LIGHT_ID_BACKLIGHT, name))
        set_light = set_light_backlight;
    else if (0 == strcmp(LIGHT_ID_BUTTONS, name))
        set_light = set_light_buttons;
#if defined(GENERIC_BLN) || defined(MULTI_COLOR_LED)
    else if (0 == strcmp(LIGHT_ID_NOTIFICATIONS, name))
        set_light = set_light_leds_notifications;
#endif
#ifdef MULTI_COLOR_LED
    else if (0 == strcmp(LIGHT_ID_BATTERY, name))
        set_light = set_light_leds_battery;
    else if (0 == strcmp(LIGHT_ID_ATTENTION, name))
        set_light = set_light_leds_attention;
#endif
    else
        return -EINVAL;

    pthread_once(&g_init, init_globals);

    struct light_device_t *dev = malloc(sizeof(struct light_device_t));
    memset(dev, 0, sizeof(*dev));

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (struct hw_module_t*)module;
    dev->common.close = (int (*)(struct hw_device_t*))close_lights;
    dev->set_light = set_light;

    *device = (struct hw_device_t*)dev;
    return 0;
}

static struct hw_module_methods_t lights_module_methods = {
    .open = open_lights,
};

/*
 * The lights Module
 */
struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .version_major = 1,
    .version_minor = 0,
    .id = LIGHTS_HARDWARE_MODULE_ID,
    .name = "Montblanc lights module",
    .author = "The CyanogenMod Project",
    .methods = &lights_module_methods,
};
