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

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/leds.h>
#include <linux/hrtimer.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/mutex.h>

#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif

#define MODULE_NAME "led-bd2802"

#define RGB_LED_GPIO 		128

#define BD2802_REG_CLKSETUP 	0x00
#define BD2802_REG_CONTROL 	0x01
#define BD2802_REG_HOUR1SETUP	0x02
#define BD2802_REG_HOUR2SETUP	0x0C

#define BD2812_DCDCDRIVER	0x40
#define BD2812_PIN_FUNC_SETUP	0x41

#define U7_MAX 0x7F

#define BD2802_CURRENT_DEFAULT	0x46 /* 14.0mA */

static u8 pattern_brightness = BD2802_CURRENT_DEFAULT;
static u8 button_brightness = BD2802_CURRENT_DEFAULT;

static struct mutex mutex;

struct bd2802_led;

struct u8_attribute {
	struct device_attribute attr;
	u8 *var;
	void (*update_func)(struct bd2802_led *bd2802_led);
};

static void bd2802_update_active_pattern(struct bd2802_led *bd2802_led);

ssize_t device_store_u7_update(struct device *dev, struct device_attribute *attr,
	 const char *buf, size_t count)
{
	struct u8_attribute *u8_attr = container_of(attr, struct u8_attribute, attr);
	struct bd2802_led *bd2802_led = i2c_get_clientdata(to_i2c_client(dev));
	u8 *var = u8_attr->var;
	u8 val = simple_strtoul(buf, NULL, 10);
	if (val > U7_MAX) {
		pr_err("Value too big (%d > %d)", val, U7_MAX);
		return -EINVAL;
	}
	*var = val;
	u8_attr->update_func(bd2802_led);
	return count;
}

ssize_t device_show_u7(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct u8_attribute *u8_attr = container_of(attr, struct u8_attribute, attr);
	return snprintf(buf, PAGE_SIZE, "%u (range: 0-127)\n", (unsigned int)*(u8_attr->var));
}

#define DEVICE_U7_UPDATE_ATTR(_name, _mode, _var, _update_func) \
	struct u8_attribute dev_attr_##_name = \
		{ \
			__ATTR(_name, _mode, device_show_u7, device_store_u7_update), \
			 &(_var), \
			_update_func, \
		}

static DEVICE_U7_UPDATE_ATTR(pattern_brightness, 0644, pattern_brightness, bd2802_update_active_pattern);
static DEVICE_U7_UPDATE_ATTR(button_brightness, 0644, button_brightness, bd2802_update_active_pattern);

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

enum VALUE_TYPE {
	VALUE_TYPE_DEFAULT = 0,
	VALUE_TYPE_SPECIAL = 1,
};

enum SPECIAL_VALUE {
	SPECIAL_VALUE_BUTTON_BRIGHTNESS,
	SPECIAL_VALUE_MAX_BRIGHTNESS,
};

u8 *const special_values[] = {
	[SPECIAL_VALUE_BUTTON_BRIGHTNESS] = &button_brightness,
	[SPECIAL_VALUE_MAX_BRIGHTNESS] = &pattern_brightness,
};

struct value {
	u8 value_type : 1;
	u8 value : 7;
};

struct led_settings {
	struct value values[2];
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

enum INPUT {
	INPUT_TOUCHKEY,
	INPUT_BUTTON,
	INPUT_PATTERN,

	INPUT_FIRST = INPUT_TOUCHKEY,
	INPUT_LAST = INPUT_PATTERN,
};

#define ENUM_SIZE(_enum) ((_enum ## _LAST) - (_enum ## _FIRST) + 1)

static const char *input_str[] = {
	set_name(INPUT_TOUCHKEY),
	set_name(INPUT_BUTTON),
	set_name(INPUT_PATTERN),
};

enum ONOFF {
	ONOFF_OFF,
	ONOFF_DYN_OFF,
	ONOFF_DYN_ON,
};

static const char *onoff_str[] = {
	[ONOFF_OFF] = "off",
	[ONOFF_DYN_OFF] = "on (dyn off)",
	[ONOFF_DYN_ON] = "on (dyn on)",
};

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
	struct rw_semaphore rwsem;
	struct delayed_work touchkey_delayed_on_work;
	struct delayed_work touchkey_delayed_off_work;

