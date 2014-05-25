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

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/leds.h>
#include <linux/hrtimer.h>
#include <linux/slab.h>
#include <linux/device.h>

#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif

#define MODULE_NAME   "led-bd2802"

#define RGB_LED_GPIO 		128

#define BD2802_REG_CLKSETUP 	0x00
#define BD2802_REG_CONTROL 	0x01
#define BD2802_REG_HOUR1SETUP	0x02
#define BD2802_REG_HOUR2SETUP	0x0C

#define BD2812_DCDCDRIVER	0x40
#define BD2812_PIN_FUNC_SETUP	0x41

#define BD2802_CURRENT_PEAK	0x5A /* 18mA */
#define BD2802_CURRENT_MAX	0x32 /* 10mA */
#define BD2802_CURRENT_MIN	0x05 /* 1mA */
#define BD2802_CURRENT_000	0x00 /* 0.0mA */

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

struct led_settings {
	u8 value : 7;
	u8 invert : 1;
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

enum LED {
	MENU,
	HOME,
	BACK,
	SEARCH,
	BLUELEFT,
	BLUERIGHT,
	LED_FIRST = MENU,
	LED_LAST = BLUERIGHT,
};

#define LEDS (LED_LAST - LED_FIRST + 1)
#define set_name(x) [x] = #x
char *led_str[LEDS] = {
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
	PATTERN_BLUE_ON,
	PATTERN_ALL_ON_BUT_MENU,
	PATTERN_ALL_ON_BUT_HOME,
	PATTERN_ALL_ON_BUT_BACK,
	PATTERN_ALL_ON_BUT_SEARCH,
	PATTERN_CUSTOM,
};

#define set_pattern_name(x) [PATTERN_##x] = #x
static const char *pattern_str[] = {
	set_pattern_name(ALL_ON),
	set_pattern_name(ALL_OFF),
	set_pattern_name(BLUE_ON),
	set_pattern_name(ALL_BLINKING),
	set_pattern_name(ALL_ON_BUT_MENU),
	set_pattern_name(ALL_ON_BUT_HOME),
	set_pattern_name(ALL_ON_BUT_BACK),
	set_pattern_name(ALL_ON_BUT_SEARCH),
	set_pattern_name(CUSTOM),
};

struct pattern {
	enum PATTERN type;
	u8 slope_up : 2;
	u8 slope_down : 2;
	u8 cycle_length : 3;
	u8 operation : 1;
	struct led_settings led_settings[LEDS];
};

enum ACTION {
	ACTION_TOUCHKEY,
	ACTION_BUTTON,
	ACTION_PATTERN,
	ACTION_SAVE_FIRST = ACTION_TOUCHKEY,
	ACTION_SAVE_LAST = ACTION_PATTERN,
	ACTION_OTHER,
};
#define ACTION_SAVE_SIZE (ACTION_SAVE_LAST - ACTION_SAVE_FIRST + 1)

static const char *action_str[] = {
	set_name(ACTION_TOUCHKEY),
	set_name(ACTION_BUTTON),
	set_name(ACTION_PATTERN),
	set_name(ACTION_OTHER),
};

struct bd2802_led {
	struct i2c_client *client;
	struct rw_semaphore rwsem;
	struct delayed_work touchkey_delayed_on_work;
	struct delayed_work touchkey_delayed_off_work;

	/* General attributes of RGB LED_DRIVERs */
	u8 register_value[23];
	u8 enabled;

