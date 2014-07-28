/*
 * drivers/video/backlight/aat2870_bl.c
 *
 * Copyright (C) 2010 LGE, Inc
 * Copyright (C) 2014 Stefan Demharter <stefan.demharter@gmx.net>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * Datasheet can be found at:
 * http://www1.futureelectronics.com/doc/ANALOGICTECH%20-%20AATI/AAT2870IUW-DB1.pdf
 */

#include "aat2870_bl.h"
#include "aat2870_bl_als.h"

#include <linux/util/fade.h>
#include <linux/util/ld.h>
#include <linux/util/sysfs.h>

#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/backlight.h>
#include <linux/fb.h>
#include <linux/i2c.h>
#include <linux/string.h>
#include <asm/system.h>
#include <mach/hardware.h>
#include <mach/gpio.h>
#include <linux/leds.h>
#include <plat/board.h>
#include <linux/notifier.h>

#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif

/* dev_* macros which use the device in the local adev var */
#define dbg(...) dev_dbg(&adev->client->dev, __VA_ARGS__)
#define info(...) dev_info(&adev->client->dev, __VA_ARGS__)
#define warn(...) dev_warn(&adev->client->dev, __VA_ARGS__)
#define err(...) dev_err(&adev->client->dev, __VA_ARGS__)

/* i2c_* macros which use the device of the locale i2c client var */
#define i2c_dbg(...) dev_dbg(&client->dev, __VA_ARGS__)
#define i2c_err(...) dev_err(&client->dev, __VA_ARGS__)

/* dev_* macros which use the device in the global aat2870_i2c_client var */
#define global_dbg(...) dev_dbg(&aat2870_i2c_client->dev, __VA_ARGS__)
#define global_info(...) dev_info(&aat2870_i2c_client->dev, __VA_ARGS__)
#define global_err(...) dev_err(&aat2870_i2c_client->dev, __VA_ARGS__)

#define for_array(_idx, _array) for (_idx = 0; _idx < ARRAY_SIZE(_array); ++_idx)
#define for_array_rev(_idx, _array) for (_idx = ARRAY_SIZE(_array); _idx-- != 0;)

struct i2c_client *aat2870_i2c_client;
EXPORT_SYMBOL(aat2870_i2c_client);

#define BRIGHTNESS_MAX 		0x7F /* the register value of the maximum brightness */
#define BRIGHTNESS_MAX_STR	"127"
#define BRIGHTNESS_DEFAULT 	0x3F
#define ALS_LEVEL_MAX_STR	"15"

#define ALS_IVAL_MAX_OFF ALS_IVAL_MAX
#define ALS_IVAL_MAX_SENSOR 5000
#define ALS_IVAL_MAX_USER ALS_IVAL_MAX

#define ALL_CH_ON	0xFF
#define ALL_CH_OFF	0x00

#define LDO_EN_ALL	0x0F
#define LDO_DIS_ALL	0x00
#define LDO_3V_1_8V	0x4C

#define ENUM_END(_enum) (_enum ## _MAX + 1)

enum BRIGHTNESS_MODE {
	BRIGHTNESS_MODE_USER,
	BRIGHTNESS_MODE_SENSOR,

	BRIGHTNESS_MODE_MIN = BRIGHTNESS_MODE_USER,
	BRIGHTNESS_MODE_MAX = BRIGHTNESS_MODE_SENSOR,
};

static const char *BRIGHTNESS_MODE_long_str[] = {
	[BRIGHTNESS_MODE_USER] = "user",
	[BRIGHTNESS_MODE_SENSOR] = "sensor",
};

static const char *BRIGHTNESS_MODE_str[] = {
	[BRIGHTNESS_MODE_USER] = "0",
	[BRIGHTNESS_MODE_SENSOR] = "1",
};

enum BL_STATE {
	BL_STATE_OFF,
	BL_STATE_ON,
};

struct aat2870_device {
	struct i2c_client *client;
	struct backlight_device *bl_dev;
	struct led_classdev led;

