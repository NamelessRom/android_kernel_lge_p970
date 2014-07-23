/*
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
 */

#include "aat2870_bl.h"
#include "aat2870_bl_als.h"
#include "aat2870_bl_ld.h"
#include "aat2870_bl_sysfs.h"

#include <linux/gpio.h>
#include <linux/sysfs.h>

#define als_dbg(...) dev_dbg(&als->client->dev, __VA_ARGS__)
#define als_info(...) dev_info(&als->client->dev, __VA_ARGS__)
#define als_warn(...) dev_warn(&als->client->dev, __VA_ARGS__)
#define als_err(...) dev_err(&als->client->dev, __VA_ARGS__)

static const char *GAIN_MODE_str[] = {
	[GAIN_MODE_LOW] = "low",
	[GAIN_MODE_HIGH] = "high",
	[GAIN_MODE_AUTO] = "auto",
};

static const char *GAIN_MODE_long_str[] = {
	[GAIN_MODE_LOW] = "low gain mode",
	[GAIN_MODE_HIGH] = "high gain mode",
	[GAIN_MODE_AUTO] = "auto gain mode (two resistors: normal brightness / dim brightness)",
};

static const char *gain_resistor_str[][4] = {
	[GAIN_MODE_LOW] = {
		[GAIN_RESISTOR_0] = "250 Ohm",
		[GAIN_RESISTOR_1] = "1 kOhm",
		[GAIN_RESISTOR_2] = "4 kOhm",
		[GAIN_RESISTOR_3] = "16 kOhm",
	},
	[GAIN_MODE_HIGH] = {
		[GAIN_RESISTOR_0] = "1 kOhm",
		[GAIN_RESISTOR_1] = "4 kOhm",
		[GAIN_RESISTOR_2] = "16 kOhm",
		[GAIN_RESISTOR_3] = "64 kOhm",
	},
	[GAIN_MODE_AUTO] = {
		[GAIN_RESISTOR_0] = "250 Ohm / 1 kOhm",
		[GAIN_RESISTOR_1] = "1 kOhm / 4 kOhm",
		[GAIN_RESISTOR_2] = "4 kOhm / 16 kOhm",
		[GAIN_RESISTOR_3] = "16 kOhm / 64 kOhm",
	}
};

static size_t gain_percentage_long_str(struct kobject *kobj,
		struct enum_attr *attr, int val, char *buf, size_t size)
{
	return scnprintf(buf, size, "%+d.%02d %%",
		val * GAIN_MULT_PER_10000 / 100,
		(val * GAIN_MULT_PER_10000) % 100);
}

/*
 * Fill the als brightness levels from min_lvl to max_level
 * in an exponential way and respect the offset, i.e.
 * br_i = (min + off) * ((max + off) / (min + off)) ^ (i / 15) - off
 */
static void fill_als_brightness_levels(struct als *als, u8 min_lvl, u8 max_lvl)
{
	u8 o = als->ld_offset;
	u8 l = min_lvl;
	u8 u = max_lvl;
	int i = 0;

	for (; i < BRIGHTNESS_REGS; ++i) {
		uint32_t ld_brightness = ld[l + o] +
			DIV_ROUND_CLOSEST(i * (ld[u + o] - ld[l + o]), BRIGHTNESS_REGS - 1);
		uint32_t brightness_o = ceil_pow_2(ld_brightness);
		als_dbg("ceil(exp(%u / 2^16)) = %u\n", ld_brightness, brightness_o);
		als->brightness_levels[i] = brightness_o - o;
	}
}

static size_t gain_resistor_long_str(struct kobject *kobj,
		struct enum_attr *attr, int val, char *buf, size_t size)
{
	struct als *als =
		container_of(kobj, struct als, kobj);

	return scnprintf(buf, size, "%s", gain_resistor_str[als->gain_mode][val]);
}


const int gain_mode_reg_val[] = {
	[GAIN_MODE_LOW] = 0x02,
	[GAIN_MODE_HIGH] = 0x06,
	[GAIN_MODE_AUTO] = 0x00,
};

const int als_measure_interval_ms[] = {
	[GAIN_MODE_LOW] = 200,
	[GAIN_MODE_HIGH] = 200,
	[GAIN_MODE_AUTO] = 300,
};

static int *int_from_kobj_offset(struct kobject *kobj, long *data)
{
	return (int *)(((char *)kobj) + *data);
}

