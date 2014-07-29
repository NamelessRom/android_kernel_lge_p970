/*
 * leds-bd2802.c - RGB LED Driver
 *
 * Copyright (C) 2009 Samsung Electronics
 * Kim Kyuwon <q1.kim@samsung.com>
 * Kim Kyungyoon <kyungyoon.kim@lge.com> modified
 * Copyright (C) 2014 Stefan Demharter <stefan.demharter@gmx.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Datasheet: http://www.rohm.com/products/databook/driver/pdf/bd2802gu-e.pdf
 *
 */

#define info(...) dev_info(&bd2802_led->client->dev, __VA_ARGS__)
#define dbg(...) dev_dbg(&bd2802_led->client->dev, __VA_ARGS__)
#define err(...) dev_err(&bd2802_led->client->dev, __VA_ARGS__)

#include <linux/util/ld.h>
#include <linux/util/fade.h>
#include <linux/util/sysfs.h>

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/leds.h>
#include <linux/hrtimer.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/notifier.h>

#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif

int aat2870_als_remove_listener(struct notifier_block *listener);
int aat2870_als_add_listener(struct notifier_block *listener);

#define MODULE_NAME "led-bd2802"

#define RGB_LED_GPIO 128

#define BD2802_REG_CLKSETUP 0x00
#define BD2802_REG_CONTROL 0x01
#define BD2802_REG_HOUR1SETUP 0x02
#define BD2802_REG_HOUR2SETUP 0x0C

#define BD2812_DCDCDRIVER 0x40
#define BD2812_PIN_FUNC_SETUP 0x41

#define U7_MAX 0x7F
#define U8_MAX 0xFF

#define BD2802_CURRENT_DEFAULT 0x46 /* 14.0mA */
#define BRIGHTNESS_MAX 127

static struct mutex mutex;

struct bd2802_led;

static void bd2802_update_active_pattern(struct bd2802_led *bd2802_led);

enum CYCLE {
	CYCLE_131_MS = 0,
	CYCLE_0_52_S = 1,
	CYCLE_1_05_S = 2,
	CYCLE_2_10_S = 3,
	CYCLE_4_19_S = 4,
	CYCLE_8_39_S = 5,
	CYCLE_12_6_S = 6,
	CYCLE_16_8_S = 7,
};

enum SLOPE {
	SLOPE_0 = 0,
	SLOPE_16TH = 1,
	SLOPE_8TH = 2,
	SLOPE_4TH = 3,
};

#define ALS_LEVELS 16

struct led_settings {
	u8 value[2];
	u8 wave : 4;
};

enum OPERATION {
	OPERATION_ONCE,
	OPERATION_PERIODIC,
};

enum LED_DRIVER {
	LED_DRIVER1,
	LED_DRIVER2,
};

enum COLOR {
	RED,
	GREEN,
	BLUE,
};

enum KEY_LED {
	MENU,
	HOME,
	BACK,
	SEARCH,
	BLUELEFT,
	BLUERIGHT,

	KEY_LED_MIN = MENU,
	KEY_LED_MAX = BLUERIGHT,
};

enum BRIGHTNESS_MODE {
	BRIGHTNESS_MODE_USER,
	BRIGHTNESS_MODE_SENSOR,

	BRIGHTNESS_MODE_MIN = BRIGHTNESS_MODE_USER,
	BRIGHTNESS_MODE_MAX = BRIGHTNESS_MODE_SENSOR,
};

const char *BRIGHTNESS_MODE_LONG_STR[] = {
	[BRIGHTNESS_MODE_USER] = "user",
	[BRIGHTNESS_MODE_SENSOR] = "sensor",
};

#define BRIGHTNESS_MODE_STR NULL

#define LEDS (KEY_LED_MAX - KEY_LED_MIN + 1)
#define set_name(x) [x] = #x
char *KEY_LED_STR[LEDS] = {
	set_name(MENU),
	set_name(HOME),
	set_name(BACK),
	set_name(SEARCH),
	set_name(BLUELEFT),
	set_name(BLUERIGHT),
};

enum PATTERN {
	PATTERN_ALL_ON,
	PATTERN_ALL_OFF,
	PATTERN_ALL_BLINKING,
	PATTERN_ALL_ON_BUT_MENU,
	PATTERN_ALL_ON_BUT_HOME,
	PATTERN_ALL_ON_BUT_BACK,
	PATTERN_ALL_ON_BUT_SEARCH,
	PATTERN_CUSTOM_BLINKING,
	PATTERN_CUSTOM_STATIC,

	PATTERN_MIN = PATTERN_ALL_ON,
	PATTERN_MAX = PATTERN_CUSTOM_STATIC,
};

#define ENUM_END(_enum) ((_enum ## _MAX) + 1)

static bool is_blinking[ENUM_END(PATTERN)] = {
	[PATTERN_ALL_BLINKING] = true,
	[PATTERN_CUSTOM_BLINKING] = true,
};

#define set_pattern_name(x) [PATTERN_##x] = #x
static const char *PATTERN_STR[] = {
	set_pattern_name(ALL_ON),
	set_pattern_name(ALL_OFF),
	set_pattern_name(ALL_BLINKING),
	set_pattern_name(ALL_ON_BUT_MENU),
	set_pattern_name(ALL_ON_BUT_HOME),
	set_pattern_name(ALL_ON_BUT_BACK),
	set_pattern_name(ALL_ON_BUT_SEARCH),
	set_pattern_name(CUSTOM_BLINKING),
	set_pattern_name(CUSTOM_STATIC),
};