	/* space for custom pattern from led_pattern */
	struct pattern custom_pattern;
	enum ACTION active_action;
	/* The following contains just pointers, thus the pointers have to be valid all time */
	const struct pattern *saved_patterns[ACTION_SAVE_SIZE];
#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
#endif
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
	WAVE_17 = 0,		// 12222222
	WAVE_26 = 1,		// 11222222
	WAVE_35 = 2,		// 11122222
	WAVE_44 = 3,		// 11112222
	WAVE_53 = 4,		// 11111222
	WAVE_62 = 5,		// 11111122
	WAVE_71 = 6,		// 11111112
	WAVE_8 = 7,		// 11111111
	WAVE_224 = 8,		// 11221111
	WAVE_422 = 9,		// 11112211
	WAVE_12221 = 10,	// 12211221
	WAVE_2222 = 11,		// 11221122
	WAVE_143 = 12,		// 12222111
	WAVE_242 = 13,		// 11222211
	WAVE_351 = 14,		// 11122221
	WAVE_11111111 = 15,	// 12121212
};

#define LED_MAX { .value = BD2802_CURRENT_MAX, .invert = 1, .wave = WAVE_8 }
#define LED_OFF { .value = 0 , .invert = 0, .wave = WAVE_8 }
#define LED_BLINK { .value = BD2802_CURRENT_MAX, .invert = 0, .wave = WAVE_44 }

static const struct pattern all_on = {
	.type = PATTERN_ALL_ON,
	.slope_up = SLOPE_0,
	.slope_down = SLOPE_0,
	.cycle_length = CYCLE_16_8_S,
	.operation = OPERATION_PERIODIC,
	.led_settings = {
		[MENU] = LED_MAX,
		[HOME] = LED_MAX,
		[BACK] = LED_MAX,
		[SEARCH] = LED_MAX,
		[BLUELEFT] = LED_MAX,
		[BLUERIGHT] = LED_MAX,
	}
};
static const struct pattern blue_on = {
	.type = PATTERN_BLUE_ON,
	.slope_up = SLOPE_0,
	.slope_down = SLOPE_0,
	.cycle_length = CYCLE_16_8_S,
	.operation = OPERATION_PERIODIC,
	.led_settings = {
		[MENU] = LED_OFF,
		[HOME] = LED_OFF,
		[BACK] = LED_OFF,
		[SEARCH] = LED_OFF,
		[BLUELEFT] = LED_MAX,
		[BLUERIGHT] = LED_MAX,
	}
};
static const struct pattern all_blinking = {
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
	.type = PATTERN_ALL_ON_BUT_ ## l0, \
	.slope_up = SLOPE_0, \
	.slope_down = SLOPE_0, \
	.cycle_length = CYCLE_16_8_S, \
	.operation = OPERATION_PERIODIC, \
	.led_settings = { \
		[l0] = LED_OFF, \
		[l1] = LED_MAX, \
		[l2] = LED_MAX, \
		[l3] = LED_MAX, \
		[l4] = LED_MAX, \
		[l5] = LED_MAX, \
	} \
}
static const struct pattern all_on_but[] = {
	all_but_first(MENU, HOME, BACK, SEARCH, BLUELEFT, BLUERIGHT),
	all_but_first(HOME, MENU, BACK, SEARCH, BLUELEFT, BLUERIGHT),
	all_but_first(BACK, MENU, HOME, SEARCH, BLUELEFT, BLUERIGHT),
	all_but_first(SEARCH, MENU, HOME, BACK, BLUELEFT, BLUERIGHT),
};

static const struct pattern all_off = {
	.type = PATTERN_ALL_OFF,
	.slope_up = SLOPE_0,
	.slope_down = SLOPE_0,
	.cycle_length = CYCLE_131_MS,
	.operation = OPERATION_ONCE,
	.led_settings = {
		[MENU] = LED_OFF,
		[HOME] = LED_OFF,
		[BACK] = LED_OFF,
		[SEARCH] = LED_OFF,
		[BLUELEFT] = LED_OFF,
		[BLUERIGHT] = LED_OFF,
	}
};

static inline u8 bd2802_get_reg_addr(enum LED led, enum LED_REG led_reg)
{
	const struct led_props *props = &led_props[led];
	return led_driver_offset[props->led_driver] + led_color_offset[props->color] + led_reg;
}


/*--------------------------------------------------------------*/
/*	BD2802GU core functions					*/
/*--------------------------------------------------------------*/

static int bd2802_write_byte(struct i2c_client *client, u8 reg, u8 val)
{
	struct bd2802_led *bd2802_led = i2c_get_clientdata(client);
	int ret = 0;
	int reg_addr = (int)(reg);

	ret = i2c_smbus_write_byte_data(client, reg, val);

	if (ret >= 0) {
		if (reg_addr < ARRAY_SIZE(bd2802_led->register_value)) {
			bd2802_led->register_value[reg_addr] = val;
		}
		return 0;
	}

	dev_err(&client->dev, "%s: reg 0x%x, val 0x%x, err %d\n",
			__func__, reg, val, ret);

	return ret;
}

static void bd2802_led_set(struct bd2802_led *bd2802_led, enum LED led, u8 current1, u8 current2, u8 wave)
{
	u8 reg;
	printk(KERN_DEBUG "bd2802: Setting %s (%d, %d, %d)\n", led_str[led],
		current1, current2, wave);
	reg = bd2802_get_reg_addr(led, LED_REG_CURRENT1);
	bd2802_write_byte(bd2802_led->client, reg, current1);
	reg = bd2802_get_reg_addr(led, LED_REG_CURRENT2);
	bd2802_write_byte(bd2802_led->client, reg, current2);
	reg = bd2802_get_reg_addr(led, LED_REG_WAVEPATTERN);
	bd2802_write_byte(bd2802_led->client, reg, wave);
}

void bd2802_set_pattern_internal(struct bd2802_led *bd2802_led, const struct pattern *pattern)
{
	u8 hour = (pattern->slope_down << 6) | (pattern->slope_up << 4) | pattern->cycle_length;
	u8 control = pattern->operation == OPERATION_ONCE ? 0x22 : 0x11;
	enum LED led;
	for (led = LED_FIRST; led <= LED_LAST; ++led) {
		const struct led_settings *led_settings = &pattern->led_settings[led];
		u8 invert = led_settings->invert;
		u8 current1 = invert ? led_settings->value : 0;
		u8 current2 = !invert ? led_settings->value : 0;
		bd2802_led_set(bd2802_led, led, current1, current2, led_settings->wave);
	}
	bd2802_write_byte(bd2802_led->client, 0x02, hour);
	bd2802_write_byte(bd2802_led->client, 0x0C, hour);
	bd2802_write_byte(bd2802_led->client, 0x01, control);
}

#define IS_OFF(pattern) (pattern == NULL || pattern->type == PATTERN_ALL_OFF)
#define IS_TO_BE_SAVED(action) (action >= ACTION_SAVE_FIRST && action <= ACTION_SAVE_LAST)
#define IS_ACTIVE(action) ((action) == bd2802_led->active_action)

/*
 * This function sets the given pattern.
 * There is also some special handling to restore previous patterns in case the parameter "pattern"
 * should turn the leds off.
 *
 * Note that the parameter "pattern" has to stay valid beyound the end of this function as it
 * may be used to restore a previous pattern in another call to this function.
 * So it either has to be a pointer to a static value or a pointer to an element of bd2802_led
 */
static void bd2802_set_pattern(struct bd2802_led *bd2802_led, const struct pattern *pattern, enum ACTION action)
{
	/*
         * set_pattern and set_action may be overwritten in this function
         * by bd2802_led->saved_pattern and the corresponding action.
         */
	const struct pattern *set_pattern = pattern;
	enum ACTION set_action = action;

	printk(KERN_DEBUG "bd2802: Setting pattern %s from action %s\n",
		pattern_str[pattern->type], action_str[action]);

	down_write(&bd2802_led->rwsem);

	if (IS_TO_BE_SAVED(action)) {
		bd2802_led->saved_patterns[action] = pattern;
		if (IS_OFF(pattern) && !IS_ACTIVE(action)) {
			/* nothing to do... */
			goto end; // nothing to do
		}
		if (IS_OFF(pattern)) {
			enum ACTION a;
			/* if there is a saved_pattern try to restore that */
			for (a = ACTION_SAVE_FIRST; a <= ACTION_SAVE_LAST; ++a) {
				const struct pattern *p_a = bd2802_led->saved_patterns[a];
				if (a != action && !IS_OFF(p_a)) {
					set_pattern = p_a;
					set_action = a;
					printk(KERN_DEBUG "bd2802: Restoring pattern %s for action %s "
						"instead of %s from %s\n",
						pattern_str[set_pattern->type], action_str[set_action],
						pattern_str[pattern->type], action_str[action]);
					break;
				}
			}
		}
	}
	bd2802_set_pattern_internal(bd2802_led, set_pattern);
	bd2802_led->active_action = set_action;

end:
	up_write(&bd2802_led->rwsem);
}

static void bd2802_enable(struct bd2802_led *bd2802_led)
{
	down_write(&bd2802_led->rwsem);
	bd2802_write_byte(bd2802_led->client, BD2812_DCDCDRIVER, 0x00);
	bd2802_write_byte(bd2802_led->client, BD2812_PIN_FUNC_SETUP, 0x0F);
	bd2802_led->enabled = true;
	up_write(&bd2802_led->rwsem);
}

static void bd2802_touchkey_on_delayed(struct work_struct *ws)
{
	struct delayed_work *dw = container_of(ws, struct delayed_work, work);
	struct bd2802_led *bd2802_led = container_of(dw, struct bd2802_led, touchkey_delayed_on_work);
	bd2802_set_pattern(bd2802_led, &all_on, ACTION_TOUCHKEY);
	schedule_delayed_work(&bd2802_led->touchkey_delayed_off_work, msecs_to_jiffies(5000));
}

static void bd2802_touchkey_off_delayed(struct work_struct *ws)
{
	struct delayed_work *dw = container_of(ws, struct delayed_work, work);
	struct bd2802_led *bd2802_led = container_of(dw, struct bd2802_led, touchkey_delayed_off_work);
	bd2802_set_pattern(bd2802_led, &all_off, ACTION_TOUCHKEY);
}

static void bd2802_disable(struct bd2802_led *bd2802_led)
{
	down_write(&bd2802_led->rwsem);
	bd2802_write_byte(bd2802_led->client, BD2802_REG_CONTROL, 0x00);
	bd2802_led->enabled = 0;
	up_write(&bd2802_led->rwsem);
}

void touchkey_pressed(enum LED led)
{
	struct bd2802_led *bd2802_led = i2c_get_clientdata(bd2802_i2c_client);

	bd2802_set_pattern(bd2802_led, &all_on_but[led], ACTION_TOUCHKEY);
	schedule_delayed_work(&bd2802_led->touchkey_delayed_on_work, msecs_to_jiffies(500));
}
EXPORT_SYMBOL(touchkey_pressed);

static ssize_t led_enable_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct bd2802_led *bd2802_led = i2c_get_clientdata(to_i2c_client(dev));
	return snprintf(buf, PAGE_SIZE, "%s\n", bd2802_led->enabled ? "enabled" : "disabled");
}