	struct fade_props fade_props;
	struct als_props als_props;

	struct fade fade;
	struct als als;

	struct mutex mutex;

	enum BL_STATE bl_state;

	enum BRIGHTNESS_MODE brightness_mode;
	u8 brightness;
	size_t sensor_poll_ival_ms[ENUM_END(BRIGHTNESS_MODE)];
	size_t sensor_poll_ival_screen_off_ms;
	/* the 16 brightness levels for the 16 als levels (0-127) */
	int brightness_levels[BRIGHTNESS_REGS];

	/* delay to adapt lcd brightness after a change in ambient brightness */
	size_t adapt_brightness_delay_ms;

	/*
	 * notifier block for als level updates, i.e. this one is responsible
	 * for altering the brightness upon a change in ambient brightness
	 */
	struct notifier_block set_brightness_listener;

#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
#endif
};

static size_t default_sensor_poll_ival_ms[] = {
	[BRIGHTNESS_MODE_SENSOR] = 500,
	[BRIGHTNESS_MODE_USER] = 10000,
};

static const struct i2c_device_id aat2870_bl_id[] = {
	{AAT2870_I2C_BL_NAME, 0},
	{},
};

static void set_brightness_mode(struct i2c_client *client, enum BRIGHTNESS_MODE mode);

int i2c_read_reg(struct i2c_client *client,
		unsigned char reg, unsigned char *val)
{
	int ret = i2c_smbus_read_byte_data(client, reg);

	if (ret < 0) {
		i2c_err("Failed to read reg = 0x%x\n", (int)reg);
		return -EIO;
	}

	*val = ret;

	return 0;
}

int i2c_write_reg(struct i2c_client *client,
		enum REG reg, unsigned char val)
{
	int status = 0;
	int ret = i2c_smbus_write_byte_data(client, reg, val);

	if (ret != 0) {
		i2c_err("Failed to write (reg = 0x%02x, val = 0x%02x)\n", (int)reg, (int)val);
		return -EIO;
	}

	i2c_dbg("Written reg = 0x%02x, val = 0x%02x\n", (int)reg, (int)val);

	return status;
}

/*
 * Set the brightness by setting all of the 16 als registers
 * REG_ALS0 to REG_ALS15 to the same value
 */
static void i2c_set_brightness_to(struct i2c_client *client, u8 brightness)
{
	enum REG reg;

	for (reg = REG_ALS0; reg <= REG_ALS15; ++reg) {
		int ret = i2c_smbus_write_byte_data(client, reg, brightness);

		if (ret < 0) {
			i2c_err("Failed to write brightness to reg %d\n", reg);
			break;
		}
	}
}

/*
 * If a fading is disabled, set the brightness (delayed) to the given value
 * and stop any running fading, otherwise start a (delayed) fade to the new brightness
 */
static void set_brightness_to(struct aat2870_device *adev, u8 brightness)
{
	int delay_ms = adev->brightness_mode == BRIGHTNESS_MODE_SENSOR ?
		adev->adapt_brightness_delay_ms : 0;

	fade_brightness_delayed(&adev->fade, delay_ms, adev->brightness, brightness);
}

static void set_user_brightness_to(struct aat2870_device *adev, u8 brightness)
{
	if (adev->brightness_mode == BRIGHTNESS_MODE_USER)
		set_brightness_to(adev, brightness);
	else
		info("Skipping brightness request to %d as brightness-mode is set to "
			"`%s` instead of `%s`\n",
			(int)brightness, BRIGHTNESS_MODE_long_str[adev->brightness_mode],
			BRIGHTNESS_MODE_long_str[BRIGHTNESS_MODE_USER]);
}

void aat2870_ldo_enable(unsigned char num, int enable)
{
	u8 org = 0;
	int ret = i2c_read_reg(aat2870_i2c_client, REG_EN_LDO, &org);

	if (ret < 0)
		return;

	i2c_write_reg(aat2870_i2c_client, REG_EN_LDO,
		enable ?  (org | (1 << num)) : (org & ~(1 << num)));
}
EXPORT_SYMBOL(aat2870_ldo_enable);