enum INPUT {
	INPUT_TOUCHKEY,
	INPUT_BUTTON,
	INPUT_PATTERN,

	INPUT_MIN = INPUT_TOUCHKEY,
	INPUT_MAX = INPUT_PATTERN,
};

static const char *INPUT_STR[] = {
	set_name(INPUT_TOUCHKEY),
	set_name(INPUT_BUTTON),
	set_name(INPUT_PATTERN),
};

enum ONOFF {
	ONOFF_OFF,
	ONOFF_DYN_OFF,
	ONOFF_DYN_ON,

	ONOFF_MIN = ONOFF_OFF,
	ONOFF_MAX = ONOFF_DYN_ON,
};

static const char *ONOFF_LONG_STR[] = {
	[ONOFF_OFF] = "off",
	[ONOFF_DYN_OFF] = "on (dyn off)",
	[ONOFF_DYN_ON] = "on (dyn on)",
};

#define ONOFF_STR NULL

struct pattern {
	void (*func)(struct bd2802_led *, const struct pattern *, enum INPUT);
	enum PATTERN type;
	u8 slope_up : 2;
	u8 slope_down : 2;
	u8 cycle_length : 3;
	u8 operation : 1;
	struct led_settings led_settings[LEDS];
};

struct bd2802_led {
	struct i2c_client *client;
	struct mutex mutex;
	struct delayed_work touchkey_delayed_on_work;
	struct delayed_work touchkey_delayed_off_work;

	enum ONOFF onoff;

	/* space for custom pattern from pattern */
	struct pattern custom_pattern;
	enum INPUT active_input;
	/* The following contains just pointers, thus the pointers have to be valid all time */
	const struct pattern *saved_patterns[ENUM_END(INPUT)];
#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
#endif

	struct notifier_block set_brightness_listener;
	enum BRIGHTNESS_MODE brightness_mode;
	enum BRIGHTNESS_MODE brightness_saved_mode;

	int brightness;
	bool button;
	bool touchkey_enabled;
	int brightness_levels[16];
	int adapt_brightness_delay;

	struct fade_props fade_props;
	struct fade fade;

	bool suspend;
};

static struct i2c_client *bd2802_i2c_client;

static const u8 led_driver_offset[] = {
	[LED_DRIVER1] = 0x2,
	[LED_DRIVER2] = 0xC,
};

static const u8 led_color_offset[] = {
	[RED] = 0x1,
	[GREEN] = 0x4,
	[BLUE] = 0x7,
};

struct led_props {
	u8 led_driver;
	u8 color;
};

static const struct led_props led_props[] = {
	[MENU] = {
		.led_driver = LED_DRIVER1,
		.color = GREEN,
	},
	[HOME] = {
		.led_driver = LED_DRIVER2,
		.color = RED,
	},
	[BACK] = {
		.led_driver = LED_DRIVER2,
		.color = GREEN,
	},
	[SEARCH] = {
		.led_driver = LED_DRIVER1,
		.color = RED,
	},
	[BLUELEFT] = {
		.led_driver = LED_DRIVER1,
		.color = BLUE,
	},
	[BLUERIGHT] = {
		.led_driver = LED_DRIVER2,
		.color = BLUE,
	},
};

enum LED_REG {
	LED_REG_CURRENT1 = 0,
	LED_REG_CURRENT2 = 1,
	LED_REG_WAVEPATTERN = 2,
};

enum WAVE {
	WAVE_17 = 0,		/* 12222222 */
	WAVE_26 = 1,		/* 11222222 */
	WAVE_35 = 2,		/* 11122222 */
	WAVE_44 = 3,		/* 11112222 */
	WAVE_53 = 4,		/* 11111222 */
	WAVE_62 = 5,		/* 11111122 */
	WAVE_71 = 6,		/* 11111112 */
	WAVE_8 = 7,			/* 11111111 */
	WAVE_224 = 8,		/* 11221111 */
	WAVE_422 = 9,		/* 11112211 */
	WAVE_12221 = 10,	/* 12211221 */
	WAVE_2222 = 11,		/* 11221122 */
	WAVE_143 = 12,		/* 12222111 */
	WAVE_242 = 13,		/* 11222211 */
	WAVE_341 = 14,		/* 11122221 */
	WAVE_11111111 = 15,	/* 12121212 */

	WAVE_MIN = WAVE_17,
	WAVE_MAX = WAVE_11111111,
};

#define WAVES (WAVE_MAX - WAVE_MIN + 1)

#define LED_ON \
{ \
	.value = {U8_MAX, U8_MAX}, \
	.wave = WAVE_8, \
}
#define LED_OFF \
{ \
	.wave = WAVE_8, \
}
#define LED_BLINK \
{ \
	.value = {0, U8_MAX}, \
	.wave = WAVE_44, \
}

static void bd2802_restore_pattern_or_off(struct bd2802_led *bd2802_led,
	const struct pattern *pattern, enum INPUT input);
static void bd2802_write_pattern(struct bd2802_led *bd2802_led,
	const struct pattern *pattern, enum INPUT input);

static const struct pattern all_on = {
	.func = bd2802_write_pattern,
	.type = PATTERN_ALL_ON,
	.slope_up = SLOPE_0,
	.slope_down = SLOPE_0,
	.cycle_length = CYCLE_16_8_S,
	.operation = OPERATION_PERIODIC,
	.led_settings = {
		[MENU] = LED_ON,
		[HOME] = LED_ON,
		[BACK] = LED_ON,
		[SEARCH] = LED_ON,
		[BLUELEFT] = LED_ON,
		[BLUERIGHT] = LED_ON,
	}
};