static inline int streq(const char *str1, const char *str2)
{
	return strcmp(str1, str2) == 0;
}

static ssize_t led_enable_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct bd2802_led *bd2802_led = i2c_get_clientdata(to_i2c_client(dev));

	if (streq("enable\n", buf)) {
		bd2802_enable(bd2802_led);
	} else if (streq("disable\n", buf)) {
		bd2802_disable(bd2802_led);
	} else {
		return -EINVAL;
	}

	return count;
}

static ssize_t led_button_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE,
		"Write 1 to this file and all the buttons illuminate "
		"(can be cancelled at some point with 0), "
		"write 0 to force off.\n");
}

static ssize_t led_button_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct bd2802_led *bd2802_led = i2c_get_clientdata(bd2802_i2c_client);
	u8 val = simple_strtoul(buf, NULL, 10);
	if (val == 0) {
		bd2802_set_pattern(bd2802_led, &all_off, ACTION_BUTTON);
	} else if (val == 1) {
		bd2802_set_pattern(bd2802_led, &all_on, ACTION_BUTTON);
	} else {
		return -EINVAL;
	}
	return count;
}

static ssize_t led_pattern_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE,
		"Write integers in the following format:\n\n"
		"cycle_length slope_up slope_down - (brightness wave-pattern,)"
		"{for each of the 6 leds: MENU, HOME, BACK, SEARCH, BLUELEFT, BLUERIGHT}\n\n"
		"cycle_length: 0-15 (representing times from 131 ms to 16.8 s)\n"
                "slope_up: 0-3 (from none to a 4th of the wave length)\n"
                "brightness: 0-90\n" // BD2802_CURRENT_PEAK
		"wave-pattern: 0-31 (whereas 16-31 are the inverses of 0-15\n\n"
		"Example:\n"
		"echo \"3 3 3 - 50 12, 50 13, 50 14, 50 3, 50 23, 50 23\" > led_pattern\n");
}
static ssize_t led_pattern_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct bd2802_led *bd2802_led = i2c_get_clientdata(to_i2c_client(dev));

	struct pattern pattern = {
		.type = PATTERN_CUSTOM,
	};
	int read = 0;
	int cycle_length;
	int slope_up;
	int slope_down;
	enum LED led;
	int values_set = 0;
	sscanf(buf, "%d %d %d -%n", &cycle_length, &slope_up, &slope_down, &read);
	if (read == 0) {
		return -EINVAL;
	}
	pattern.cycle_length = cycle_length;
	pattern.slope_up = slope_up;
	pattern.slope_down = slope_down;
	pattern.operation = OPERATION_PERIODIC;
	for (led = LED_FIRST; led <= LED_LAST; ++led) {
		int read2 = 0;
		struct led_settings *led_settings = &pattern.led_settings[led];
		int value, wave;
		int read_items = sscanf(buf + read, " %d %d,%n", &value, &wave, &read2);
		if (read_items < 2 || read2 == 0) {
			return -EINVAL;
		}
		if (value > BD2802_CURRENT_PEAK) {
			printk(KERN_ERR "brightness for key %s too high (%d > %d)\n",
				led_str[led], value, BD2802_CURRENT_PEAK);
			return -EINVAL;
		}

		led_settings->invert = wave >= 16;
		led_settings->value = value;
		led_settings->wave = wave;
		if (value > 0) {
			++values_set;
		}
		read += read2;
	}

	if (values_set == 0) { // i.e. turn off
		bd2802_set_pattern(bd2802_led, &all_off, ACTION_PATTERN);
	} else {
		// need to store that pattern permanently;
		bd2802_led->custom_pattern = pattern;
		bd2802_set_pattern(bd2802_led, &bd2802_led->custom_pattern, ACTION_PATTERN);
	}

	return count;
}