u8 aat2870_touch_ldo_read(u8 reg)
{
	u8 val = 0;
	i2c_read_reg(aat2870_i2c_client, reg, &val);

	return val;
}

int aat2870_touch_ldo_write(u8 reg , u8 val)
{
	return i2c_write_reg(aat2870_i2c_client, reg, val);
}

/* backlight on */
static void bl_on(struct i2c_client *client)
{
	struct aat2870_device *adev = i2c_get_clientdata(client);
	struct als *als = &adev->als;

	if (adev->bl_state == BL_STATE_ON)
		return;

	if (adev->bl_state != BL_STATE_ON) {
		adev->bl_state = BL_STATE_ON;
		i2c_set_brightness_to(client, adev->brightness);
		i2c_write_reg(client, REG_EN_CH, ALL_CH_ON);

		if (adev->brightness_mode == BRIGHTNESS_MODE_SENSOR)
			als_add_listener(als, &adev->set_brightness_listener);

		als_set_poll_ival(als, adev->sensor_poll_ival_ms[adev->brightness_mode]);
	}

	return;
}

/* backlight off */
static void bl_off(struct i2c_client *client)
{
	struct aat2870_device *adev = i2c_get_clientdata(client);
	struct als *als = &adev->als;

	if (adev->bl_state == BL_STATE_OFF)
		return;

	if (adev->bl_state != BL_STATE_OFF) {
		adev->bl_state = BL_STATE_OFF;
		i2c_write_reg(client, REG_EN_CH, ALL_CH_OFF);

		if (adev->brightness_mode == BRIGHTNESS_MODE_SENSOR)
			als_remove_listener(als, &adev->set_brightness_listener);

		als_set_poll_ival(als, adev->sensor_poll_ival_screen_off_ms);

		if (adev->fade.state != FADE_STATE_STOPPED) {
			fade_stop(&adev->fade);
			adev->brightness = adev->fade.brightness_next;
		}
	}
}

#if defined(CONFIG_MACH_LGE_HUB) || defined(CONFIG_MACH_LGE_SNIPER)
/*
 * Enable LDO outputs and set the voltage to the default levels, i.e.
 * 1.8 V and 3V
 */
static void ldo_activate(struct i2c_client *client)
{
	i2c_dbg("ldo enable..\n");
	i2c_write_reg(client, REG_LDOAB, LDO_3V_1_8V);
	i2c_write_reg(client, REG_LDOCD, LDO_3V_1_8V);
	i2c_write_reg(client, REG_EN_LDO, LDO_EN_ALL);
}

int check_bl_shutdown = 0;
EXPORT_SYMBOL(check_bl_shutdown);

void hub_lcd_initialize(void)
{
	struct aat2870_device *adev = i2c_get_clientdata(aat2870_i2c_client);

	als_stay_off(&adev->als, false);

	mutex_lock(&adev->mutex);

	gpio_set_value(LCD_CP_EN, 1);
	ldo_activate(adev->client);
	bl_on(adev->client);

	mutex_unlock(&adev->mutex);
}
EXPORT_SYMBOL(hub_lcd_initialize);

void aat2870_shutdown(void)
{
	struct aat2870_device *adev = i2c_get_clientdata(aat2870_i2c_client);

	als_stay_off(&adev->als, true);

	mutex_lock(&adev->mutex);

	bl_off(adev->client);
	i2c_write_reg(aat2870_i2c_client, REG_EN_LDO, LDO_DIS_ALL);
	gpio_set_value(LCD_CP_EN, 0);
	check_bl_shutdown = 1;

	mutex_unlock(&adev->mutex);
}
EXPORT_SYMBOL(aat2870_shutdown);
#endif

#define CONVERT(_val, _from, _to) ((_to + 1) * _val / (_from + 1))