	/* General attributes of RGB LED_DRIVERs */
	u8 register_value[23];
	enum ONOFF onoff;

	/* space for custom pattern from pattern */
	struct pattern custom_pattern;
	enum INPUT active_input;
	/* The following contains just pointers, thus the pointers have to be valid all time */
	const struct pattern *saved_patterns[ENUM_SIZE(INPUT)];
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
	WAVE_351 = 14,		/* 11122221 */
	WAVE_11111111 = 15,	/* 12121212 */
	WAVE_LAST = WAVE_11111111,
	WAVE_FIRST = WAVE_17,
};

#define WAVES (WAVE_LAST - WAVE_FIRST + 1)

#define LED_MAX \
{ \
	.values = { \
		{ \
			.value_type = VALUE_TYPE_SPECIAL, \
			.value = SPECIAL_VALUE_BUTTON_BRIGHTNESS, \
		}, \
	}, \
	.wave = WAVE_8, \
}
#define LED_OFF \
{ \
	.wave = WAVE_8, \
}
#define LED_BLINK \
{ \
	.values = { \
		{ \
		}, \
		{ \
			.value_type = VALUE_TYPE_SPECIAL, \
			.value = SPECIAL_VALUE_BUTTON_BRIGHTNESS, \
		}, \
	}, \
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
		[MENU] = LED_MAX,
		[HOME] = LED_MAX,
		[BACK] = LED_MAX,
		[SEARCH] = LED_MAX,
		[BLUELEFT] = LED_MAX,
		[BLUERIGHT] = LED_MAX,
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
	.func = bd2802_restore_pattern_or_off,
	.type = PATTERN_ALL_OFF,
};

static inline u8 bd2802_get_reg_addr(enum LED led, enum LED_REG led_reg)
{
	const struct led_props *props = &led_props[led];
	return led_driver_offset[props->led_driver] + led_color_offset[props->color] + led_reg;
}

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

static void bd2802_led_set(struct bd2802_led *bd2802_led, enum LED led,
	u8 current1, u8 current2, u8 wave)
{
	u8 reg;
	pr_debug("Setting %s (%d, %d, %d)\n", led_str[led],
		current1, current2, wave);
	reg = bd2802_get_reg_addr(led, LED_REG_CURRENT1);
	bd2802_write_byte(bd2802_led->client, reg, current1);
	reg = bd2802_get_reg_addr(led, LED_REG_CURRENT2);
	bd2802_write_byte(bd2802_led->client, reg, current2);
	reg = bd2802_get_reg_addr(led, LED_REG_WAVEPATTERN);
	bd2802_write_byte(bd2802_led->client, reg, wave);
}

#define IS_OFF(pattern) (pattern->type == PATTERN_ALL_OFF)
#define IS_ACTIVE(input) ((input) == bd2802_led->active_input)

static void bd2802_locked_dyn_on(struct bd2802_led *bd2802_led)
{
	if (bd2802_led->onoff == ONOFF_DYN_OFF) {
		gpio_set_value(RGB_LED_GPIO, 1);
		udelay(200);
		bd2802_write_byte(bd2802_led->client, BD2812_DCDCDRIVER, 0x00);
		bd2802_write_byte(bd2802_led->client, BD2812_PIN_FUNC_SETUP, 0x0F);
		bd2802_led->onoff = ONOFF_DYN_ON;
	}
}

static void bd2802_locked_dyn_off(struct bd2802_led *bd2802_led)
{
	if (bd2802_led->onoff == ONOFF_DYN_ON) {
		bd2802_write_byte(bd2802_led->client, BD2802_REG_CONTROL, 0x00);
		gpio_set_value(RGB_LED_GPIO, 0);
		bd2802_led->onoff = ONOFF_DYN_OFF;
	}
}