DEVICE_ATTR(led_enable, 0644, led_enable_show, led_enable_store);
DEVICE_ATTR(led_pattern, 0644, led_pattern_show, led_pattern_store);
DEVICE_ATTR(led_button, 0644, led_button_show, led_button_store);

struct reg_attr {
	u8 reg;
	struct device_attribute attr;
};

static ssize_t reg_store(struct device* dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct reg_attr *reg_attr = container_of(attr, struct reg_attr, attr);
	struct bd2802_led *bd2802_led = i2c_get_clientdata(to_i2c_client(dev));
	unsigned long val;
	int ret;

	if (!count)
		return -EINVAL;

	val = simple_strtoul(buf, NULL, 10);
	down_write(&bd2802_led->rwsem);
	bd2802_led->register_value[reg_attr->reg] = (u8) val;
	ret = bd2802_write_byte(bd2802_led->client, reg_attr->reg, (u8) val);
	up_write(&bd2802_led->rwsem);
	return count;
}

static ssize_t reg_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct reg_attr *reg_attr = container_of(attr, struct reg_attr, attr);
	struct bd2802_led *bd2802_led = i2c_get_clientdata(to_i2c_client(dev));
	u8 val = bd2802_led->register_value[reg_attr->reg];
	return snprintf(buf, PAGE_SIZE, "%d\n", (int) val);
}