static const struct pattern all_blinking = {
	.func = bd2802_write_pattern,
	.type = PATTERN_ALL_BLINKING,
	.slope_up = SLOPE_4TH,
	.slope_down = SLOPE_4TH,
	.cycle_length = CYCLE_2_10_S,
	.operation = OPERATION_PERIODIC,
	.led_settings = {
		[MENU] = LED_BLINK,
		[HOME] = LED_BLINK,
		[BACK] = LED_BLINK,
		[SEARCH] = LED_BLINK,
		[BLUELEFT] = LED_BLINK,
		[BLUERIGHT] = LED_BLINK,
	}
};

#define all_but_first(l0, l1, l2, l3, l4, l5) \
[l0] = { \
	.func = bd2802_write_pattern, \
	.type = PATTERN_ALL_ON_BUT_ ## l0, \
	.slope_up = SLOPE_0, \
	.slope_down = SLOPE_0, \
	.cycle_length = CYCLE_16_8_S, \
	.operation = OPERATION_PERIODIC, \
	.led_settings = { \
		[l0] = LED_OFF, \
		[l1] = LED_ON, \
		[l2] = LED_ON, \
		[l3] = LED_ON, \
		[l4] = LED_ON, \
		[l5] = LED_ON, \
	} \
}
static const struct pattern all_on_but[] = {
	all_but_first(MENU, HOME, BACK, SEARCH, BLUELEFT, BLUERIGHT),
	all_but_first(HOME, MENU, BACK, SEARCH, BLUELEFT, BLUERIGHT),
	all_but_first(BACK, MENU, HOME, SEARCH, BLUELEFT, BLUERIGHT),
	all_but_first(SEARCH, MENU, HOME, BACK, BLUELEFT, BLUERIGHT),
};

static const struct pattern all_off = {
	.func = bd2802_restore_pattern_or_off,
	.type = PATTERN_ALL_OFF,
};

static inline u8 bd2802_get_reg_addr(enum KEY_LED led, enum LED_REG led_reg)
{
	const struct led_props *props = &led_props[led];
	return led_driver_offset[props->led_driver] + led_color_offset[props->color] + led_reg;
}

static int bd2802_write_byte(struct i2c_client *client, u8 reg, u8 val)
{
	struct bd2802_led *bd2802_led = i2c_get_clientdata(client);
	int ret = i2c_smbus_write_byte_data(client, reg, val);

	if (ret < 0) {
		err("reg 0x%x, val 0x%x, err %d\n", reg, val, ret);
		return ret;
	}

	dbg("Written reg 0x%x, val 0x%x\n", reg, val);

	return 0;
}

static void bd2802_led_set(struct bd2802_led *bd2802_led, enum KEY_LED led,
	u8 current1, u8 current2, u8 wave)
{
	u8 reg;

	dbg("Setting %s (%d, %d, %d)\n", KEY_LED_STR[led],
		(int)current1, (int)current2, (int)wave);

	reg = bd2802_get_reg_addr(led, LED_REG_CURRENT1);
	bd2802_write_byte(bd2802_led->client, reg, current1);
	reg = bd2802_get_reg_addr(led, LED_REG_CURRENT2);
	bd2802_write_byte(bd2802_led->client, reg, current2);
	reg = bd2802_get_reg_addr(led, LED_REG_WAVEPATTERN);
	bd2802_write_byte(bd2802_led->client, reg, wave);
}

#define IS_OFF(pattern) (pattern->type == PATTERN_ALL_OFF)
#define IS_ACTIVE(input) ((input) == bd2802_led->active_input)

static void auto_brightness_listener_on(struct bd2802_led *bd2802_led);
static void auto_brightness_listener_off(struct bd2802_led *bd2802_led);

static void bd2802_locked_dyn_on(struct bd2802_led *bd2802_led)
{
	if (bd2802_led->onoff == ONOFF_DYN_OFF) {
		gpio_set_value(RGB_LED_GPIO, 1);
		udelay(200);
		bd2802_write_byte(bd2802_led->client, BD2812_DCDCDRIVER, 0x00);
		bd2802_write_byte(bd2802_led->client, BD2812_PIN_FUNC_SETUP, 0x0F);
		bd2802_led->onoff = ONOFF_DYN_ON;
		auto_brightness_listener_on(bd2802_led);
	}
}

static void bd2802_locked_dyn_off(struct bd2802_led *bd2802_led)
{
	if (bd2802_led->onoff == ONOFF_DYN_ON) {
		auto_brightness_listener_off(bd2802_led);
		bd2802_write_byte(bd2802_led->client, BD2802_REG_CONTROL, 0x00);
		gpio_set_value(RGB_LED_GPIO, 0);
		bd2802_led->onoff = ONOFF_DYN_OFF;
	}
}

static void bd2802_off(struct bd2802_led *bd2802_led)
{
	info("Deactivating key leds\n");

	mutex_lock(&bd2802_led->mutex);

	if (bd2802_led->onoff != ONOFF_OFF) {
		bd2802_locked_dyn_off(bd2802_led);
		bd2802_led->onoff = ONOFF_OFF;
	}

	mutex_unlock(&bd2802_led->mutex);
}