static void bd2802_off(struct bd2802_led *bd2802_led)
{
	pr_info("Deactivating key leds\n");
	down_write(&bd2802_led->rwsem);
	if (bd2802_led->onoff != ONOFF_OFF) {
		bd2802_locked_dyn_off(bd2802_led);
		bd2802_led->onoff = ONOFF_OFF;
	}
	up_write(&bd2802_led->rwsem);
}

static void bd2802_on(struct bd2802_led *bd2802_led)
{
	pr_info("Reactivating key leds\n");
	down_write(&bd2802_led->rwsem);
	if (bd2802_led->onoff == ONOFF_OFF) {
		bd2802_led->onoff = ONOFF_DYN_OFF;
	}
	up_write(&bd2802_led->rwsem);
	bd2802_update_active_pattern(bd2802_led);
}

static void bd2802_reset(struct bd2802_led *bd2802_led)
{
	enum INPUT input;
	bd2802_off(bd2802_led);
	down_write(&bd2802_led->rwsem);
	for (input = INPUT_FIRST; input <= INPUT_LAST; ++input) {
		bd2802_led->saved_patterns[input] = &all_off;
	}
	up_write(&bd2802_led->rwsem);
	bd2802_on(bd2802_led);
}

#define CALC_VALUE(value_struct) \
	((value_struct).value_type == VALUE_TYPE_SPECIAL ? \
	*special_values[(value_struct).value] : \
	(unsigned int)(value_struct).value * pattern_brightness / U7_MAX)

/*
 * Actually write the pattern via i2c.
 */
static void bd2802_write_pattern(struct bd2802_led *bd2802_led,
	const struct pattern *pattern, enum INPUT input)
{
	u8 hour = (pattern->slope_down << 6) | (pattern->slope_up << 4) | pattern->cycle_length;
	u8 control = pattern->operation == OPERATION_ONCE ? 0x22 : 0x11;
	enum LED led;

	bd2802_led->active_input = input;

	if (bd2802_led->onoff == ONOFF_OFF) {
		pr_info("Skipping write of pattern as leds are deactivated\n");
		return;
	}

	bd2802_locked_dyn_on(bd2802_led);

	for (led = LED_FIRST; led <= LED_LAST; ++led) {
		const struct led_settings *led_settings = &pattern->led_settings[led];
		u8 value0 = CALC_VALUE(led_settings->values[0]);
		u8 value1 = CALC_VALUE(led_settings->values[1]);
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

	if (!IS_OFF(pattern)) {
		pr_err("Restore pattern called with %s (!= PATTERN_TYPE_ALL_OFF)\n", pattern_str[pattern->type]);
		return;
	}

	if (!IS_ACTIVE(input)) {
		/* nothing to do as another pattern is active */
		return;
	}

	/* if there is a saved_pattern try to restore that */
	for (a = INPUT_FIRST; a <= INPUT_LAST; ++a) {
		const struct pattern *p = bd2802_led->saved_patterns[a];
		if (a != input && !IS_OFF(p)) {
			restore_pattern = p;
			restore_input = a;
			pr_info("Restoring pattern %s from input %s "
				"instead of setting %s from %s\n",
				pattern_str[restore_pattern->type], input_str[restore_input],
				pattern_str[pattern->type], input_str[input]);
			break;
		}
	}

	if (IS_OFF(restore_pattern)) {
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

	down_write(&bd2802_led->rwsem);

	input = bd2802_led->active_input;
	pattern = bd2802_led->saved_patterns[input];
	pattern->func(bd2802_led, pattern, input);

	up_write(&bd2802_led->rwsem);
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
	pr_info("Setting pattern %s from input %s\n",
		pattern_str[pattern->type], input_str[input]);

	down_write(&bd2802_led->rwsem);

	bd2802_led->saved_patterns[input] = pattern;
	pattern->func(bd2802_led, pattern, input);

	up_write(&bd2802_led->rwsem);
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

void touchkey_pressed(enum LED led)
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

static ssize_t onoff_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct bd2802_led *bd2802_led = i2c_get_clientdata(to_i2c_client(dev));
	return snprintf(buf, PAGE_SIZE, "%s\n", onoff_str[bd2802_led->onoff]);
}

struct onoff_cb {
	const char *input;
	void (*on_off)(struct bd2802_led *);
};

struct onoff_cb onoff_cb[] = {
	{"off", bd2802_off},
	{"on", bd2802_on},
	{"reset", bd2802_reset},
	{"0", bd2802_off},
	{"1", bd2802_on},
};

static ssize_t onoff_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct bd2802_led *bd2802_led = i2c_get_clientdata(to_i2c_client(dev));
	int i;

	for (i = 0; i < ARRAY_SIZE(onoff_cb); ++i) {
		if (strstarts(buf, onoff_cb[i].input)) {
			onoff_cb[i].on_off(bd2802_led);
			return count;
		}
	}

	pr_err("Value %s not supported\n", buf);

	return -EINVAL;
}

static ssize_t button_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE,
		"Write 1 to this file and all the buttons illuminate, "
		"write 0 to cancel it\n");
}