#define BD2802_SET_REGISTER(reg_addr) \
static const struct reg_attr reg_##reg_addr = { \
	.reg = reg_addr, \
	.attr = { \
		.attr = { \
			.name = #reg_addr, \
			.mode = 0644, \
		}, \
		.store = reg_store, \
		.show = reg_show, \
	} \
};

BD2802_SET_REGISTER(0x00); // CLKSETUP
BD2802_SET_REGISTER(0x01); // LED_DRIVERCONTROL
BD2802_SET_REGISTER(0x02); // RGB1_HOURSETUP
BD2802_SET_REGISTER(0x03); // R1_CURRENT1
BD2802_SET_REGISTER(0x04); // R1_CURRENT2
BD2802_SET_REGISTER(0x05); // R1_PATTERN
BD2802_SET_REGISTER(0x06); // G1_CURRENT1
BD2802_SET_REGISTER(0x07); // G1_CURRENT2
BD2802_SET_REGISTER(0x08); // G1_PATTERN
BD2802_SET_REGISTER(0x09); // B1_CURRENT1
BD2802_SET_REGISTER(0x0a); // B1_CURRENT2
BD2802_SET_REGISTER(0x0b); // B1_PATTERN
BD2802_SET_REGISTER(0x0c); // RGB2_HOURSETUP
BD2802_SET_REGISTER(0x0d); // R2_CURRENT1
BD2802_SET_REGISTER(0x0e); // R2_CURRENT2
BD2802_SET_REGISTER(0x0f); // R2_PATTERN
BD2802_SET_REGISTER(0x10); // G2_CURRENT1
BD2802_SET_REGISTER(0x11); // G2_CURRENT2
BD2802_SET_REGISTER(0x12); // G2_PATTERN
BD2802_SET_REGISTER(0x13); // B2_CURRENT1
BD2802_SET_REGISTER(0x14); // B2_CURRENT2
BD2802_SET_REGISTER(0x15); // B2_PATTERN