ssize_t brightness_levels_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct als *als = container_of(kobj, struct als, kobj);
	int reg;
	int o = 0;

	for (reg = 0; reg < BRIGHTNESS_REGS; ++reg) {
		o += scnprintf(buf + o, PAGE_SIZE - o,
			"%d%s", als->brightness_levels[reg],
			reg != BRIGHTNESS_REGS - 1 ? " " : "\n");
	}

	return o;
}

static ssize_t brightness_levels_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct als *als = container_of(kobj, struct als, kobj);
	u8 vals[BRIGHTNESS_REGS];
	int o = 0;
	int i;

	for (i = 0; i < BRIGHTNESS_REGS; ++i) {
		int read = 0;
		uint32_t val;
		sscanf(buf + o, "%d%n", &val, &read);

		if (read == 0)
			break;

		if (val > BRIGHTNESS_MAX) {
			als_err("Brightness value %d too high (> %d)", val, BRIGHTNESS_MAX);
			return -EINVAL;
		}

		vals[i] = val;
		o += read;

		if (count != o && buf[o] != ' ' && buf[o] != '\n')
			return -EINVAL;

		o += 1;
	}

	mutex_lock(&als->mutex);
	if (i == 1) {
		/* one value given, calc brightness levels between this value and 127 */
		fill_als_brightness_levels(als, vals[0], BRIGHTNESS_MAX);
	} else if (i == 2) {
		/* two vals given, calc brightness levels between those two values */
		fill_als_brightness_levels(als, vals[0], vals[1]);
	} else if (i == BRIGHTNESS_REGS) {
		int j;
		for (j = 0; j < BRIGHTNESS_REGS; ++j) {
			als->brightness_levels[j] = vals[j];
		}
	} else {
		return -EINVAL;
	}
	mutex_unlock(&als->mutex);

	return count;
}

static ssize_t trigger_measure_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t size)
{
	struct als *als = container_of(kobj, struct als, kobj);
	uint32_t msecs_ago = jiffies_to_msecs(jiffies - als->level_jif);

	if (msecs_ago > als->polling_interval_ms) {
		schedule_delayed_work(&als->update_als_level_stage1, 0);
	}

	return size;
}

static ssize_t level_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct als *als = container_of(kobj, struct als, kobj);

	return scnprintf(buf, PAGE_SIZE, "%d/%d\n", (int)als->level, ALS_LVL_MAX);
}

static ssize_t level_measured_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct als *als = container_of(kobj, struct als, kobj);
	uint32_t msecs_ago = jiffies_to_msecs(jiffies - als->level_jif);
	uint32_t msecs_10 = (msecs_ago + 5) / 10 * 10;
	uint32_t msecs_100 = (msecs_ago + 50) / 100 * 100;
	uint32_t poll_ival = als->polling_interval_ms;

	/* We use rounded values because of inaccuracies */
	msecs_ago = msecs_10 < 1000 ? msecs_10 : msecs_100;

	if (msecs_ago > poll_ival) {
		/*
		 * Schedule a work item if none is scheduled
		 * so that the als level is updated after the read
		 */
		schedule_delayed_work(&als->update_als_level_stage1, 0);
	}

	return scnprintf(buf, PAGE_SIZE, "%u ms ago\n", msecs_ago);
}

#define KOBJ_OFFSET(_val) int_from_kobj_offset, \
	(offsetof(struct als, _val) - offsetof(struct als, kobj))

static KOBJ_INT_RO_ATTR(listeners, 0444, KOBJ_OFFSET(listeners));
static KOBJ_INT_ATTR(adapt_brightness_delay, 0644, 0, 10000,
	KOBJ_OFFSET(adapt_brightness_delay_ms), NULL);
static KOBJ_ATTR(brightness_levels, 0644,
	brightness_levels_show, brightness_levels_store);
static KOBJ_ENUM_FN_ATTR(gain_resistor, 0644, GAIN_RESISTOR,
	enum_int_str, gain_resistor_long_str,
	KOBJ_OFFSET(gain_resistor), NULL);
static KOBJ_ENUM_LONG_ATTR(gain_mode, 0644,
	GAIN_MODE, KOBJ_OFFSET(gain_mode), NULL);
static KOBJ_ENUM_FN_ATTR(gain_percentage, 0644, GAIN_PERCENTAGE,
	enum_int_str, gain_percentage_long_str,
	KOBJ_OFFSET(gain), NULL);