static void aat2870_led_brightness_set(struct led_classdev *led_cdev,
		enum led_brightness value)
{
	struct aat2870_device *adev = dev_get_drvdata(led_cdev->dev->parent);

	dbg("%s\n", __func__);
	set_user_brightness_to(adev, CONVERT(value, LED_FULL, BRIGHTNESS_MAX));

	return;
}

static enum led_brightness aat2870_led_brightness_get(struct led_classdev *led_cdev)
{
	struct aat2870_device *adev = dev_get_drvdata(led_cdev->dev->parent);
	return CONVERT(adev->brightness, BRIGHTNESS_MAX, LED_FULL);
}

static void set_brightness_to_for_fade(struct fade_props *fade_props,
		uint32_t brightness)
{
	struct aat2870_device *adev =
		container_of(fade_props, struct aat2870_device, fade_props);

	mutex_lock(&adev->mutex);

	adev->brightness = brightness;
	i2c_set_brightness_to(adev->client, brightness);

	mutex_unlock(&adev->mutex);
}

static int bl_get_intensity(struct backlight_device *bd)
{
	struct i2c_client *client = to_i2c_client(bd->dev.parent);
	struct aat2870_device *adev = i2c_get_clientdata(client);

	return adev->brightness;
}

static int bl_set_intensity(struct backlight_device *bd)
{
	struct i2c_client *client = to_i2c_client(bd->dev.parent);
	struct aat2870_device *adev = i2c_get_clientdata(client);

	set_user_brightness_to(adev, bd->props.brightness);

	return 0;
}

/*
 * Just set the mode, restart the als polling (with the updated polling interval)
 * and if brightness is sensor-controlled, add the notifier to adjust the brightness
 * if the als_level was updated
 */
static void set_brightness_mode(struct i2c_client *client, enum BRIGHTNESS_MODE mode)
{
	struct aat2870_device *adev = i2c_get_clientdata(client);
	struct als *als = &adev->als;
	bool mode_change = adev->brightness_mode != mode;

	if (adev->bl_state == BL_STATE_OFF) {
		info("Setting brightness mode to %s, but it won't come into effect until screen is on\n",
			BRIGHTNESS_MODE_long_str[mode]);
		adev->brightness_mode = mode;
		return;
	}

	info("Setting brightness mode to %s\n", BRIGHTNESS_MODE_long_str[mode]);

	if (mode_change && adev->brightness_mode == BRIGHTNESS_MODE_SENSOR)
		als_remove_listener(als, &adev->set_brightness_listener);

	adev->brightness_mode = mode;

	if (mode_change && mode == BRIGHTNESS_MODE_SENSOR)
		als_add_listener(als, &adev->set_brightness_listener);

	als_set_poll_ival(als, adev->sensor_poll_ival_ms[mode]);

	return;
}

ssize_t onoff_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", gpio_get_value(LCD_CP_EN));
}

ssize_t onoff_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct aat2870_device *adev = dev_get_drvdata(dev);
	ssize_t ret = count;

	mutex_lock(&adev->mutex);

	if (sysfs_streq(buf, "0") || sysfs_streq(buf, "off")) {
		bl_off(adev->client);
		i2c_write_reg(adev->client, REG_EN_LDO, LDO_DIS_ALL);
		gpio_set_value(LCD_CP_EN, 0);
		info("onoff: off\n");
	} else if (sysfs_streq(buf, "1") || sysfs_streq(buf, "on")) {
		gpio_set_value(LCD_CP_EN, 1);
		ldo_activate(adev->client);
		bl_on(adev->client);
		info("onoff: on\n");
	} else
		ret = -EINVAL;

	mutex_unlock(&adev->mutex);

	return ret;
}

static void *val_from_i2c_dev_with_offset(struct kobject *kobj, struct val_attr *attr)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	char *adev = dev_get_drvdata(dev);

	return adev + attr->data;
}