static const struct device_attribute *bd2802_attributes[] = {
	&dev_attr_led_pattern,
	&dev_attr_led_enable,
	&dev_attr_led_button,
	&reg_0x00.attr,
	&reg_0x01.attr,
	&reg_0x02.attr,
	&reg_0x03.attr,
	&reg_0x04.attr,
	&reg_0x05.attr,
	&reg_0x06.attr,
	&reg_0x07.attr,
	&reg_0x08.attr,
	&reg_0x09.attr,
	&reg_0x0a.attr,
	&reg_0x0b.attr,
	&reg_0x0c.attr,
	&reg_0x0d.attr,
	&reg_0x0e.attr,
	&reg_0x0f.attr,
	&reg_0x10.attr,
	&reg_0x11.attr,
	&reg_0x12.attr,
	&reg_0x13.attr,
	&reg_0x14.attr,
	&reg_0x15.attr,
};

static void bd2802_on_resume(struct bd2802_led *bd2802_led)
{
	bd2802_enable(bd2802_led);
	bd2802_set_pattern(bd2802_led, &blue_on, ACTION_TOUCHKEY);
	schedule_delayed_work(&bd2802_led->touchkey_delayed_on_work, msecs_to_jiffies(1500));
}

static void bd2802_on_suspend(struct bd2802_led *bd2802_led)
{
	/* Cancel all touchkey work and set touchkey immediately off */
	cancel_delayed_work_sync(&bd2802_led->touchkey_delayed_on_work);
	cancel_delayed_work_sync(&bd2802_led->touchkey_delayed_off_work);
	bd2802_set_pattern(bd2802_led, &all_off, ACTION_TOUCHKEY);
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

static int __devinit bd2802_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct bd2802_led *bd2802_led;
	int ret, i;
	const struct pattern *pattern;

	bd2802_led = kzalloc(sizeof(struct bd2802_led), GFP_KERNEL);
	if (!bd2802_led) {
		dev_err(&client->dev, "failed to allocate driver data\n");
		return -ENOMEM;
	}

	bd2802_led->client = client;
	i2c_set_clientdata(client, bd2802_led);

	bd2802_i2c_client = bd2802_led->client;

	init_rwsem(&bd2802_led->rwsem);

	for (i = 0; i < ARRAY_SIZE(bd2802_attributes); i++) {
		ret = device_create_file(&bd2802_led->client->dev,
						bd2802_attributes[i]);
		if (ret) {
			dev_err(&bd2802_led->client->dev, "failed: sysfs file %s\n",
					bd2802_attributes[i]->attr.name);
			goto failed_unregister_dev_file;
		}
	}

	INIT_DELAYED_WORK(&bd2802_led->touchkey_delayed_on_work, bd2802_touchkey_on_delayed);
	INIT_DELAYED_WORK(&bd2802_led->touchkey_delayed_off_work, bd2802_touchkey_off_delayed);

#ifdef CONFIG_HAS_EARLYSUSPEND
	bd2802_led->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN;
	bd2802_led->early_suspend.suspend = bd2802_early_suspend;
	bd2802_led->early_suspend.resume = bd2802_late_resume;
	register_early_suspend(&bd2802_led->early_suspend);
#endif

	gpio_set_value(RGB_LED_GPIO, 1);
	udelay(200);
	bd2802_enable(bd2802_led);
	pattern = &all_blinking;
	bd2802_set_pattern(bd2802_led, pattern, ACTION_OTHER);

	return 0;

failed_unregister_dev_file:
	printk(KERN_INFO "bd2802: unregistering dev files\n");
	for (i--; i >= 0; i--)
		device_remove_file(&bd2802_led->client->dev, bd2802_attributes[i]);

	return ret;
}

static int __exit bd2802_remove(struct i2c_client *client)
{
	struct bd2802_led *bd2802_led = i2c_get_clientdata(client);
	int i;

	cancel_delayed_work_sync(&bd2802_led->touchkey_delayed_on_work);
	cancel_delayed_work_sync(&bd2802_led->touchkey_delayed_off_work);

	gpio_set_value(RGB_LED_GPIO, 0);

	for (i = 0; i < ARRAY_SIZE(bd2802_attributes); i++)
		device_remove_file(&bd2802_led->client->dev, bd2802_attributes[i]);
	i2c_set_clientdata(client, NULL);
	kfree(bd2802_led);

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
	i2c_add_driver(&bd2802_i2c_driver);
	return 0;
}
module_init(bd2802_init);

static void __exit bd2802_exit(void)
{
	i2c_del_driver(&bd2802_i2c_driver);
}
module_exit(bd2802_exit);

MODULE_AUTHOR("Kim Kyuwon, Stefan Demharter");
MODULE_DESCRIPTION("BD2802 LED driver");
MODULE_LICENSE("GPL v2");