static void bd2802_on(struct bd2802_led *bd2802_led)
{
	info("Reactivating key leds\n");

	mutex_lock(&bd2802_led->mutex);

	if (bd2802_led->onoff == ONOFF_OFF)
		bd2802_led->onoff = ONOFF_DYN_OFF;

	mutex_unlock(&bd2802_led->mutex);

	bd2802_update_active_pattern(bd2802_led);
}

static void bd2802_reset(struct bd2802_led *bd2802_led)
{
	enum INPUT input;

	bd2802_off(bd2802_led);

	mutex_lock(&bd2802_led->mutex);

	for (input = INPUT_MIN; input <= INPUT_MAX; ++input)
		bd2802_led->saved_patterns[input] = &all_off;

	mutex_unlock(&bd2802_led->mutex);

	bd2802_on(bd2802_led);
}

#define IS_DISABLED(input) \
	(input == INPUT_TOUCHKEY && !bd2802_led->touchkey_enabled)

/*
 * Actually write the pattern via i2c.
 */
static void bd2802_write_pattern(struct bd2802_led *bd2802_led,
	const struct pattern *pattern, enum INPUT input)
{
	u8 hour = (pattern->slope_down << 6) | (pattern->slope_up << 4) | pattern->cycle_length;
	u8 control = pattern->operation == OPERATION_ONCE ? 0x22 : 0x11;
	enum KEY_LED led;

	bd2802_led->active_input = input;

	if (bd2802_led->onoff == ONOFF_OFF) {
		dbg("Skipping write of pattern as leds are deactivated\n");
		return;
	}

	if (IS_DISABLED(input)) {
		info("Skipping write as %s is disabled\n", INPUT_STR[input]);
		return;
	}

	bd2802_locked_dyn_on(bd2802_led);

	for (led = KEY_LED_MIN; led <= KEY_LED_MAX; ++led) {
		const struct led_settings *led_settings = &pattern->led_settings[led];
		u8 value0 = led_settings->value[0] * (bd2802_led->brightness + 1) / (U8_MAX + 1);
		u8 value1 = led_settings->value[1] * (bd2802_led->brightness + 1) / (U8_MAX + 1);
		bd2802_led_set(bd2802_led, led, value0, value1, led_settings->wave);
	}

	bd2802_write_byte(bd2802_led->client, BD2802_REG_HOUR1SETUP, hour);
	bd2802_write_byte(bd2802_led->client, BD2802_REG_HOUR2SETUP, hour);
	bd2802_write_byte(bd2802_led->client, BD2802_REG_CONTROL, control);
}

/*
 * This method is called if an an input is set to off.
 * This methods thus either turns the leds off or
 * restores another input if that input is still set.
 */
static void bd2802_restore_pattern_or_off(struct bd2802_led *bd2802_led,
	const struct pattern *pattern, enum INPUT input)
{
	enum INPUT a;
	enum INPUT restore_input = input;
	const struct pattern *restore_pattern = pattern;

	if (!IS_OFF(pattern) && !IS_DISABLED(input))
		return;

	if (!IS_ACTIVE(input))
		/* nothing to do as another pattern is active */
		return;

	/* if there is a saved_pattern try to restore that */
	for (a = INPUT_MIN; a <= INPUT_MAX; ++a) {
		const struct pattern *p = bd2802_led->saved_patterns[a];
		if (a != input && !IS_OFF(p) && !IS_DISABLED(input)) {
			restore_pattern = p;
			restore_input = a;
			info("Restoring pattern %s for %s "
				"instead of setting %s for %s\n",
				PATTERN_STR[restore_pattern->type], INPUT_STR[restore_input],
				PATTERN_STR[pattern->type], INPUT_STR[input]);
			break;
		}
	}

	if (IS_OFF(restore_pattern) || IS_DISABLED(input)) {
		bd2802_locked_dyn_off(bd2802_led);
		bd2802_led->active_input = input;
	} else {
		bd2802_write_pattern(bd2802_led, restore_pattern, restore_input);
	}
}

/*
 * Called when updating the pattern or button brightness or writing on to onoff
 */
static void bd2802_update_active_pattern(struct bd2802_led *bd2802_led)
{
	enum INPUT input;
	const struct pattern *pattern;

	mutex_lock(&bd2802_led->mutex);

	input = bd2802_led->active_input;
	pattern = bd2802_led->saved_patterns[input];
	pattern->func(bd2802_led, pattern, input);

	mutex_unlock(&bd2802_led->mutex);
}

/*
 * This function sets the given pattern and stores it in bd2802_led.
 * There is also some special handling to restore previous patterns in case
 * the parameter "pattern" is an OFF-pattern.
 *
 * Note that the parameter "pattern" has to stay valid beyound the end of this function as it
 * may be used to restore a previous pattern in another call to this function.
 * So it either should be a pointer to a global variable or a pointer to an element of bd2802_led
 */
static void bd2802_set_pattern(struct bd2802_led *bd2802_led,
	const struct pattern *pattern, enum INPUT input)
{
	const struct pattern *active_pattern =
		bd2802_led->saved_patterns[bd2802_led->active_input];

	if (is_blinking[active_pattern->type] || is_blinking[pattern->type])
		fade_finish(&bd2802_led->fade);

	info("Setting pattern %s for %s\n",
		PATTERN_STR[pattern->type], INPUT_STR[input]);

	mutex_lock(&bd2802_led->mutex);

	bd2802_led->saved_patterns[input] = pattern;
	pattern->func(bd2802_led, pattern, input);

	mutex_unlock(&bd2802_led->mutex);
}