static int update_brightness_mode(struct kobj_props *p, int new_val)
{
	struct device *dev = container_of(p->kobj, struct device, kobj);
	struct aat2870_device *adev = dev_get_drvdata(dev);

	REQUIRE_ATTR_TYPE(ATTR_TYPE_ENUM);

	mutex_lock(&adev->mutex);
	set_brightness_mode(adev->client, new_val);
	mutex_unlock(&adev->mutex);

	return 0;
}

static int update_poll_ival(struct kobj_props *p, int new_val)
{
	struct device *dev = container_of(p->kobj, struct device, kobj);
	struct aat2870_device *adev = dev_get_drvdata(dev);

	REQUIRE_ATTR_TYPE(ATTR_TYPE_INT);

	mutex_lock(&adev->mutex);

	*(int *)p->val = new_val;
	als_set_poll_ival(&adev->als, adev->sensor_poll_ival_ms[adev->brightness_mode]);

	mutex_unlock(&adev->mutex);

	return 0;
}

static int update_brightness_levels(struct kobj_props *p, int *new_vals, size_t size)
{
	struct device *dev = container_of(p->kobj, struct device, kobj);
	struct aat2870_device *adev = dev_get_drvdata(dev);
	int ret = 0;
	int i;

	REQUIRE_ATTR_TYPE(ATTR_TYPE_INT_ARRAY);

	for (i = 0; i < size; ++i) {
		if (new_vals[i] < 0 || new_vals[i] > BRIGHTNESS_MAX) {
			err("%d not in range [0, %d]", new_vals[i], BRIGHTNESS_MAX);
			return -EINVAL;
		}
	}

	mutex_lock(&adev->mutex);

	if (size == 1 || size == 2) {
		int min = new_vals[0];
		int max = size == 1 ? BRIGHTNESS_MAX : new_vals[1];

		if (min > max) {
			err("min brightness level %d > max brightness level %d\n", min, max);
			ret = -EINVAL;
		} else {
			util_fill_exp(p->val, BRIGHTNESS_REGS, adev->fade.ld_offset, min, max);
		}
	} else if (size == BRIGHTNESS_REGS) {
		int j;

		for (j = 0; j < BRIGHTNESS_REGS; ++j) {
			adev->brightness_levels[j] = new_vals[j];
		}
	} else {
		ret = -EINVAL;
	}

	mutex_unlock(&adev->mutex);

	return ret;
}

#define I2C_OFFSET(_val) \
	val_from_i2c_dev_with_offset, offsetof(struct aat2870_device, _val)
#define SIZE(_val) \
	ARRAY_SIZE(((struct aat2870_device *)0)->_val)

static DEV_INT_ATTR(adapt_brightness_delay, 0644, 0, 10000,
	I2C_OFFSET(adapt_brightness_delay_ms), NULL);
static DEV_INT_MIN_MAX_ATTR(adapt_brightness_delay, 0444);

static DEV_INT_ARRAY_ATTR(brightness_levels, 0644,
	I2C_OFFSET(brightness_levels), SIZE(brightness_levels),
	update_brightness_levels);

static DEV_INFO_ATTR(info, 0444,
	"Here you can control the reaction of the backlight to ambient brightness changes:\n"
	"\n"
	"In brightness_mode you can set the mode to auto to enable auto adjustments to the\n"
	"brightness upon changes in the ambient brightness\n"
	"\n"
	"Via adapt_brightness_delay you can set the delay in ms before\n"
	"adapting the lcd brightnesss to a change in ambient brightness.\n"
	"\n"
	"Via brightness_levels you can set the 16 backlight brightness levels\n"
	"corresponding to the 16 ambient brightness levels the sensor can yield.\n"
	"You can also provide just the minimum and maximum brightness level.\n"
	"The intermediate brightness levels are then calculated automatically in\n"
	"an exponential way to match the logarithmic response of the human eye.\n"
	"The formula depends on the value of fade/ld_offset.\n"
	"You can also omit the maximum level in which case " BRIGHTNESS_MAX_STR " is used\n"
	"All brightness levels have to be in the interval [0, " BRIGHTNESS_MAX_STR "]\n"
	"\n"
	"Fading and the sensor can be controlled in their sub directories.");

