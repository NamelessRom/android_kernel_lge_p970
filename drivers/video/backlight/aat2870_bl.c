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
#include "aat2870_bl_fade.h"
#include "aat2870_bl_ld.h"
#include "aat2870_bl_util.h"
#include "aat2870_bl_sysfs.h"

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

/* dev_* macros which use the device in the local adev variable */
#define adev_dbg(...) dev_dbg(&adev->client->dev, __VA_ARGS__)
#define adev_info(...) dev_info(&adev->client->dev, __VA_ARGS__)
#define adev_warn(...) dev_warn(&adev->client->dev, __VA_ARGS__)
#define adev_err(...) dev_err(&adev->client->dev, __VA_ARGS__)

/* dev_* macros which use the device in the global aat2870_i2c_client variable */
#define global_dbg(...) dev_dbg(&aat2870_i2c_client->dev, __VA_ARGS__)
#define global_info(...) dev_info(&aat2870_i2c_client->dev, __VA_ARGS__)
#define global_err(...) dev_err(&aat2870_i2c_client->dev, __VA_ARGS__)

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

enum MODE {
	MODE_USER,
	MODE_SENSOR,
	MODE_SCREEN_OFF,

	MODE_MIN = MODE_USER,
	MODE_MAX = MODE_SCREEN_OFF,
};

static const char *MODE_long_str[] = {
	[MODE_USER] = "user",
	[MODE_SENSOR] = "sensor",
	[MODE_SCREEN_OFF] = "screen-off",
};

static const char *MODE_str[] = {
	[MODE_USER] = "0",
	[MODE_SENSOR] = "1",
	[MODE_SCREEN_OFF] = "x",
};

enum STATE {
	SLEEP_STATE,
	WAKE_STATE,
};

struct aat2870_device {
	struct i2c_client *client;
	struct backlight_device *bl_dev;
	struct led_classdev led;

	struct fade fade;
	struct als als;

	struct mutex mutex;

	enum STATE state;

	enum MODE mode;
	enum MODE saved_mode;
	u8 brightness;
	size_t sensor_polling_interval_ms[ENUM_SIZE(MODE)];

	/*
	 * notifier block for als level updates, i.e. this one is responsible
	 * for altering the brightness upon a change in ambient brightness
	 */
	struct notifier_block set_brightness_listener;

#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
#endif
};

static size_t default_sensor_polling_interval_ms[] = {
	[MODE_SENSOR] = 500,
	[MODE_USER] = 10000,
	[MODE_SCREEN_OFF] = 60000,
};

static const struct i2c_device_id aat2870_bl_id[] = {
	{AAT2870_I2C_BL_NAME, 0},
	{},
};

static void set_mode(struct i2c_client *client, enum MODE mode);