static void bd2802_touchkey_on_delayed(struct work_struct *ws)
{
	struct delayed_work *dw = container_of(ws, struct delayed_work, work);
	struct bd2802_led *bd2802_led = container_of(dw, struct bd2802_led, touchkey_delayed_on_work);
	cancel_delayed_work(&bd2802_led->touchkey_delayed_off_work);
	bd2802_set_pattern(bd2802_led, &all_on, INPUT_TOUCHKEY);
	schedule_delayed_work(&bd2802_led->touchkey_delayed_off_work, msecs_to_jiffies(5000));
}

static void bd2802_touchkey_off_delayed(struct work_struct *ws)
{
	struct delayed_work *dw = container_of(ws, struct delayed_work, work);
	struct bd2802_led *bd2802_led = container_of(dw, struct bd2802_led, touchkey_delayed_off_work);
	cancel_delayed_work(&bd2802_led->touchkey_delayed_on_work);
	bd2802_set_pattern(bd2802_led, &all_off, INPUT_TOUCHKEY);
}

void touchkey_pressed(enum KEY_LED led)
{
	mutex_lock(&mutex);

	if (bd2802_i2c_client) {
		struct bd2802_led *bd2802_led = i2c_get_clientdata(bd2802_i2c_client);
		bd2802_set_pattern(bd2802_led, &all_on_but[led], INPUT_TOUCHKEY);
		schedule_delayed_work(&bd2802_led->touchkey_delayed_on_work, msecs_to_jiffies(500));
	}

	mutex_unlock(&mutex);
}
EXPORT_SYMBOL(touchkey_pressed);

static ssize_t pattern_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct bd2802_led *bd2802_led = i2c_get_clientdata(to_i2c_client(dev));
	struct pattern pattern;
	int o = 0;
	unsigned int cycle_length;
	unsigned int slope_up;
	unsigned int slope_down;
	enum KEY_LED led;
	bool is_off = true;
	bool is_blinking = false;

	sscanf(buf, "%u %u %u -%n", &cycle_length, &slope_up, &slope_down, &o);

	if (o == 0) {
		return -EINVAL;
	}

	pattern = (struct pattern) {
		.func = bd2802_write_pattern,
		.cycle_length = cycle_length,
		.slope_up = slope_up,
		.slope_down = slope_down,
		.operation = OPERATION_PERIODIC,
	};

	for (led = KEY_LED_MIN; led <= KEY_LED_MAX; ++led) {
		int read = 0;
		struct led_settings *led_settings = &pattern.led_settings[led];
		unsigned int value0, value1, wave;
		int read_items = sscanf(buf + o, " %u %u %u,%n", &value0, &value1, &wave, &read);

		if (read_items < 3 || read == 0)
			return -EINVAL;

		if (value0 > 100 || value1 > 100) {
			err("Brightness percentage for key %s too high\n", KEY_LED_STR[led]);
			return -EINVAL;
		}

		if (wave > WAVE_MAX) {
			err("Wave value too high (%u > %u)\n", wave, WAVE_MAX);
			return -EINVAL;
		}

		*led_settings = (struct led_settings) {
			.value = {value0 * U8_MAX / 100, value1 * U8_MAX / 100},
			.wave = wave,
		};

		if ((wave == WAVE_8 && value0 > 0) ||
			(wave != WAVE_8 && (value0 > 0 || value1 > 0)))
			is_off = false;

		if (wave != WAVE_8 && (value0 != 0 || value1 != 0))
			is_blinking = true;

		o += read;
	}

	pattern.type = is_blinking ? PATTERN_CUSTOM_BLINKING : PATTERN_CUSTOM_STATIC;

	if (is_off) {
		bd2802_set_pattern(bd2802_led, &all_off, INPUT_PATTERN);
	} else {
		/* store the pattern permanently and pass the stored pattern */
		mutex_lock(&bd2802_led->mutex);
		bd2802_led->custom_pattern = pattern;
		mutex_unlock(&bd2802_led->mutex);

		bd2802_set_pattern(bd2802_led, &bd2802_led->custom_pattern, INPUT_PATTERN);
	}

	return count;
}

static void auto_brightness_listener_on(struct bd2802_led *bd2802_led)
{
	if (bd2802_led->brightness_mode == BRIGHTNESS_MODE_SENSOR) {
		aat2870_als_add_listener(&bd2802_led->set_brightness_listener);
	}
}

static void auto_brightness_listener_off(struct bd2802_led *bd2802_led)
{
	if (bd2802_led->brightness_mode == BRIGHTNESS_MODE_SENSOR) {
		aat2870_als_remove_listener(&bd2802_led->set_brightness_listener);
	}
}

static void *val_from_i2c_dev_with_offset(struct kobject *kobj, struct val_attr *attr)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	char *bdev = dev_get_drvdata(dev);

	return bdev + attr->data;
}

static int update_onoff(struct kobj_props *p, int new_val)
{
	struct device *dev = container_of(p->kobj, struct device, kobj);
	struct bd2802_led *bd2802_led = i2c_get_clientdata(to_i2c_client(dev));

	REQUIRE_ATTR_TYPE(ATTR_TYPE_ENUM);

	if (new_val == ONOFF_OFF)
		bd2802_off(bd2802_led);
	else
		bd2802_on(bd2802_led);

	bd2802_led->onoff = new_val;

	return 0;
}