static DEV_INT_ATTR(sensor_poll_ival_screen_off, 0644,
	ALS_IVAL_MIN, ALS_IVAL_MAX_OFF,
	I2C_OFFSET(sensor_poll_ival_screen_off_ms), update_poll_ival);
static DEV_INT_MIN_MAX_ATTR(sensor_poll_ival_screen_off, 0444);

static DEV_INT_ATTR(sensor_poll_ival_sensor, 0644,
	ALS_IVAL_MIN, ALS_IVAL_MAX_SENSOR,
	I2C_OFFSET(sensor_poll_ival_ms[BRIGHTNESS_MODE_SENSOR]), update_poll_ival);
static DEV_INT_MIN_MAX_ATTR(sensor_poll_ival_sensor, 0444);

static DEV_INT_ATTR(sensor_poll_ival_user, 0644,
	ALS_IVAL_MIN, ALS_IVAL_MAX_USER,
	I2C_OFFSET(sensor_poll_ival_ms[BRIGHTNESS_MODE_USER]), update_poll_ival);
static DEV_INT_MIN_MAX_ATTR(sensor_poll_ival_user, 0444);

static DEV_ENUM_LONG_ATTR(brightness_mode, 0644, BRIGHTNESS_MODE,
	I2C_OFFSET(brightness_mode), update_brightness_mode);

static DEVICE_ATTR(onoff, 0644, onoff_show, onoff_store);

static struct device_attribute *attrs[] = {
	&dev_val_attr_adapt_brightness_delay.dev_attr,
	&dev_val_attr_adapt_brightness_delay_min_max.dev_attr,
	&dev_val_attr_brightness_levels.dev_attr,
	&dev_val_attr_info.dev_attr,
	&dev_val_attr_sensor_poll_ival_screen_off.dev_attr,
	&dev_val_attr_sensor_poll_ival_screen_off_min_max.dev_attr,
	&dev_val_attr_sensor_poll_ival_sensor.dev_attr,
	&dev_val_attr_sensor_poll_ival_sensor_min_max.dev_attr,
	&dev_val_attr_sensor_poll_ival_user.dev_attr,
	&dev_val_attr_sensor_poll_ival_user_min_max.dev_attr,
	&dev_val_attr_brightness_mode.dev_attr,
	&dev_attr_onoff,
};

#ifdef CONFIG_HAS_EARLYSUSPEND
void aat2870_resume_for_lcd(void)
{
}
EXPORT_SYMBOL(aat2870_resume_for_lcd);

/*
 * No need to do anything here as all is done in
 * hub_lcd_initialize/aat2870_shutdown.
 * And doing the stuff here makes it even worse.
 */
static void aat2870_early_suspend(struct early_suspend *h)
{
}

static void aat2870_late_resume(struct early_suspend *h)
{
}

#else /* CONFIG_HAS_EARLYSUSPEND */

static int aat2870_suspend(struct i2c_client *client, pm_message_t state)
{
	return 0;
}

static int aat2870_resume(struct i2c_client *client)
{
	return 0;
}
#endif /* CONFIG_HAS_EARLYSUSPEND */

static struct backlight_ops bl_ops = {
	.update_status	= bl_set_intensity,
	.get_brightness	= bl_get_intensity,
};

static struct backlight_properties bl_props = {
	.max_brightness = BRIGHTNESS_MAX,
	.brightness = BRIGHTNESS_DEFAULT,
	.power = FB_BLANK_UNBLANK,
	.type = BACKLIGHT_RAW,
};

/*
 * The set brightness notifier, to be called from store_als_stage2
 * This method starts the brightness adjustments in sensor-brightness mode
 */