int i2c_read_reg(struct i2c_client *client,
		unsigned char reg, unsigned char *val)
{
	int ret;
	struct aat2870_device *adev = i2c_get_clientdata(client);

	ret = i2c_smbus_read_byte_data(client, reg);
	if (ret < 0) {
		adev_err("Failed to read reg = 0x%x\n", (int)reg);
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
	struct aat2870_device *adev = i2c_get_clientdata(client);

	if (ret != 0) {
		adev_err("Failed to write (reg = 0x%02x, val = 0x%02x)\n", (int)reg, (int)val);
		return -EIO;
	}

	adev_dbg("Written reg = 0x%02x, val = 0x%02x\n", (int)reg, (int)val);

	return status;
}

/*
 * Set the brightness by setting all of the 16 als registers
 * REG_ALS0 to REG_ALS15 to the same value
 */
static void i2c_set_brightness(struct i2c_client *client)
{
	struct aat2870_device *adev = i2c_get_clientdata(client);
	enum REG reg;
	u8 val = adev->brightness;
	if (adev->state != WAKE_STATE) {
		return;
	}
	for (reg = REG_ALS0; reg <= REG_ALS15; ++reg) {
		int ret = i2c_smbus_write_byte_data(client, reg, val);
		if (ret < 0) {
			adev_err("Failed to write brightness to reg %d\n", reg);
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
	int delay_ms = adev->mode == MODE_SENSOR ? adev->als.adapt_brightness_delay_ms : 0;

	fade_brightness_delayed(&adev->fade, delay_ms, adev->brightness, brightness);
}

static void set_user_brightness_to(struct aat2870_device *adev, u8 brightness)
{
	if (adev->mode == MODE_USER) {
		set_brightness_to(adev, brightness);
	} else {
		adev_info("Skipping brightness request to %d as brightness-mode is set to "
			"`%s` instead of `%s`\n",
			(int)brightness, MODE_long_str[adev->mode], MODE_long_str[MODE_USER]);
	}
}

void aat2870_ldo_enable(unsigned char num, int enable)
{
	u8 org = 0;

	int ret = i2c_read_reg(aat2870_i2c_client, REG_EN_LDO, &org);
	if (ret < 0) {
		return;
	}

	if (enable) {
		i2c_write_reg(aat2870_i2c_client, REG_EN_LDO, org | (1 << num));
	} else {
		i2c_write_reg(aat2870_i2c_client, REG_EN_LDO, org & ~(1 << num));
	}
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

	if (adev->state == WAKE_STATE)
		return;

	if (adev->state != WAKE_STATE) {
		i2c_set_brightness(client);
		i2c_write_reg(client, REG_EN_CH, ALL_CH_ON);
		adev->state = WAKE_STATE;
		if (adev->mode == MODE_SCREEN_OFF) {
			set_mode(client, adev->saved_mode);
		}
	}

	return;
}

/* backlight off */
static void bl_off(struct i2c_client *client)
{
	struct aat2870_device *adev = i2c_get_clientdata(client);

	if (adev->state == SLEEP_STATE)
		return;

	if (adev->state != SLEEP_STATE) {
		i2c_write_reg(client, REG_EN_CH, ALL_CH_OFF);
		adev->state = SLEEP_STATE;
		adev->saved_mode = adev->mode;
		set_mode(client, MODE_SCREEN_OFF);
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
	global_dbg("ldo enable..\n");
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

	adev_dbg("%s\n", __func__);
	set_user_brightness_to(adev, CONVERT(value, LED_FULL, BRIGHTNESS_MAX));

	return;
}

static enum led_brightness aat2870_led_brightness_get(struct led_classdev *led_cdev)
{
	struct aat2870_device *adev = dev_get_drvdata(led_cdev->dev->parent);
	return CONVERT(adev->brightness, BRIGHTNESS_MAX, LED_FULL);
}

static void set_brightness_to_for_fade(struct fade *fade, uint32_t brightness)
{
	struct aat2870_device *adev =
		container_of(fade, struct aat2870_device, fade);

	mutex_lock(&adev->mutex);
	adev->brightness = brightness;
	i2c_set_brightness(adev->client);
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
static void set_mode(struct i2c_client *client, enum MODE mode)
{
	struct aat2870_device *adev = i2c_get_clientdata(client);
	struct als *als = &adev->als;
	bool mode_change = adev->mode != mode;

	adev_info("Setting brightness mode to %s\n", MODE_long_str[mode]);
	als->polling_interval_ms = adev->sensor_polling_interval_ms[mode];

	if (mode_change && adev->mode == MODE_SENSOR) {
		als_remove_listener(als, &adev->set_brightness_listener);
	}
	adev->mode = mode;
	if (mode_change && mode == MODE_SENSOR) {
		als_add_listener(als, &adev->set_brightness_listener);
	}

	/*
	 * Always issue an initial brightness measure to ensure the registers
	 * are initialized correctly
	 */
	schedule_delayed_work(&adev->als.update_als_level_stage1, 0);

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

	if (sysfs_streq(buf, "0") || sysfs_streq(buf, "off")) {
		mutex_lock(&adev->mutex);
		bl_off(adev->client);
		i2c_write_reg(adev->client, REG_EN_LDO, LDO_DIS_ALL);
		gpio_set_value(LCD_CP_EN, 0);
		adev_info("onoff: off\n");
		mutex_unlock(&adev->mutex);
	} else if (sysfs_streq(buf, "1") || sysfs_streq(buf, "on")) {
		mutex_lock(&adev->mutex);
		gpio_set_value(LCD_CP_EN, 1);
		ldo_activate(adev->client);
		bl_on(adev->client);
		adev_info("onoff: on\n");
		mutex_unlock(&adev->mutex);
	} else
		return -EINVAL;

	return count;
}

static int *int_from_i2c_dev_with_offset(struct kobject *kobj, long *data)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	char *adev = dev_get_drvdata(dev);

	return (int*)(adev + *data);
}

static void update_mode(struct kobject *kobj, int *mode, int new_mode)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct aat2870_device *adev = dev_get_drvdata(dev);

	mutex_lock(&adev->mutex);
	set_mode(adev->client, new_mode);
	mutex_unlock(&adev->mutex);
}

static void update_polling_interval(struct kobject *kobj, int *poll_ival, int new_poll_ival)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct aat2870_device *adev = dev_get_drvdata(dev);

	mutex_lock(&adev->mutex);
	*poll_ival = new_poll_ival;
	adev->als.polling_interval_ms = new_poll_ival;
	mutex_unlock(&adev->mutex);
}

#define I2C_OFFSET(_val) \
	int_from_i2c_dev_with_offset, offsetof(struct aat2870_device, _val)

static DEV_INT_ATTR(sensor_polling_interval_screen_off, 0644,
	ALS_IVAL_MIN, ALS_IVAL_MAX_OFF,
	I2C_OFFSET(sensor_polling_interval_ms[MODE_SCREEN_OFF]), update_polling_interval);
static DEV_INT_ATTR(sensor_polling_interval_sensor, 0644,
	ALS_IVAL_MIN, ALS_IVAL_MAX_SENSOR,
	I2C_OFFSET(sensor_polling_interval_ms[MODE_SENSOR]), update_polling_interval);
static DEV_INT_ATTR(sensor_polling_interval_user, 0644,
	ALS_IVAL_MIN, ALS_IVAL_MAX_USER,
	I2C_OFFSET(sensor_polling_interval_ms[MODE_USER]), update_polling_interval);
static DEV_ENUM_LONG_ATTR(backlight_mode, 0644, MODE,
	I2C_OFFSET(mode), update_mode);
static DEVICE_ATTR(onoff, 0644, onoff_show, onoff_store);

static struct device_attribute *attrs[] = {
	&dev_int_attr_sensor_polling_interval_screen_off.attr.dev_attr,
	&dev_int_attr_sensor_polling_interval_screen_off.min_attr.dev_attr,
	&dev_int_attr_sensor_polling_interval_screen_off.max_attr.dev_attr,
	&dev_int_attr_sensor_polling_interval_sensor.attr.dev_attr,
	&dev_int_attr_sensor_polling_interval_sensor.min_attr.dev_attr,
	&dev_int_attr_sensor_polling_interval_sensor.max_attr.dev_attr,
	&dev_int_attr_sensor_polling_interval_user.attr.dev_attr,
	&dev_int_attr_sensor_polling_interval_user.min_attr.dev_attr,
	&dev_int_attr_sensor_polling_interval_user.max_attr.dev_attr,
	&dev_enum_attr_backlight_mode.attr.dev_attr,
	&dev_attr_onoff,
};

static int aat2870_remove(struct i2c_client *client)
{
	struct aat2870_device *adev = i2c_get_clientdata(client);
	size_t i;

	als_cleanup(&adev->als);
	fade_cleanup(&adev->fade);

	unregister_early_suspend(&adev->early_suspend);

	gpio_free(LCD_CP_EN);

	for_array_rev (i, attrs) {
		device_remove_file(&client->dev, attrs[i]);
	}

	backlight_device_unregister(adev->bl_dev);
	led_classdev_unregister(&adev->led);
	i2c_set_clientdata(client, NULL);

	return 0;
}

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
	struct als *als = &adev->als;
	u8 b_new = als->brightness_levels[als->level];

	set_brightness_to(adev, b_new);
	return NOTIFY_OK;
}

static int aat2870_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct aat2870_device *adev;
	size_t i;

	dev_info(&client->dev, "%s\n", __func__);

	aat2870_i2c_client = client;

	adev = kzalloc(sizeof(struct aat2870_device), GFP_KERNEL);

	if (adev == NULL) {
		dev_err(&client->dev, "fail alloc for aat2870_device\n");
		return 0;
	}

	mutex_init(&adev->mutex);

	adev->bl_dev = backlight_device_register(AAT2870_I2C_BL_NAME,
		&client->dev, NULL, &bl_ops, &bl_props);

	adev->client = client;
	i2c_set_clientdata(client, adev);

	adev->state = WAKE_STATE;
	adev->mode = MODE_USER;

	if (gpio_request(LCD_CP_EN, "lcdcs") < 0) {
		goto cleanup;
	}

	gpio_direction_output(LCD_CP_EN, 1);

	if (gpio_request(HUB_PANEL_LCD_RESET_N, "lcd reset") < 0) {
		goto cleanup;
	}

	gpio_direction_output(HUB_PANEL_LCD_RESET_N, 1);
#if defined(CONFIG_MACH_LGE_HUB) || defined(CONFIG_MACH_LGE_SNIPER)
	ldo_activate(client);
#endif

	for_array (i, attrs) {
		int err = device_create_file(&client->dev, attrs[i]);
		if (err) {
			adev_err("Couldn't create sysfs file\n");
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

	memcpy(adev->sensor_polling_interval_ms, default_sensor_polling_interval_ms,
		sizeof(adev->sensor_polling_interval_ms));

	fade_init(&adev->fade, &client->dev);
	adev->fade.set_brightness_to = set_brightness_to_for_fade;

	als_init(&adev->als, client);
	adev->set_brightness_listener.notifier_call = set_brightness_listener;

	sysfs_create_link(&adev->bl_dev->dev.kobj, &adev->als.kobj, "sensor");
	sysfs_create_link(&adev->bl_dev->dev.kobj, &adev->fade.kobj, "fade");
	sysfs_create_link(&adev->led.dev->kobj, &adev->als.kobj, "sensor");
	sysfs_create_link(&adev->led.dev->kobj, &adev->fade.kobj, "fade");

	gpio_request(LCD_CP_EN, "LCD_CP_EN");
	set_mode(adev->client, MODE_USER);

	return 0;

cleanup:
	kfree(adev);
	return -ENOSYS;
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

MODULE_DESCRIPTION("AAT2870 Backlight Control");
MODULE_AUTHOR("Stefan Demharter, Yool-Je Cho <yoolje.cho@lge.com>");
MODULE_LICENSE("GPL");