static ssize_t button_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct bd2802_led *bd2802_led = i2c_get_clientdata(bd2802_i2c_client);
	u8 val = simple_strtoul(buf, NULL, 10);
	if (val == 0) {
		bd2802_set_pattern(bd2802_led, &all_off, INPUT_BUTTON);
	} else if (val == 1) {
		bd2802_set_pattern(bd2802_led, &all_on, INPUT_BUTTON);
	} else {
		return -EINVAL;
	}
	return count;
}

static ssize_t pattern_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE,
		"Write integers in the following format:\n\n"
		"cycle_length slope_up slope_down - (brightness0 brightness1 wave_pattern,)\n"
		"{for each of the 6 leds: MENU, HOME, BACK, SEARCH, BLUELEFT, BLUERIGHT}\n\n"
		"cycle_length: 0-15 (representing cycle lengths in the range from 131 ms to 16.8 s (*))\n"
		"slope_up, slope_down: 0-3 (none, 16th, 8th, 4th of the cycle length)\n"
		"brightness{0,1}: 0-127 (multiples of pattern_brightness / 127)\n" /* U7_MAX */
		"wave_pattern: 0-15 (*)\n" /* WAVE_FIRST to WAVE_LAST */
		"(*) Have a look at the source code or datasheet for details\n"
		"To disable the pattern write a pattern with all brightness levels set to 0\n\n"
		"Examples:\n"
		"echo \"3 3 3 - 0 127 12, 0 127 13, 0 127 14, 0 127 3, 127 0 7, 127 0 7\" > pattern\n"
		"echo \"0 0 0 - 0 0 0, 0 0 0, 0 0 0, 0 0 0, 0 0 0, 0 0 0\" > pattern\n");
}

static ssize_t pattern_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct bd2802_led *bd2802_led = i2c_get_clientdata(to_i2c_client(dev));
	struct pattern pattern = {
		.func = bd2802_write_pattern,
		.type = PATTERN_CUSTOM,
	};
	int read = 0;
	unsigned int cycle_length;
	unsigned int slope_up;
	unsigned int slope_down;
	enum LED led;
	bool is_off = true;

	sscanf(buf, "%u %u %u -%n", &cycle_length, &slope_up, &slope_down, &read);
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
		unsigned int value0, value1, wave;
		int read_items = sscanf(buf + read, " %u %u %u,%n", &value0, &value1, &wave, &read2);
		if (read_items < 3 || read2 == 0) {
			return -EINVAL;
		}
		if (value0 > U7_MAX || value1 > U7_MAX) {
			pr_err("Brightness for key %s too high\n", led_str[led]);
			return -EINVAL;
		}
		if (wave > WAVE_LAST) {
			pr_err("Wave value too high (%u > %u)\n", wave, WAVE_LAST);
			return -EINVAL;
		}

		led_settings->values[0].value = value0;
		led_settings->values[1].value = value1;
		led_settings->wave = wave;
		if (value0 > 0 || value1 > 0) {
			is_off = false;
		}
		read += read2;
	}

	if (is_off) {
		bd2802_set_pattern(bd2802_led, &all_off, INPUT_PATTERN);
	} else {
		/* store the pattern permanently and pass the stored pattern */
		down_write(&bd2802_led->rwsem);
		bd2802_led->custom_pattern = pattern;
		up_write(&bd2802_led->rwsem);
		bd2802_set_pattern(bd2802_led, &bd2802_led->custom_pattern, INPUT_PATTERN);
	}

	return count;
}