static KOBJ_INFO_ATTR(info, 0444,
	"Via adapt_brightness_delay you can set the delay in ms before\n"
	"adapting the lcd brightnesss to a change in ambient brightness\n\n"
	"Via brightness_levels you can set all the als brightness levels.\n"
	"You can also provide just the minimum and maximum brightness level.\n"
	"The intermediate brightness levels are then calculated automatically in\n"
	"an exponential way to match the logarithmic response of the human eye.\n"
	"The formula depends on the value of ld_offset.\n"
	"You can also omit the maximum level in which case " BRIGHTNESS_MAX_STR " is used\n"
	"All brightness levels have to be in the interval [0, " BRIGHTNESS_MAX_STR "]\n\n"
	"The polling interval in ms can be set via polling_interval,\n"
	"but it may be overwritten if the brightness_mode of the device is changed.\n\n"
	"Automatic polling is only enabled if there are listeners.\n"
	"You can lookup the value in the corresponding file.\n"
	"If there is no listener you can trigger a manual measure once in the polling interval\n"
	"by writing to trigger_measure.");
static KOBJ_INT_ATTR(ld_offset, 0644, LD_OFFSET_MIN, LD_OFFSET_MAX,
	KOBJ_OFFSET(ld_offset), NULL);
static KOBJ_ATTR(level, 0444, level_show, NULL);
static KOBJ_ATTR(level_measured, 0444, level_measured_show, NULL);
static KOBJ_INT_ATTR(polling_interval, 0644,
	ALS_IVAL_MIN, ALS_IVAL_MAX,
	KOBJ_OFFSET(polling_interval_ms), NULL);
static KOBJ_ATTR(trigger_measure, 0200, NULL, trigger_measure_store);

static struct attribute *attrs[] = {
	&kobj_int_attr_adapt_brightness_delay.attr.kobj_attr.attr,
	&kobj_int_attr_adapt_brightness_delay.min_attr.kobj_attr.attr,
	&kobj_int_attr_adapt_brightness_delay.max_attr.kobj_attr.attr,
	&kobj_int_ro_attr_listeners.attr.kobj_attr.attr,
	&kobj_info_attr_info.attr.kobj_attr.attr,
	&kobj_attr_brightness_levels.attr,
	&kobj_enum_attr_gain_mode.attr.kobj_attr.attr,
	&kobj_enum_attr_gain_resistor.attr.kobj_attr.attr,
	&kobj_enum_attr_gain_percentage.attr.kobj_attr.attr,
	&kobj_attr_level.attr,
	&kobj_attr_level_measured.attr,
	&kobj_int_attr_ld_offset.attr.kobj_attr.attr,
	&kobj_int_attr_polling_interval.attr.kobj_attr.attr,
	&kobj_int_attr_polling_interval.min_attr.kobj_attr.attr,
	&kobj_int_attr_polling_interval.max_attr.kobj_attr.attr,
	&kobj_attr_trigger_measure.attr,
	NULL,
};

/* start the als measure and call stage2 after the end of the measure interval */
static void update_als_level_stage1(struct work_struct *ws)
{
	struct als *als=
		container_of(ws, struct als, update_als_level_stage1.work);
	int delay_ms;

	mutex_lock(&als->mutex);

	if (als->measure_running) {
		als_warn("als measure already running, don't start it twice, thus doing a reschedule\n");
		goto reschedule;
	}

	if (gpio_get_value(LCD_CP_EN) == 0) {
		gpio_set_value(LCD_CP_EN, 1);
	}
	/*
	 * Set SBIAS power to on
	 * This gives power to the ambient light sensors
	 * and also activates the handling of the brightness via
	 * the als-brightness registers (REG18-REG33)
	 */
	i2c_write_reg(als->client, REG_ALS_CFG1, 0x01);
	/* set manual polling and set gain */
	i2c_write_reg(als->client, REG_ALS_CFG2, 0xF0 | (als->gain & 0x0F));
	/* set logarithmic output, enable als, set gain resistor and mode */
	i2c_write_reg(als->client, REG_ALS_CFG0,
		0x41 |
		((als->gain_resistor & 0x03) << 4) |
		gain_mode_reg_val[als->gain_mode]
	);
	/*
	 * read als level after 210 ms, i.e. after the end of the
	 * measure interval (200 ms) (resp. 310 ms / 300 ms if in auto gain mode).
	 * See also figure 26 in the datasheet.
	 */
	delay_ms = als_measure_interval_ms[als->gain_mode] + 10;
	if (schedule_delayed_work(&als->update_als_level_stage2,
		msecs_to_jiffies(delay_ms))) {
		als->measure_running = true;
	}

reschedule:
	if (als->listeners) {
		schedule_delayed_work(&als->update_als_level_stage1,
			msecs_to_jiffies(als->polling_interval_ms));
	} else {
		als_dbg("Not scheduling a next als measure as there are no listeners\n");
	}

	mutex_unlock(&als->mutex);
}