static int update_button(struct kobj_props *p, bool new_val)
{
	struct device *dev = container_of(p->kobj, struct device, kobj);
	struct bd2802_led *bd2802_led = i2c_get_clientdata(to_i2c_client(dev));

	REQUIRE_ATTR_TYPE(ATTR_TYPE_BOOL);

	bd2802_set_pattern(bd2802_led, new_val ? &all_on : &all_off, INPUT_BUTTON);
	bd2802_led->button = new_val;

	return 0;
}

static int update_touchkey_enabled(struct kobj_props *p, bool new_val)
{
	struct device *dev = container_of(p->kobj, struct device, kobj);
	struct bd2802_led *bd2802_led = i2c_get_clientdata(to_i2c_client(dev));
	const struct pattern *pattern = bd2802_led->saved_patterns[INPUT_TOUCHKEY];

	REQUIRE_ATTR_TYPE(ATTR_TYPE_BOOL);

	bd2802_led->touchkey_enabled = new_val;

	info("%s touchkey leds\n", new_val ? "Activating" : "Deactivating");

	if (new_val)
		bd2802_set_pattern(bd2802_led, pattern, INPUT_TOUCHKEY);
	else
		bd2802_restore_pattern_or_off(bd2802_led, pattern, INPUT_TOUCHKEY);

	return 0;
}

static void set_brightness_mode_to(struct bd2802_led *bd2802_led,
		enum BRIGHTNESS_MODE mode)
{
	if (bd2802_led->brightness_mode != mode &&
		bd2802_led->onoff == ONOFF_DYN_ON) {

		auto_brightness_listener_off(bd2802_led);
		bd2802_led->brightness_mode = mode;
		auto_brightness_listener_on(bd2802_led);
	}

	bd2802_led->brightness_mode = mode;
}

static int update_brightness_mode(struct kobj_props *p, int new_val)
{
	struct device *dev = container_of(p->kobj, struct device, kobj);
	struct bd2802_led *bd2802_led = i2c_get_clientdata(to_i2c_client(dev));

	mutex_lock(&bd2802_led->mutex);

	if (bd2802_led->suspend) {
		info("Delaying setting of brightness mode to `%s` until resume from standby",
			BRIGHTNESS_MODE_LONG_STR[new_val]);
		bd2802_led->brightness_saved_mode = new_val;
	} else {
		set_brightness_mode_to(bd2802_led, new_val);
	}

	mutex_unlock(&bd2802_led->mutex);

	return 0;
}

static void set_or_fade_brightness_delayed_to(struct bd2802_led *bd2802_led,
		int fade_delay_ms, int new_brightness)
{
	const struct pattern *active_pattern =
		bd2802_led->saved_patterns[bd2802_led->active_input];

	if (bd2802_led->brightness == new_brightness)
		return;

	if (is_blinking[active_pattern->type]) {
		dbg("Setting brightness to %d for %s\n", new_brightness,
			PATTERN_STR[active_pattern->type]);
		fade_stop(&bd2802_led->fade);
		bd2802_led->brightness = new_brightness;
		bd2802_update_active_pattern(bd2802_led);
	} else {
		dbg("Fading brightness to %d for %s after %d ms\n", new_brightness,
			PATTERN_STR[active_pattern->type], fade_delay_ms);
		fade_brightness_delayed(&bd2802_led->fade, fade_delay_ms,
			bd2802_led->brightness, new_brightness);
	}
}

static int update_brightness(struct kobj_props *p, int new_val)
{
	struct device *dev = container_of(p->kobj, struct device, kobj);
	struct bd2802_led *bd2802_led = i2c_get_clientdata(to_i2c_client(dev));

	REQUIRE_ATTR_TYPE(ATTR_TYPE_INT);

	if (bd2802_led->brightness_mode != BRIGHTNESS_MODE_USER) {
		info("Won't update brightness as mode is set to `%s` and not `%s`\n",
			BRIGHTNESS_MODE_LONG_STR[bd2802_led->brightness_mode],
			BRIGHTNESS_MODE_LONG_STR[BRIGHTNESS_MODE_USER]);
		return -EINVAL;
	}

	set_or_fade_brightness_delayed_to(bd2802_led, 0, new_val);

	return 0;
}

static int update_brightness_levels(struct kobj_props *p, int *new_vals, size_t size)
{
	struct device *dev = container_of(p->kobj, struct device, kobj);
	struct bd2802_led *bd2802_led = dev_get_drvdata(dev);
	int ret = 0;
	int i;

	REQUIRE_ATTR_TYPE(ATTR_TYPE_INT_ARRAY);

	for (i = 0; i < size; ++i) {
		if (new_vals[i] < 0 || new_vals[i] > BRIGHTNESS_MAX) {
			err("%d not in range [0, %d]", new_vals[i], BRIGHTNESS_MAX);
			return -EINVAL;
		}
	}

	mutex_lock(&bd2802_led->mutex);

	if (size == 1 || size == 2) {
		int min = new_vals[0];
		int max = size == 1 ? BRIGHTNESS_MAX : new_vals[1];

		if (min > max) {
			err("min brightness level %d > max brightness level %d\n", min, max);
			ret = -EINVAL;
		} else {
			util_fill_exp(p->val, ALS_LEVELS, bd2802_led->fade.ld_offset, min, max);
		}
	} else if (size == ALS_LEVELS) {
		int j;

		for (j = 0; j < ALS_LEVELS; ++j) {
			bd2802_led->brightness_levels[j] = new_vals[j];
		}
	} else {
		ret = -EINVAL;
	}

	mutex_unlock(&bd2802_led->mutex);

	return ret;
}