static DEVICE_ATTR(onoff, 0644, onoff_show, onoff_store);
static DEVICE_ATTR(pattern, 0644, pattern_show, pattern_store);
static DEVICE_ATTR(button, 0644, button_show, button_store);

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

BD2802_SET_REGISTER(0x00); /* CLKSETUP */
BD2802_SET_REGISTER(0x01); /* LED_DRIVERCONTROL */
BD2802_SET_REGISTER(0x02); /* RGB1_HOURSETUP */
BD2802_SET_REGISTER(0x03); /* R1_CURRENT1 */
BD2802_SET_REGISTER(0x04); /* R1_CURRENT2 */
BD2802_SET_REGISTER(0x05); /* R1_PATTERN */
BD2802_SET_REGISTER(0x06); /* G1_CURRENT1 */
BD2802_SET_REGISTER(0x07); /* G1_CURRENT2 */
BD2802_SET_REGISTER(0x08); /* G1_PATTERN */
BD2802_SET_REGISTER(0x09); /* B1_CURRENT1 */
BD2802_SET_REGISTER(0x0a); /* B1_CURRENT2 */
BD2802_SET_REGISTER(0x0b); /* B1_PATTERN */
BD2802_SET_REGISTER(0x0c); /* RGB2_HOURSETUP */
BD2802_SET_REGISTER(0x0d); /* R2_CURRENT1 */
BD2802_SET_REGISTER(0x0e); /* R2_CURRENT2 */
BD2802_SET_REGISTER(0x0f); /* R2_PATTERN */
BD2802_SET_REGISTER(0x10); /* G2_CURRENT1 */
BD2802_SET_REGISTER(0x11); /* G2_CURRENT2 */
BD2802_SET_REGISTER(0x12); /* G2_PATTERN */
BD2802_SET_REGISTER(0x13); /* B2_CURRENT1 */
BD2802_SET_REGISTER(0x14); /* B2_CURRENT2 */
BD2802_SET_REGISTER(0x15); /* B2_PATTERN */

static const struct device_attribute *bd2802_attributes[] = {
	&dev_attr_pattern,
	&dev_attr_onoff,
	&dev_attr_button,
	&dev_attr_pattern_brightness.attr,
	&dev_attr_button_brightness.attr,
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
}

static void bd2802_on_suspend(struct bd2802_led *bd2802_led)
{
	/* set touchkey input immediately off */
	schedule_delayed_work(&bd2802_led->touchkey_delayed_off_work, 0);
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

	bd2802_led = kzalloc(sizeof(struct bd2802_led), GFP_KERNEL);
	if (!bd2802_led) {
		dev_err(&client->dev, "failed to allocate driver data\n");
		return -ENOMEM;
	}

	bd2802_led->client = client;
	i2c_set_clientdata(client, bd2802_led);

	mutex_lock(&mutex);
	bd2802_i2c_client = bd2802_led->client;
	mutex_unlock(&mutex);

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

	bd2802_reset(bd2802_led);
	bd2802_write_pattern(bd2802_led, &all_blinking, INPUT_TOUCHKEY);

	return 0;

failed_unregister_dev_file:
	pr_info("Unregistering dev files\n");
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