static void store_als_level(struct als *als)
{
	s8 val = 0;
	int als_level;
	int res = i2c_read_reg(als->client, REG_AMB, &val);

	if (res < 0)
		return;

	als_level = (int)val >> 3;
	als_dbg("als_level = 0x%x\n", als_level);
	als->level = als_level;
	als->level_jif = jiffies;
}

/*
 * Disable the als again, read the measured als level, store it in als->level
 * and call the als notifiers.
 */
static void update_als_level_stage2(struct work_struct *ws)
{
	struct als *als =
		container_of(ws, struct als, update_als_level_stage2.work);
	bool als_level_updated = false;

	mutex_lock(&als->mutex);
	als->measure_running = false;
	if (gpio_get_value(LCD_CP_EN) == 1) {
		/* gpio is still active and we can read the measured als level */

		/* disable als */
		i2c_write_reg(als->client, REG_ALS_CFG0,
			0x40 |
			((als->gain_resistor & 0x03) << 4) |
			gain_mode_reg_val[als->gain_mode]
		);
		store_als_level(als);

		if (als->stay_off) {
			/* Need to disable gpio again if it was only enabled by stage1 */
			gpio_set_value(LCD_CP_EN, 0);
		}
		als_level_updated = true;
	}
	mutex_unlock(&als->mutex);

	if (als_level_updated) {
		/* Call all who want to be notified about the updated als level */
		srcu_notifier_call_chain(&als->level_update_notifier, als->level, NULL);
	}
};

int als_add_listener(struct als *als, struct notifier_block *listener)
{
	int res;

	mutex_lock(&als->mutex);
	res = srcu_notifier_chain_register(&als->level_update_notifier, listener);
	if (res == 0) {
		if (als->listeners++ == 0) {
			schedule_delayed_work(&als->update_als_level_stage1, 0);
		}
	} else
		als_warn("srcu_notifier_chain_register returned %d\n", res);
	mutex_unlock(&als->mutex);

	return res;
}

int als_remove_listener(struct als *als, struct notifier_block *listener)
{
	int res;

	mutex_lock(&als->mutex);
	res = srcu_notifier_chain_unregister(&als->level_update_notifier, listener);
	if (res == 0) {
		if (--als->listeners == 0) {
			cancel_delayed_work(&als->update_als_level_stage1);
		}
	} else
		als_warn("srcu_notifier_chain_register returned %d\n", res);
	mutex_unlock(&als->mutex);

	return res;
}

void als_stay_off(struct als *als, bool stay_off)
{
	mutex_lock(&als->mutex);
	als->stay_off = stay_off;
	mutex_unlock(&als->mutex);
}

static struct kobj_type kt = {
	.sysfs_ops = &kobj_sysfs_ops,
	.default_attrs = attrs,
};

int als_init(struct als *als, struct i2c_client *client)
{
	int ret = kobject_init_and_add(&als->kobj, &kt, &client->dev.kobj, "sensor");

	if (ret) {
		kobject_put(&als->kobj);
		return -EINVAL;
	}
	als->client = client;

	INIT_DELAYED_WORK(&als->update_als_level_stage1, update_als_level_stage1);
	INIT_DELAYED_WORK(&als->update_als_level_stage2, update_als_level_stage2);
	mutex_init(&als->mutex);

	fill_als_brightness_levels(als, 0x06, 0x7F);

	srcu_init_notifier_head(&als->level_update_notifier);
	als->adapt_brightness_delay_ms = 600;

	als->gain_mode = GAIN_MODE_HIGH;
	als->gain_resistor = GAIN_RESISTOR_2;
	als->gain = GAIN_PERCENTAGE_43_75;;

	kobject_uevent(&als->kobj, KOBJ_ADD);

	return 0;
}

void als_cleanup(struct als *als)
{
	cancel_delayed_work_sync(&als->update_als_level_stage1);
	cancel_delayed_work_sync(&als->update_als_level_stage2);

	srcu_cleanup_notifier_head(&als->level_update_notifier);
}