#define I2C_OFFSET(_val) \
	val_from_i2c_dev_with_offset, offsetof(struct bd2802_led, _val)
#define SIZE(_val) \
	ARRAY_SIZE(((struct bd2802_led *)0)->_val)

static DEV_INT_ATTR(brightness, 0644, 4, BRIGHTNESS_MAX,
	I2C_OFFSET(brightness), update_brightness);
static DEV_INT_MIN_MAX_ATTR(brightness, 0644);

static DEV_INT_ATTR(adapt_brightness_delay, 0644, 0, 5000,
	I2C_OFFSET(adapt_brightness_delay), NULL);

static DEV_INT_ARRAY_ATTR(brightness_levels, 0644,
	I2C_OFFSET(brightness_levels), SIZE(brightness_levels),
	update_brightness_levels);

static DEV_ENUM_LONG_ATTR(onoff, 0644, ONOFF, I2C_OFFSET(onoff), update_onoff);

static DEV_INFO_ATTR(info, 0444,
	"To set a custom pattern write integers in the following format to pattern:\n"
	"\n"
	"cycle_length slope_up slope_down - (brightness0 brightness1 wave_pattern,)\n"
	"{for each of the 6 leds: MENU, HOME, BACK, SEARCH, BLUELEFT, BLUERIGHT}\n"
	"\n"
	"cycle_length: 0-15 (representing cycle lengths in the range from 131 ms to 16.8 s (*))\n"
	"slope_up, slope_down: 0-3 (none, 16th, 8th, 4th of the cycle length)\n"
	"brightness{0,1}: in percent\n"
	"wave_pattern: 0-15 (*)\n" /* WAVE_MIN to WAVE_MAX */
	"\n"
	"(*) Have a look at the source code or datasheet for details\n"
	"\n"
	"To disable the pattern write a pattern with all brightness levels set to 0\n"
	"\n"
	"Examples:\n"
	"echo \"3 3 3 - 0 100 12, 0 100 13, 0 100 14, 0 100 3, 100 0 7, 100 0 7\" > pattern\n"
	"echo \"0 0 0 - 0 0 0, 0 0 0, 0 0 0, 0 0 0, 0 0 0, 0 0 0\" > pattern\n"
	"\n"
	"Write 1 to button to illuminate all the buttons or 0 to cancel.\n"
	"\n"
	"Write 0 or 1 to touchkey_enabled to disable or enable the touchkey input");

static DEV_ATTR(pattern, 0200, NULL, pattern_store);

static DEV_BOOL_ATTR(button, 0644, I2C_OFFSET(button), update_button);

static DEV_ENUM_LONG_ATTR(brightness_mode, 0644, BRIGHTNESS_MODE,
	I2C_OFFSET(brightness_mode), update_brightness_mode);

static DEV_BOOL_ATTR(touchkey_enabled, 0644,
	I2C_OFFSET(touchkey_enabled), update_touchkey_enabled);

static const struct device_attribute *bd2802_attributes[] = {
	&dev_val_attr_adapt_brightness_delay.dev_attr,
	&dev_val_attr_button.dev_attr,
	&dev_val_attr_brightness.dev_attr,
	&dev_val_attr_brightness_levels.dev_attr,
	&dev_val_attr_brightness_min_max.dev_attr,
	&dev_val_attr_brightness_mode.dev_attr,
	&dev_val_attr_info.dev_attr,
	&dev_val_attr_onoff.dev_attr,
	&dev_attr_pattern,
	&dev_val_attr_touchkey_enabled.dev_attr,
};

static void set_brightness_to_for_fade(struct fade_props *fade_props,
		uint32_t brightness)
{
	struct bd2802_led *bd2802_led =
		container_of(fade_props, struct bd2802_led, fade_props);

	bd2802_led->brightness = brightness;
	bd2802_update_active_pattern(bd2802_led);
}

static int set_brightness_listener(struct notifier_block *nb,
		unsigned long als_level, void *data)
{
	struct bd2802_led *bd2802_led =
		container_of(nb, struct bd2802_led, set_brightness_listener);

	BUG_ON(als_level >= ARRAY_SIZE(bd2802_led->brightness_levels));

	set_or_fade_brightness_delayed_to(bd2802_led,
		bd2802_led->adapt_brightness_delay, bd2802_led->brightness_levels[als_level]);

	return NOTIFY_OK;
}

static void bd2802_on_resume(struct bd2802_led *bd2802_led)
{
	mutex_lock(&bd2802_led->mutex);

	bd2802_led->suspend = false;
	set_brightness_mode_to(bd2802_led, bd2802_led->brightness_saved_mode);

	mutex_unlock(&bd2802_led->mutex);
}