static int set_brightness_listener(struct notifier_block *nb,
		unsigned long als_level, void *data)
{
	struct aat2870_device *adev =
		container_of(nb, struct aat2870_device, set_brightness_listener);
	u8 b_new;

	if (als_level > ARRAY_SIZE(adev->brightness_levels)) {
		err("als_level %lu too high (> %zu)", als_level,
			ARRAY_SIZE(adev->brightness_levels));
	} else {
		b_new = adev->brightness_levels[als_level];
		set_brightness_to(adev, b_new);
	}

	return NOTIFY_OK;
}

static void on_fade_release(struct fade_props* fade_props)
{
	struct aat2870_device *adev =
		container_of(fade_props, struct aat2870_device, fade_props);

	aat2870_i2c_client = NULL;
	gpio_free(LCD_CP_EN);
	gpio_free(HUB_PANEL_LCD_RESET_N);
	kfree(adev);
}

static void on_als_release(struct als_props* als_props)
{
	struct aat2870_device *adev =
		container_of(als_props, struct aat2870_device, als_props);

	kobject_put(&adev->fade.kobj);
}

static int aat2870_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct aat2870_device *adev;
	size_t i;

	dev_info(&client->dev, "%s\n", __func__);

	aat2870_i2c_client = client;

	adev = kzalloc(sizeof(struct aat2870_device), GFP_KERNEL);

	if (adev == NULL) {
		dev_err(&client->dev, "Couldn't alloc\n");
		return -ENOMEM;
	}

	mutex_init(&adev->mutex);

	adev->bl_dev = backlight_device_register(AAT2870_I2C_BL_NAME,
		&client->dev, NULL, &bl_ops, &bl_props);

	adev->client = client;
	i2c_set_clientdata(client, adev);

	adev->bl_state = BL_STATE_ON;
	adev->brightness_mode = BRIGHTNESS_MODE_USER;

	if (gpio_request(LCD_CP_EN, "lcdcs") < 0) {
		dev_err(&client->dev, "gpio_request lcdcs failed\n");
		goto cleanup;
	}

	gpio_direction_output(LCD_CP_EN, 1);

	if (gpio_request(HUB_PANEL_LCD_RESET_N, "lcd reset") < 0) {
		dev_err(&client->dev, "gpio_request lcd recest failed\n");
		goto cleanup;
	}

	gpio_direction_output(HUB_PANEL_LCD_RESET_N, 1);
#if defined(CONFIG_MACH_LGE_HUB) || defined(CONFIG_MACH_LGE_SNIPER)
	ldo_activate(client);
#endif

	for_array (i, attrs) {
		int err = device_create_file(&client->dev, attrs[i]);
		if (err) {
			err("Couldn't create sysfs file\n");
		}
	}

	adev->led = (struct led_classdev) {
		.name = "lcd-backlight",
		.brightness = LED_HALF,
		.max_brightness = LED_FULL,
		.brightness_set = aat2870_led_brightness_set,
		.brightness_get = aat2870_led_brightness_get,
	};

	led_classdev_register(&client->dev, &adev->led);

#ifdef CONFIG_HAS_EARLYSUSPEND
	adev->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN;
	adev->early_suspend.suspend = aat2870_early_suspend;
	adev->early_suspend.resume = aat2870_late_resume;
	register_early_suspend(&adev->early_suspend);