static void bd2802_on_suspend(struct bd2802_led *bd2802_led)
{
	/* set touchkey input immediately off */
	schedule_delayed_work(&bd2802_led->touchkey_delayed_off_work, 0);

	mutex_lock(&bd2802_led->mutex);

	/*
	 * Userspace usually sets the brightness mode to user upon suspend
	 * and sets the brightness to half of the maximum brightness.
	 *
	 * So brightness-mode sensor is always overriden.
	 * Thus set the mode always to sensor to get a reasonable brightness
	 * and to save power.
	 */
	bd2802_led->brightness_saved_mode = bd2802_led->brightness_mode;
	set_brightness_mode_to(bd2802_led, BRIGHTNESS_MODE_SENSOR);
	bd2802_led->suspend = true;

	mutex_unlock(&bd2802_led->mutex);
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void bd2802_early_suspend(struct early_suspend *h)
{
	struct bd2802_led *bd2802_led = container_of(h, struct bd2802_led, early_suspend);
	bd2802_on_suspend(bd2802_led);
}

static void bd2802_late_resume(struct early_suspend *h)
{
	struct bd2802_led *bd2802_led = container_of(h, struct bd2802_led, early_suspend);
	bd2802_on_resume(bd2802_led);
}
#else
static int bd2802_suspend(struct i2c_client *client, pm_message_t mesg)
{
	struct bd2802_led *bd2802_led = i2c_get_clientdata(client);
	bd2802_on_suspend(bd2802_led);
	return 0;
}

static int bd2802_resume(struct i2c_client *client)
{
	struct bd2802_led *bd2802_led = i2c_get_clientdata(client);
	bd2802_on_resume(bd2802_led);
	return 0;
}
#endif

static void on_fade_release(struct fade_props* fade_props)
{
	struct bd2802_led *bd2802_led =
		container_of(fade_props, struct bd2802_led, fade_props);

	kfree(bd2802_led);
}

static int __devinit bd2802_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct bd2802_led *bd2802_led;
	int ret, i;

	bd2802_led = kzalloc(sizeof(struct bd2802_led), GFP_KERNEL);
	if (!bd2802_led) {
		err("failed to allocate driver data\n");
		return -ENOMEM;
	}

	bd2802_led->client = client;
	i2c_set_clientdata(client, bd2802_led);

	mutex_lock(&mutex);
	bd2802_i2c_client = bd2802_led->client;
	mutex_unlock(&mutex);

	mutex_init(&bd2802_led->mutex);

	bd2802_led->set_brightness_listener.notifier_call = set_brightness_listener;
	bd2802_led->touchkey_enabled = true;

	for (i = 0; i < ARRAY_SIZE(bd2802_attributes); i++) {
		ret = device_create_file(&bd2802_led->client->dev,
						bd2802_attributes[i]);
		if (ret) {
			err("failed: sysfs file %s\n",
					bd2802_attributes[i]->attr.name);
			goto failed_unregister_dev_file;
		}
	}

	INIT_DELAYED_WORK(&bd2802_led->touchkey_delayed_on_work, bd2802_touchkey_on_delayed);
	INIT_DELAYED_WORK(&bd2802_led->touchkey_delayed_off_work, bd2802_touchkey_off_delayed);

	bd2802_led->brightness = BD2802_CURRENT_DEFAULT;

#ifdef CONFIG_HAS_EARLYSUSPEND
	bd2802_led->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN;
	bd2802_led->early_suspend.suspend = bd2802_early_suspend;
	bd2802_led->early_suspend.resume = bd2802_late_resume;
	register_early_suspend(&bd2802_led->early_suspend);
#endif

	bd2802_led->adapt_brightness_delay = 600;

	bd2802_led->fade_props = (struct fade_props) {
		.parent = &client->dev,
		.set_brightness_to = set_brightness_to_for_fade,
		.on_release = on_fade_release,
	};
	fade_init(&bd2802_led->fade, &bd2802_led->fade_props);
	util_fill_exp(bd2802_led->brightness_levels, 16,
		bd2802_led->fade.ld_offset, 8, BRIGHTNESS_MAX);

	bd2802_reset(bd2802_led);
	bd2802_write_pattern(bd2802_led, &all_blinking, INPUT_BUTTON);

	return 0;

failed_unregister_dev_file:
	info("Unregistering dev files\n");

	for (i--; i >= 0; i--)
		device_remove_file(&bd2802_led->client->dev, bd2802_attributes[i]);

	return ret;
}

static int __exit bd2802_remove(struct i2c_client *client)
{
	struct bd2802_led *bd2802_led = i2c_get_clientdata(client);
	int i;

	for (i = 0; i < ARRAY_SIZE(bd2802_attributes); i++)
		device_remove_file(&bd2802_led->client->dev, bd2802_attributes[i]);

	mutex_lock(&mutex);
	bd2802_i2c_client = NULL;
	mutex_unlock(&mutex);

	i2c_set_clientdata(client, NULL);

	cancel_delayed_work_sync(&bd2802_led->touchkey_delayed_on_work);
	cancel_delayed_work_sync(&bd2802_led->touchkey_delayed_off_work);

	bd2802_off(bd2802_led);

	return 0;
}

static const struct i2c_device_id bd2802_id[] = {
	{ "BD2802", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, bd2802_id);

static struct i2c_driver bd2802_i2c_driver = {
	.driver	= {
		.name	= "BD2802",
	},
	.probe		= bd2802_probe,
	.remove		= __exit_p(bd2802_remove),
#ifndef CONFIG_HAS_EARLYSUSPEND /* 20110304 seven.kim@lge.com late_resume_lcd */
	.suspend	= bd2802_suspend,
	.resume		= bd2802_resume,
#endif
	.id_table	= bd2802_id,
};

static int __init bd2802_init(void)
{
	mutex_init(&mutex);
	i2c_add_driver(&bd2802_i2c_driver);
	return 0;
}
module_init(bd2802_init);

static void __exit bd2802_exit(void)
{
	i2c_del_driver(&bd2802_i2c_driver);
	mutex_destroy(&mutex);
}
module_exit(bd2802_exit);

MODULE_AUTHOR("Kim Kyuwon, Stefan Demharter");
MODULE_DESCRIPTION("BD2802 LED driver");
MODULE_LICENSE("GPL v2");