#endif

	memcpy(adev->sensor_poll_ival_ms, default_sensor_poll_ival_ms,
		sizeof(adev->sensor_poll_ival_ms));

	adev->sensor_poll_ival_ms[BRIGHTNESS_MODE_SENSOR] = 500;
	adev->sensor_poll_ival_ms[BRIGHTNESS_MODE_USER] = 5000;
	adev->sensor_poll_ival_screen_off_ms = 10000;

	adev->fade_props = (struct fade_props) {
		.parent = &client->dev,
		.set_brightness_to = set_brightness_to_for_fade,
		.on_release = on_fade_release,
	};
	fade_init(&adev->fade, &adev->fade_props);

	adev->als_props = (struct als_props) {
		.client = client,
		.on_release = on_als_release,
	};
	als_init(&adev->als, &adev->als_props);

	adev->set_brightness_listener.notifier_call = set_brightness_listener;
	adev->adapt_brightness_delay_ms = 600;

	util_fill_exp(adev->brightness_levels, BRIGHTNESS_REGS,
		adev->fade.ld_offset, 8, BRIGHTNESS_MAX);

	sysfs_create_link(&adev->bl_dev->dev.kobj, &adev->als.kobj, "sensor");
	sysfs_create_link(&adev->bl_dev->dev.kobj, &adev->fade.kobj, "fade");
	sysfs_create_link(&adev->led.dev->kobj, &adev->als.kobj, "sensor");
	sysfs_create_link(&adev->led.dev->kobj, &adev->fade.kobj, "fade");

	gpio_request(LCD_CP_EN, "LCD_CP_EN");
	set_brightness_mode(adev->client, BRIGHTNESS_MODE_USER);

	return 0;

cleanup:
	kfree(adev);
	return -ENOSYS;
}

static int aat2870_remove(struct i2c_client *client)
{
	struct aat2870_device *adev = i2c_get_clientdata(client);
	size_t i;

	sysfs_delete_link(&adev->bl_dev->dev.kobj, &adev->als.kobj, "sensor");
	sysfs_delete_link(&adev->bl_dev->dev.kobj, &adev->fade.kobj, "fade");
	sysfs_delete_link(&adev->led.dev->kobj, &adev->als.kobj, "sensor");
	sysfs_delete_link(&adev->led.dev->kobj, &adev->fade.kobj, "fade");

	unregister_early_suspend(&adev->early_suspend);

	for_array_rev (i, attrs) {
		device_remove_file(&client->dev, attrs[i]);
	}

	backlight_device_unregister(adev->bl_dev);
	led_classdev_unregister(&adev->led);

	kobject_put(&adev->als.kobj);

	return 0;
}

static struct i2c_driver aat2870_driver = {
	.probe		= aat2870_probe,
	.remove		= aat2870_remove,
#if !defined(CONFIG_HAS_EARLYSUSPEND)
	.suspend	= aat2870_suspend,
	.resume		= aat2870_resume,
#endif
	.id_table	= aat2870_bl_id,

	.driver = {
		.name	= AAT2870_I2C_BL_NAME,
		.owner	= THIS_MODULE,
	},
};

static int __init aat2870_init(void)
{
	return i2c_add_driver(&aat2870_driver);
}

static void __exit aat2870_exit(void)
{
	i2c_del_driver(&aat2870_driver);
}

module_init(aat2870_init);
module_exit(aat2870_exit);

int aat2870_als_add_listener(struct notifier_block *listener)
{
	struct kobject *kobj;
	struct aat2870_device *adev;
	int res;

	if (!aat2870_i2c_client)
		return -EINVAL;

	adev = i2c_get_clientdata(aat2870_i2c_client);
	kobj = kobject_get(&adev->als.kobj);
	if (!kobj)
		return -EINVAL;

	res = als_add_listener(&adev->als, listener);
	if (res)
		kobject_put(&adev->als.kobj);

	return res;
}
EXPORT_SYMBOL_GPL(aat2870_als_add_listener);

int aat2870_als_remove_listener(struct notifier_block *listener)
{
	struct aat2870_device *adev = i2c_get_clientdata(aat2870_i2c_client);
	int res = als_remove_listener(&adev->als, listener);

	if (!res)
		kobject_put(&adev->als.kobj);

	return res;
}
EXPORT_SYMBOL_GPL(aat2870_als_remove_listener);

MODULE_DESCRIPTION("AAT2870 Backlight Control");
MODULE_AUTHOR("Stefan Demharter, Yool-Je Cho <yoolje.cho@lge.com>");
MODULE_LICENSE("GPL");
