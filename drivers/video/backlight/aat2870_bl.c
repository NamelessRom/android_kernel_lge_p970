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

#define DEBUG

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
#include <linux/hrtimer.h>
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
#define gdev_dbg(...) dev_dbg(&aat2870_i2c_client->dev, __VA_ARGS__)
#define gdev_info(...) dev_info(&aat2870_i2c_client->dev, __VA_ARGS__)
#define gdev_err(...) dev_err(&aat2870_i2c_client->dev, __VA_ARGS__)

#define for_array(_idx, _array) for (_idx = 0; _idx < ARRAY_SIZE(_array); ++_idx)
#define for_array_rev(_idx, _array) for (_idx = ARRAY_SIZE(_array) - 1; _idx-- > 0;)
#define for_enum(_idx, _enum) for (_idx = _enum ## _FIRST; _idx <= _enum ## _LAST; ++_idx)


struct i2c_client *aat2870_i2c_client;
EXPORT_SYMBOL(aat2870_i2c_client);

/*
 * This driver uses some fixed point arithmetic to calculate
 * the fading brightness levels. The following "ld" table
 * is used to fade the brightness in an exponential way
 * to match the logarithmic response of the human eye.
 */

#define LD_OFFSET_MIN 1
#define LD_OFFSET_DFL 4
#define LD_OFFSET_MAX 128

/* round(ln(x) / ln(2) * 2^16) for x in range(1, 257) */
static const uint32_t ld[] = {
	UINT_MAX,
	    0x0, 0x10000, 0x195c0, 0x20000, 0x2526a, 0x295c0, 0x2ceaf, 0x30000,
	0x32b80, 0x3526a, 0x3759d, 0x395c0, 0x3b350, 0x3ceaf, 0x3e82a, 0x40000,
	0x41664, 0x42b80, 0x43f78, 0x4526a, 0x4646f, 0x4759d, 0x48608, 0x495c0,
	0x4a4d4, 0x4b350, 0x4c140, 0x4ceaf, 0x4dba5, 0x4e82a, 0x4f446, 0x50000,
	0x50b5d, 0x51664, 0x52119, 0x52b80, 0x5359f, 0x53f78, 0x54910, 0x5526a,
	0x55b89, 0x5646f, 0x56d20, 0x5759d, 0x57dea, 0x58608, 0x58dfa, 0x595c0,
	0x59d5e, 0x5a4d4, 0x5ac24, 0x5b350, 0x5ba59, 0x5c140, 0x5c807, 0x5ceaf,
	0x5d538, 0x5dba5, 0x5e1f5, 0x5e82a, 0x5ee45, 0x5f446, 0x5fa2f, 0x60000,
	0x605ba, 0x60b5d, 0x610eb, 0x61664, 0x61bc8, 0x62119, 0x62656, 0x62b80,
	0x63098, 0x6359f, 0x63a94, 0x63f78, 0x6444c, 0x64910, 0x64dc5, 0x6526a,
	0x65700, 0x65b89, 0x66003, 0x6646f, 0x668ce, 0x66d20, 0x67165, 0x6759d,
	0x679ca, 0x67dea, 0x681ff, 0x68608, 0x68a06, 0x68dfa, 0x691e2, 0x695c0,
	0x69994, 0x69d5e, 0x6a11e, 0x6a4d4, 0x6a881, 0x6ac24, 0x6afbe, 0x6b350,
	0x6b6d9, 0x6ba59, 0x6bdd1, 0x6c140, 0x6c4a8, 0x6c807, 0x6cb5f, 0x6ceaf,
	0x6d1f7, 0x6d538, 0x6d872, 0x6dba5, 0x6ded0, 0x6e1f5, 0x6e513, 0x6e82a,
	0x6eb3b, 0x6ee45, 0x6f149, 0x6f446, 0x6f73e, 0x6fa2f, 0x6fd1a, 0x70000,
	0x702e0, 0x705ba, 0x7088e, 0x70b5d, 0x70e27, 0x710eb, 0x713aa, 0x71664,
	0x71919, 0x71bc8, 0x71e73, 0x72119, 0x723ba, 0x72656, 0x728ed, 0x72b80,
	0x72e0f, 0x73098, 0x7331e, 0x7359f, 0x7381b, 0x73a94, 0x73d08, 0x73f78,
	0x741e4, 0x7444c, 0x746b0, 0x74910, 0x74b6c, 0x74dc5, 0x75019, 0x7526a,
	0x754b7, 0x75700, 0x75946, 0x75b89, 0x75dc7, 0x76003, 0x7623a, 0x7646f,
	0x766a0, 0x768ce, 0x76af8, 0x76d20, 0x76f44, 0x77165, 0x77383, 0x7759d,
	0x777b5, 0x779ca, 0x77bdb, 0x77dea, 0x77ff6, 0x781ff, 0x78405, 0x78608,
	0x78809, 0x78a06, 0x78c01, 0x78dfa, 0x78fef, 0x791e2, 0x793d2, 0x795c0,
	0x797ab, 0x79994, 0x79b7a, 0x79d5e, 0x79f3f, 0x7a11e, 0x7a2fa, 0x7a4d4,
	0x7a6ab, 0x7a881, 0x7aa53, 0x7ac24, 0x7adf2, 0x7afbe, 0x7b188, 0x7b350,
	0x7b515, 0x7b6d9, 0x7b89a, 0x7ba59, 0x7bc16, 0x7bdd1, 0x7bf8a, 0x7c140,
	0x7c2f5, 0x7c4a8, 0x7c658, 0x7c807, 0x7c9b4, 0x7cb5f, 0x7cd08, 0x7ceaf,
	0x7d054, 0x7d1f7, 0x7d399, 0x7d538, 0x7d6d6, 0x7d872, 0x7da0c, 0x7dba5,
	0x7dd3b, 0x7ded0, 0x7e063, 0x7e1f5, 0x7e385, 0x7e513, 0x7e69f, 0x7e82a,
	0x7e9b3, 0x7eb3b, 0x7ecc1, 0x7ee45, 0x7efc8, 0x7f149, 0x7f2c8, 0x7f446,
	0x7f5c3, 0x7f73e, 0x7f8b7, 0x7fa2f, 0x7fba5, 0x7fd1a, 0x7fe8e, 0x80000,
	UINT_MAX, /* sentinel element */
};

/*
 * ceil(2^(ld_val / 2^16)), i.e.
 * find smallest idx with ld_val <= ld[idx]
 * valid for 0 <= ld_val <= last_ld_element
 */
static uint32_t ceil_pow_2(uint32_t ld_val)
{
	size_t start = 1;
	size_t end = ARRAY_SIZE(ld) - 2;
	size_t mid;

	while (start <= end) {
		uint32_t ld_mid;
		mid = (start + end) / 2;
		ld_mid = ld[mid];
		if (ld_val <= ld_mid)
			end = mid - 1;
		else
			start = mid + 1;
	}

	return end + 1;
}

/* 0x7F is the register value of the maximum brightness */
#define BRIGHTNESS_MAX 		0x7F
#define BRIGHTNESS_DEFAULT 	0x3F

enum REG {
	REG_EN_CH = 0x00, /* Enable backlight channels */
	REG_ALS_CFG0 = 0x0E, /* gain resistor, als enable, logarithmic i/o */
	REG_ALS_CFG1 = 0x0F, /* sbias voltage, enable */
	REG_ALS_CFG2 = 0x10, /* polling mode, interval, gain */
	REG_AMB = 0x11, /* ambient light level digital output */
	REG_ALS0 = 0x12, /* ALS brightness level for als_level 0 */
	/* ... */
	REG_ALS15 = 0x21, /* ALS brightness level for als_level 15 */
	REG_LDOAB = 0x24, /* LDO A+B output voltage level */
	REG_LDOCD = 0x25, /* LDO C+D output voltage level */
	REG_EN_LDO = 0x26, /* LDO A-D output enable */
};

/* brightness regs are from REG18 to REG33 */
#define BRIGHTNESS_REGS (REG_ALS15 - REG_ALS0 + 1)

#define ALL_CH_ON	0xFF
#define ALL_CH_OFF	0x00

#define LDO_EN_ALL	0x0F
#define LDO_DIS_ALL	0x00
#define LDO_3V_1_8V	0x4C

enum MODE {
	MODE_FIXED,
	MODE_AUTO,
	MODE_SCREEN_OFF,

	MODE_FIRST = MODE_FIXED,
	MODE_LAST = MODE_SCREEN_OFF,
};

#define ENUM_SIZE(_enum) ((_enum ## _LAST) - (_enum ## _FIRST) + 1)

static const char *mode_str[] = {
	[MODE_FIXED] = "fixed-brightness",
	[MODE_AUTO] = "auto-brightness",
	[MODE_SCREEN_OFF] = "screen off",
};

struct input_val {
	const char *input;
	int val;
};

static struct input_val mode_input_val[] = {
	{"0", MODE_FIXED},
	{"fixed", MODE_FIXED},
	{"1", MODE_AUTO},
	{"auto", MODE_AUTO},
};

static struct input_val onoff_input_val[] = {
	{"0", 0},
	{"off", 0},
	{"1", 1},
	{"on", 1},
};

enum STATE {
	SLEEP_STATE,
	WAKE_STATE,
};

enum FADE_STATE {
	FADE_STATE_STOPPED,
	FADE_STATE_SCHEDULED,
	FADE_STATE_IN_PROGRESS,
};

enum GAIN_RESISTOR {
	GAIN_RESISTOR_0,
	GAIN_RESISTOR_1,
	GAIN_RESISTOR_2,
	GAIN_RESISTOR_3,

	GAIN_RESISTOR_FIRST = GAIN_RESISTOR_0,
	GAIN_RESISTOR_LAST = GAIN_RESISTOR_3,
};

enum GAIN_MODE {
	GAIN_MODE_LOW,
	GAIN_MODE_HIGH,
	GAIN_MODE_AUTO,

	GAIN_MODE_FIRST = GAIN_MODE_LOW,
	GAIN_MODE_LAST = GAIN_MODE_AUTO,
};

const int gain_mode_reg_val[] = {
	[GAIN_MODE_LOW] = 0x02,
	[GAIN_MODE_HIGH] = 0x06,
	[GAIN_MODE_AUTO] = 0x00,
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

const int als_measure_interval[] = {
	[GAIN_MODE_LOW] = 200,
	[GAIN_MODE_HIGH] = 200,
	[GAIN_MODE_AUTO] = 300,
};

#define GAIN_MIN -8
#define GAIN_MAX 7
#define GAIN_MULT_PER_10000 625

static const char *gain_mode_str[] = {
	[GAIN_MODE_LOW] = "low gain mode",
	[GAIN_MODE_HIGH] = "high gain mode",
	[GAIN_MODE_AUTO] = "auto gain mode (normal brightnes / dim brightness)",
};

static const char *gain_mode_input_str[] = {
	[GAIN_MODE_LOW] = "low",
	[GAIN_MODE_HIGH] = "high",
	[GAIN_MODE_AUTO] = "auto",
};

struct fade {
	bool enabled;
	u8 brightness_target;
	u8 brightness_next;
	u8 brightness_start;
	/* fade in progress or scheduled */
	enum FADE_STATE state;
	/* minimum interval per fade step */
	size_t min_step_ival_ms;
	/* interval for the full fade */
	size_t full_ival_ms;
	/* an offset used in calculations with the logarithm */
	size_t ld_offset;

	struct mutex mutex;

	struct delayed_work fade_set_brightness_work;
	struct delayed_work fade_brightness_start_work;
};

struct als_notifier {
	int (*register_als_notifier)(struct als_notifier *, struct notifier_block *);
	int (*unregister_als_notifier)(struct als_notifier *, struct notifier_block *);
};

struct als {
	/* the 16 brightness levels for the 16 als levels (0-127) */
	u8 brightness_levels[BRIGHTNESS_REGS];
	/* the last measured als level (0-15) */
	u8 level;
	/* the time of the last als measure */
	unsigned long level_jif;
	size_t polling_interval_ms[ENUM_SIZE(MODE)];

	enum GAIN_MODE gain_mode;
	enum GAIN_RESISTOR gain_resistor;
	int gain; /* multiply by 6.25 to get the gain in percent */

	struct delayed_work store_als_stage1_work;
	struct delayed_work store_als_stage2_work;
	bool measure_running;

	/* delay to adapt lcd brightness after a change in ambient brightness */
	size_t adapt_brightness_delay_ms;

	/* notifier for als level updates */
	struct srcu_notifier_head level_update_notifier;
	size_t registered_notifiers;

	/*
	 * notifier block for als level updates, i.e. this one is responsible
	 * for altering the brightness upon a change in ambient brightness
	 */
	struct notifier_block set_brightness_nb;

	struct als_notifier als_notifier;
};

struct aat2870_device {
	struct i2c_client *client;
	struct backlight_device *bl_dev;
	struct led_classdev led;

	struct fade fade;
	struct als als;

	struct mutex mutex;

	enum STATE state;
	int bl_resumed;

	enum MODE mode;
	enum MODE saved_mode;
	u8 brightness;

	struct delayed_work set_brightness_delayed_work;

#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
#endif
};

#define LCD_CP_EN				62
#define HUB_PANEL_LCD_RESET_N	34
#define HUB_PANEL_LCD_CS		54
#define AAT2870_I2C_BL_NAME	"aat2870_i2c_bl"

static size_t default_als_polling_interval_ms[] = {
	[MODE_AUTO] = 500,
	[MODE_FIXED] = 10000,
	[MODE_SCREEN_OFF] = 60000,
};

static const struct i2c_device_id aat2870_bl_id[] = {
	{AAT2870_I2C_BL_NAME, 0},
	{},
};

static void set_mode(struct i2c_client *client, enum MODE mode);

static int i2c_read_reg(struct i2c_client *client,
		unsigned char reg, unsigned char *pval)
{
	int ret;
	struct aat2870_device *adev = i2c_get_clientdata(client);

	ret = i2c_smbus_read_byte_data(client, reg);
	if (ret < 0) {
		adev_err("Failed to read reg = 0x%x\n", (int)reg);
		return -EIO;
	}

	*pval = ret;

	return 0;
}

static int i2c_write_reg(struct i2c_client *client,
		enum REG reg, unsigned char val)
{
	int status = 0;
	int ret = i2c_smbus_write_byte_data(client, reg, val);
	struct aat2870_device *adev = i2c_get_clientdata(client);

	if (ret != 0) {
		adev_err("Failed to write (reg = 0x%02x, val = 0x%02x)\n", (int)reg, (int)val);
		return -EIO;
	} else {
		adev_dbg("Written reg = 0x%02x, val = 0x%02x\n", (int)reg, (int)val);
	}

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
 * Fill the als brightness levels from min_lvl to max_level
 * in an exponential way and respect the offset, i.e.
 * br_i = (min + off) * ((max + off) / (min + off)) ^ (i / 15) - off
 */
static void fill_als_brightness_levels(struct aat2870_device *adev,
		u8 min_lvl, u8 max_lvl)
{
	u8 o = adev->fade.ld_offset;
	u8 l = min_lvl;
	u8 u = max_lvl;
	int i = 0;
	for (; i < BRIGHTNESS_REGS; ++i) {
		uint32_t ld_brightness = ld[l + o] +
			DIV_ROUND_CLOSEST(i * (ld[u + o] - ld[l + o]), BRIGHTNESS_REGS - 1);
		uint32_t brightness_o = ceil_pow_2(ld_brightness);
		adev_dbg("ceil(exp(%u / 2^16)) = %u\n", ld_brightness, brightness_o);
		adev->als.brightness_levels[i] = brightness_o - o;
	}
}

/*
 * find next brightness s.t. fade_step_ms > min_step_ival_ms
 * or the target brightness is reached
 */
static u8 fade_find_next(struct aat2870_device *adev)
{
	struct fade *fade = &adev->fade;
	int32_t min_ival = msecs_to_jiffies(fade->min_step_ival_ms);
	int32_t full_ival = msecs_to_jiffies(fade->full_ival_ms);
	u8 start = fade->brightness_start;
	u8 cur = fade->brightness_next;
	u8 next = cur;
	u8 target = fade->brightness_target;
	int o = fade->ld_offset;
	/*
	 * fade_step_ms =
	 * (ld[next + o] - ld[cur + o]) / (ld[target + o] - ld[start + o]) * full_ival
	 */
	int32_t ld_next_o;
	if (start > target) {
		ld_next_o = ld[cur + o] -
			min_ival * (ld[start + o] - ld[target + o]) / full_ival;
		while (next != target && ld[next + o] > ld_next_o)
			--next;
	} else {
		ld_next_o = ld[cur + o] +
			min_ival * (ld[target + o] - ld[start + o]) / full_ival;
		while (next != target && ld[next + o] < ld_next_o)
			++next;
	}
	adev_dbg("Fade: next %d ldo %d lim %d\n",
		(int)next, (int)ld[next + o], (int)ld_next_o);

	return next;
}

/* Calculate the next fade step and schedule it (if necessary) */
static void fade_brightness_schedule(struct aat2870_device *adev)
{
	uint32_t fade_step_ival;
	struct fade *fade = &adev->fade;
	u8 next;
	u8 start = fade->brightness_start;
	u8 cur = fade->brightness_next;
	u8 target = fade->brightness_target;
	int o = fade->ld_offset;

	if (target == cur) {
		/* target brightness reached, so stop */
		fade->state = FADE_STATE_STOPPED;
		return;
	}

	next = fade_find_next(adev);
	fade->brightness_next = next;

	if (target < start) {
		fade_step_ival = DIV_ROUND_CLOSEST(
			msecs_to_jiffies(fade->full_ival_ms) * (ld[cur + o] - ld[next + o]),
			(ld[start + o] - ld[target + o]));
	} else {
		fade_step_ival = DIV_ROUND_CLOSEST(
			msecs_to_jiffies(fade->full_ival_ms) * (ld[next + o] - ld[cur + o]),
			(ld[target + o] - ld[start + o]));
	}

	adev_dbg("Fade step from %d to %d scheduled in %u ms\n",
		(int)cur, (int)next, jiffies_to_msecs(fade_step_ival));

	schedule_delayed_work(&fade->fade_set_brightness_work, fade_step_ival);
}

/*
 * Set the brightness to the precalculated next brightness step
 * and schedule a new brightness step (if necessary)
 */
static void fade_set_brightness(struct work_struct *ws)
{
	struct aat2870_device *adev =
		container_of(ws, struct aat2870_device, fade.fade_set_brightness_work.work);

	adev_dbg("Fade step from %d to %d\n",
		(int)adev->brightness, (int)adev->fade.brightness_next);

	mutex_lock(&adev->mutex);
	adev->brightness = adev->fade.brightness_next;
	i2c_set_brightness(adev->client);
	mutex_unlock(&adev->mutex);

	mutex_lock(&adev->fade.mutex);
	if (adev->fade.state == FADE_STATE_IN_PROGRESS) {
		fade_brightness_schedule(adev);
	}
	mutex_unlock(&adev->fade.mutex);
}

/* Start the brightness fade from fade->start to fade->target */
static void fade_brightness_start(struct work_struct *ws)
{
	struct aat2870_device *adev =
		container_of(ws, struct aat2870_device, fade.fade_brightness_start_work.work);

	mutex_lock(&adev->fade.mutex);
	if (adev->fade.state == FADE_STATE_SCHEDULED) {
		adev->fade.state = FADE_STATE_IN_PROGRESS;
		fade_brightness_schedule(adev);
	}
	mutex_unlock(&adev->fade.mutex);
}

static void fade_stop(struct aat2870_device *adev);

/*
 * Start a delayed fade from the current brightness to the given brightness
 * If a fade is still in progress, stop it if necessary or adapt it.
 */
static void fade_brightness_delayed_to_impl(struct aat2870_device *adev, u8 new_brightness)
{
	struct fade *fade = &adev->fade;
	struct als *als = &adev->als;
	/* schedule fade after the configured delay if in mode auto, otherwise schedule immediately */
	int delay_ms = adev->mode == MODE_AUTO ? als->adapt_brightness_delay_ms : 0;

	if (adev->brightness == new_brightness && fade->state == FADE_STATE_STOPPED) {
		/*
		 * nothing to do, as the target brightness is already reached
		 * and no fader is running
		 */
		return;
	}

	if (fade->state != FADE_STATE_STOPPED) {
		/* 1 = down, 0 = up */
		bool direction = fade->brightness_next > fade->brightness_target;
		bool new_direction = fade->brightness_next > new_brightness;
		if (direction == new_direction) {
			/*
			 * In that case the fader is already running in the right direction (or is scheduled),
			 * so just update start and target brightness
			 */
			fade->brightness_start = fade->brightness_next;
			fade->brightness_target = new_brightness;
			adev_dbg("Setting new target brightness of running fade to %d\n",
				(int)fade->brightness_target);
			return;
		}
		fade_stop(adev);
	}

	fade->brightness_target = new_brightness;
	fade->brightness_start = adev->brightness;
	fade->brightness_next = adev->brightness;
	fade->state = FADE_STATE_SCHEDULED;

	adev_info("Starting fade from %d to %d after %d ms (interval: %d ms)\n",
		(int)adev->brightness, (int)new_brightness,
		delay_ms, fade->full_ival_ms);

	schedule_delayed_work(&fade->fade_brightness_start_work,
		msecs_to_jiffies(delay_ms));
}

static void fade_brightness_delayed_to(struct aat2870_device *adev, u8 new_brightness)
{
	mutex_lock(&adev->fade.mutex);
	fade_brightness_delayed_to_impl(adev, new_brightness);
	mutex_unlock(&adev->fade.mutex);
}

/* Stop any scheduled fade or fade in progress */
static void fade_stop(struct aat2870_device *adev)
{
	struct fade *fade = &adev->fade;

	if (fade->state != FADE_STATE_STOPPED) {
		adev_info("Stopping fade\n");
		cancel_delayed_work(&fade->fade_brightness_start_work);
		cancel_delayed_work(&fade->fade_set_brightness_work);
		fade->state = FADE_STATE_STOPPED;
	}
}

static void set_brightness_delayed(struct work_struct *ws)
{
	struct aat2870_device *adev =
		container_of(ws, struct aat2870_device, set_brightness_delayed_work.work);

	mutex_lock(&adev->mutex);
	i2c_set_brightness(adev->client);
	mutex_unlock(&adev->mutex);
}

static void set_brightness_delayed_to(struct aat2870_device *adev, u8 brightness)
{
	struct als *als = &adev->als;
	int delay_ms = adev->mode == MODE_AUTO ? als->adapt_brightness_delay_ms : 0;

	mutex_lock(&adev->mutex);
	fade_stop(adev);
	if (adev->brightness != brightness) {
		adev->brightness = brightness;
		cancel_delayed_work(&adev->set_brightness_delayed_work);
		schedule_delayed_work(&adev->set_brightness_delayed_work,
			msecs_to_jiffies(delay_ms));
	}
	mutex_unlock(&adev->mutex);
}

/*
 * If a fading is disabled, set the brightness (delayed) to the given value
 * and stop any running fading, otherwise start a (delayed) fade to the new brightness
 */
static void set_brightness_to(struct aat2870_device *adev, u8 brightness)
{
	struct fade *fade = &adev->fade;

	if (fade->enabled) {
		adev_info("Fading brightness to %d\n", (int)brightness);
		fade_brightness_delayed_to(adev, brightness);
	} else {
		adev_info("Setting brightness to %d\n", (int)brightness);
		set_brightness_delayed_to(adev, brightness);
	}
}

static void set_brightness_to_fixed(struct aat2870_device *adev, u8 brightness)
{
	if (adev->mode == MODE_FIXED) {
		set_brightness_to(adev, brightness);
	} else {
		adev_info("Skipping brightness request to (%d/%d) as mode is set to "
			"`%s` instead of `%s`\n",
			(int)brightness, BRIGHTNESS_MAX,
			mode_str[adev->mode], mode_str[MODE_FIXED]);
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
			fade_stop(adev);
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
	gdev_dbg("ldo enable..\n");
	i2c_write_reg(client, REG_LDOAB, LDO_3V_1_8V);
	i2c_write_reg(client, REG_LDOCD, LDO_3V_1_8V);
	i2c_write_reg(client, REG_EN_LDO, LDO_EN_ALL);
}

int check_bl_shutdown = 0;
void hub_lcd_initialize(void)
{
	struct aat2870_device *adev = i2c_get_clientdata(aat2870_i2c_client);

	mutex_lock(&adev->mutex);
	gpio_set_value(LCD_CP_EN, 1);
	ldo_activate(adev->client);
	bl_on(adev->client);
	adev->bl_resumed = 1;
	mutex_unlock(&adev->mutex);
}
EXPORT_SYMBOL(hub_lcd_initialize);
EXPORT_SYMBOL(check_bl_shutdown);

void aat2870_shutdown(void)
{
	struct aat2870_device *adev = i2c_get_clientdata(aat2870_i2c_client);

	mutex_lock(&adev->mutex);
	adev->bl_resumed = 0;
	bl_off(adev->client);
	i2c_write_reg(aat2870_i2c_client, REG_EN_LDO, LDO_DIS_ALL);
	gpio_set_value(LCD_CP_EN, 0);
	check_bl_shutdown = 1;
	mutex_unlock(&adev->mutex);
}
EXPORT_SYMBOL(aat2870_shutdown);
#endif

/* start the als measure and call stage2 after the end of the measure interval */
static void store_als_stage1(struct work_struct *ws)
{
	struct aat2870_device *adev =
		container_of(ws, struct aat2870_device, als.store_als_stage1_work.work);
	struct als *als = &adev->als;
	int delay_ms;

	mutex_lock(&adev->mutex);

	if (als->measure_running)
		goto reschedule;

	if (gpio_get_value(LCD_CP_EN) == 0) {
		if (adev->state != SLEEP_STATE) {
			adev_warn("gpio is disabled, but device is not in sleep mode. "
				"Skipping ambient light measure.\n");
			goto reschedule;
		}
		gpio_set_value(LCD_CP_EN, 1);
	}
	/*
	 * Set SBIAS power to on
	 * This gives power to the ambient light sensors
	 * and also activates the handling of the brightness via
	 * the als-brightness registers (REG18-REG33)
	 */
	i2c_write_reg(adev->client, REG_ALS_CFG1, 0x01);
	/* set manual polling and set gain */
	i2c_write_reg(adev->client, REG_ALS_CFG2, 0xF0 | (als->gain & 0x0F));
	/* set logarithmic output, enable als, set gain resistor and mode */
	i2c_write_reg(adev->client, REG_ALS_CFG0,
		0x41 |
		((als->gain_resistor & 0x03) << 4) |
		gain_mode_reg_val[als->gain_mode]
	);
	/*
	 * read als level after 210 ms, i.e. after the end of the
	 * measure interval (200 ms) (resp. 310 ms / 300 ms if in auto gain mode).
	 * See also figure 26 in the datasheet.
	 */
	delay_ms = als_measure_interval[als->gain_mode] + 10;
	schedule_delayed_work(&als->store_als_stage2_work,
		msecs_to_jiffies(delay_ms));
	als->measure_running = true;

reschedule:
	if (als->registered_notifiers) {
		schedule_delayed_work(&als->store_als_stage1_work,
			msecs_to_jiffies(als->polling_interval_ms[adev->mode]));
	}

	mutex_unlock(&adev->mutex);
}

static void store_als_level(struct aat2870_device *adev)
{
	s8 val = 0;
	int als_level;
	int res = i2c_read_reg(adev->client, REG_AMB, &val);

	if (res < 0)
		return;

	als_level = (int)val >> 3;
	adev_dbg("als_level = 0x%x\n", als_level);
	adev->als.level = als_level;
	adev->als.level_jif = jiffies;
}

/*
 * Disable the als again, read the measured als level, store it in als->level
 * and call the als notifiers.
 */
static void store_als_stage2(struct work_struct *ws)
{
	struct aat2870_device *adev =
		container_of(ws, struct aat2870_device, als.store_als_stage2_work.work);
	struct als *als = &adev->als;
	bool als_level_updated = false;

	mutex_lock(&adev->mutex);
	als->measure_running = false;
	if (gpio_get_value(LCD_CP_EN) == 1) {
		/* gpio is still active and we can read the measured als level */

		/* disable als */
		i2c_write_reg(adev->client, REG_ALS_CFG0,
			0x40 |
			((als->gain_resistor & 0x03) << 4) |
			gain_mode_reg_val[als->gain_mode]
		);
		store_als_level(adev);

		if (adev->bl_resumed == 0) {
			/* Need to disable gpio again if it was only enabled by stage1 */
			gpio_set_value(LCD_CP_EN, 0);
		}
		als_level_updated = true;
	}
	mutex_unlock(&adev->mutex);

	if (als_level_updated) {
		/* Call all who want to be notified about the updated als level */
		srcu_notifier_call_chain(&als->level_update_notifier, adev->als.level, NULL);
	}
};

/*
 * The set brightness notifier, to be called from store_als_stage2
 * This method starts the brightness adjustments in auto-brightness mode
 */
static int set_brightness_notifier(struct notifier_block *nb,
		unsigned long als_level, void *data)
{
	struct aat2870_device *adev =
		container_of(nb, struct aat2870_device, als.set_brightness_nb);
	struct als *als = &adev->als;
	u8 b_new = als->brightness_levels[als->level];

	set_brightness_to(adev, b_new);
	return NOTIFY_OK;
}

#define CONVERT(_val, _from, _to) ((_to + 1) * _val / (_from + 1))

static void aat2870_led_brightness_set(struct led_classdev *led_cdev,
		enum led_brightness value)
{
	struct aat2870_device *adev = dev_get_drvdata(led_cdev->dev->parent);

	adev_dbg("%s\n", __func__);
	set_brightness_to_fixed(adev, CONVERT(value, LED_FULL, BRIGHTNESS_MAX));

	return;
}

static enum led_brightness aat2870_led_brightness_get(struct led_classdev *led_cdev)
{
	struct aat2870_device *adev = dev_get_drvdata(led_cdev->dev->parent);
	return CONVERT(adev->brightness, BRIGHTNESS_MAX, LED_FULL);
}

static int bl_set_intensity(struct backlight_device *bd)
{
	struct i2c_client *client = to_i2c_client(bd->dev.parent);
	struct aat2870_device *adev = i2c_get_clientdata(client);

	adev_dbg("%s\n", __func__);
	set_brightness_to_fixed(adev, bd->props.brightness);

	return 0;
}

static int bl_get_intensity(struct backlight_device *bd)
{
	struct i2c_client *client = to_i2c_client(bd->dev.parent);
	struct aat2870_device *adev = i2c_get_clientdata(client);
	return adev->brightness;
}

static int adev_notifier_chain_register(struct aat2870_device *adev,
		struct notifier_block *nb)
{
	int res = srcu_notifier_chain_register(&adev->als.level_update_notifier, nb);
	if (res == 0) {
		if (adev->als.registered_notifiers++ == 0) {
			schedule_delayed_work(&adev->als.store_als_stage1_work, 0);
		}
	}
	return res;
}

static int adev_notifier_chain_unregister(struct aat2870_device *adev,
		struct notifier_block *nb)
{
	int res = srcu_notifier_chain_unregister(&adev->als.level_update_notifier, nb);
	if (res == 0) {
		if (--adev->als.registered_notifiers == 0) {
			cancel_delayed_work(&adev->als.store_als_stage1_work);
		}
	}
	return res;
}

/*
 * Just set the mode, restart the als polling (with the updated polling interval)
 * and if mode is auto-brightness, add the notifier to adjust the brightness
 * if the als_level was updated
 */
static void set_mode(struct i2c_client *client, enum MODE mode)
{
	struct aat2870_device *adev = i2c_get_clientdata(client);
	struct als *als = &adev->als;
	bool mode_change = adev->mode != mode;

	adev_info("Setting mode: %s\n", mode_str[mode]);

	if (mode_change && adev->mode == MODE_AUTO) {
		adev_notifier_chain_unregister(adev, &als->set_brightness_nb);
	}
	adev->mode = mode;
	if (mode_change && mode == MODE_AUTO) {
		adev_notifier_chain_register(adev, &als->set_brightness_nb);
	}

	/*
	 * Always issue an initial brightness measure to ensure the registers
	 * are initialized correctly
	 */
	schedule_delayed_work(&adev->als.store_als_stage1_work, 0);

	return;
}


static ssize_t backlight_mode_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct aat2870_device *adev = dev_get_drvdata(dev);
	int o = 0;
	int i;

	o += snprintf(buf + o, PAGE_SIZE - o, "%s\n\n", mode_str[adev->mode]);
	for_array (i, mode_input_val) {
		o += snprintf(buf + o, PAGE_SIZE - o, "%s -> %s\n",
			mode_input_val[i].input,
			mode_str[mode_input_val[i].val]);
	}
	return o;
}

static ssize_t backlight_mode_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct aat2870_device *adev = dev_get_drvdata(dev);
	bool found = false;
	int i;

	for_array (i, mode_input_val) {
		if (strstarts(buf, mode_input_val[i].input)) {
			found = true;
			break;
		}
	}

	if (found) {
		mutex_lock(&adev->mutex);
		set_mode(adev->client, mode_input_val[i].val);
		mutex_unlock(&adev->mutex);
	} else {
		adev_err("value is not valid\n");
		return -EINVAL;
	}

	return count;
}

static ssize_t als_level_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct aat2870_device *adev = dev_get_drvdata(dev);
	struct als *als = &adev->als;
	uint32_t msecs_ago = jiffies_to_msecs(jiffies - adev->als.level_jif);
	uint32_t msecs_10 = (msecs_ago + 5) / 10 * 10;
	uint32_t msecs_100 = (msecs_ago + 50) / 100 * 100;
	uint32_t poll_ival = adev->als.polling_interval_ms[adev->mode];

	/* We use rounded values because of inaccuracies */
	msecs_ago = msecs_10 < 1000 ? msecs_10 : msecs_100;

	if (msecs_ago > poll_ival) {
		/*
		 * Schedule a work item if none is scheduled
		 * so that the als level is updated after the read
		 */
		schedule_delayed_work(&adev->als.store_als_stage1_work, 0);
	}

	return snprintf(buf, PAGE_SIZE, "0x%x\n"
		"last measure: %u ms ago\n"
		"poll interval: %u ms\n%s",
		als->level, msecs_ago, poll_ival,
		als->registered_notifiers ? "" :
			"Automatic poll is disabled as no notifier is registered.\n"
			"By reading this file you can trigger a manual poll once in the poll interval.\n"
		);
}

ssize_t als_brightness_levels_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct aat2870_device *adev = dev_get_drvdata(dev);
	int reg;
	int o = 0;

	for (reg = 0; reg < BRIGHTNESS_REGS; ++reg) {
		o += snprintf(buf + o, PAGE_SIZE - o,
			"%d%s", adev->als.brightness_levels[reg],
			reg != BRIGHTNESS_REGS - 1 ? " " : "\n\n");
	}

	o += snprintf(buf + o, PAGE_SIZE - o,
		"You can set all the als brightness levels here.\n"
		"You can also provide just the minimum and maximum brightness level.\n"
		"The intermediate brightness levels are then calculated automatically in\n"
		"an exponential way to match the logarithmic response of the human eye.\n"
		"The formula depends on the value of ld_offset.\n"
		"You can also omit the maximum level in which case %d is used\n"
		"NOTE: These brightness levels have to be in the interval [0, %d]\n",
		BRIGHTNESS_MAX, BRIGHTNESS_MAX);

	return o;
}

static ssize_t als_brightness_levels_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct aat2870_device *adev = dev_get_drvdata(dev);
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
			adev_err("Brightness value %d too high (> %d)", val, BRIGHTNESS_MAX);
			return -EINVAL;
		}

		vals[i] = val;
		o += read;

		if (count != o && buf[o] != ' ' && buf[o] != '\n')
			return -EINVAL;

		o += 1;
	}

	mutex_lock(&adev->mutex);
	if (i == 1) {
		/* one value given, calc brightness levels between this value and 127 */
		fill_als_brightness_levels(adev, vals[0], BRIGHTNESS_MAX);
	} else if (i == 2) {
		/* two vals given, calc brightness levels between those two values */
		fill_als_brightness_levels(adev, vals[0], vals[1]);
	} else if (i == BRIGHTNESS_REGS) {
		int j;
		for (j = 0; j < BRIGHTNESS_REGS; ++j) {
			adev->als.brightness_levels[j] = vals[j];
		}
	} else {
		return -EINVAL;
	}

	set_mode(adev->client, adev->mode);
	mutex_unlock(&adev->mutex);

	return count;
}

ssize_t onoff_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", gpio_get_value(LCD_CP_EN));
}

ssize_t onoff_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct aat2870_device *adev = dev_get_drvdata(dev);
	int i;
	bool found = false;

	for_array (i, onoff_input_val) {
		if (strstarts(buf, onoff_input_val[i].input)) {
			found = true;
			break;
		}
	}

	if (!found) {
		adev_err("%s is not a valid value\n", buf);
		return -EINVAL;
	}

	mutex_lock(&adev->mutex);
	switch (onoff_input_val[i].val) {
	case 0:
		bl_off(adev->client);
		i2c_write_reg(adev->client, REG_EN_LDO, LDO_DIS_ALL);
		gpio_set_value(LCD_CP_EN, 0);
		adev_info("onoff: off\n");
		break;
	case 1:
		gpio_set_value(LCD_CP_EN, 1);
		ldo_activate(adev->client);
		bl_on(adev->client);
		adev_info("onoff: on\n");
		break;
	}
	mutex_unlock(&adev->mutex);

	return count;
}

/* Used to highlight a active values */
static const char* hl[][2] = {
	{"", ""}, /* strings for inactive values */
	{"[", "]"}, /* strings for active values */
};

ssize_t als_gain_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aat2870_device *adev = dev_get_drvdata(dev);
	struct als *als = &adev->als;
	enum GAIN_MODE mode;
	int o = 0;

	o += snprintf(buf + o, PAGE_SIZE - o, "%s %d %d\n\n",
			gain_mode_input_str[als->gain_mode],
			als->gain_resistor,
			als->gain);

	o += snprintf(buf + o, PAGE_SIZE - o,
		"This file accepts the following format:\n"
		"<gain-mode> <resistor-idx> <gain>\n\n"
		"* gain-mode is one of {");

	for_enum (mode, GAIN_MODE) {
		bool mode_hl = mode == als->gain_mode;
		o += snprintf(buf + o, PAGE_SIZE - o,
			"%s%s%s%s",
			hl[mode_hl][0],
			gain_mode_input_str[mode],
			hl[mode_hl][1],
			mode != GAIN_MODE_LAST ? ", " : "},\n");
	}

	o += snprintf(buf + o, PAGE_SIZE - o,
		"* resistor-idx is an integer in the range [%d, %d],\n"
		"  which maps to the following resistors:\n",
		GAIN_RESISTOR_FIRST, GAIN_RESISTOR_LAST);

	for_enum (mode, GAIN_MODE) {
		enum GAIN_RESISTOR r;
		bool mode_hl = mode == als->gain_mode;
		o += snprintf(buf + o, PAGE_SIZE - o,
			"  - %s%s%s:\n    ",
			hl[mode_hl][0],
			gain_mode_str[mode],
			hl[mode_hl][1]);
		for_enum (r, GAIN_RESISTOR) {
			bool res_hl = mode_hl && r == als->gain_resistor;
			o += snprintf(buf + o, PAGE_SIZE - o,
				"%s%s%s%s",
				hl[res_hl][0],
				gain_resistor_str[mode][r],
				hl[res_hl][1],
				r != GAIN_RESISTOR_LAST ? ", " : "\n");
		}
	}

	o += snprintf(buf + o, PAGE_SIZE - o,
		"* gain is an integer in the range [%d, %d],\n"
		"  which is multiplied by %d.%02d to get a gain percentage"
		" (currently: %+d.%02d%%)\n",
		GAIN_MIN, GAIN_MAX,
		GAIN_MULT_PER_10000 / 100, GAIN_MULT_PER_10000 % 100,
		als->gain * GAIN_MULT_PER_10000 / 100,
		(als->gain * GAIN_MULT_PER_10000) % 100);

	return o;
}

ssize_t als_gain_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct aat2870_device *adev = dev_get_drvdata(dev);
	struct als *als = &adev->als;
	int tokens;
	char mode_str[5];
	int resistor;
	enum GAIN_MODE mode;
	int gain;

	tokens = sscanf(buf, "%5s %d %d", mode_str, &resistor, &gain);

	if (tokens < 3) {
		adev_err("Too few parameters for sysfs als_gain\n");
		return -EINVAL;
	}

	if (resistor < GAIN_RESISTOR_FIRST || resistor > GAIN_RESISTOR_LAST) {
		adev_err("Resistor idx not in range [%d, %d]",
			GAIN_RESISTOR_LAST, GAIN_RESISTOR_LAST);
		return -EINVAL;
	}

	if (gain < GAIN_MIN || gain > GAIN_MAX) {
		adev_err("Gain not in range [%d, %d]\n", GAIN_MIN, GAIN_MAX);
		return -EINVAL;
	}

	for_enum (mode, GAIN_MODE) {
		if (strstarts(gain_mode_input_str[mode], mode_str)) {
			break;
		}
	}

	if (mode > GAIN_MODE_LAST) {
		adev_err("Invalid gain mode %s\n", mode_str);
		return -EINVAL;
	}

	mutex_lock(&adev->mutex);
	als->gain_mode = mode;
	als->gain_resistor = resistor;
	als->gain = gain;
	mutex_unlock(&adev->mutex);

	return count;
}

/*
 * Attribute to set and show a bool with a description
 * and a function to extract the value from the device
 */
struct device_bool_attr {
	struct device_attribute attr;
	char *desc;
	void *(*val_fn)(struct device *, long);
	long val_data; /* passed as the second parameter to val_fn */
};

/*
 * Attribute to set and show a size_t in range min and max with a description
 * and a function to extract the value from the device
 */
struct device_size_t_attr {
	struct device_attribute attr;
	char *desc;
	size_t min;
	size_t max;
	void *(*val_fn)(struct device *, long);
	long val_data; /* passed as the second parameter to val_fn */
};

static ssize_t device_bool_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct device_bool_attr *dba =
		container_of(attr, struct device_bool_attr, attr);
	bool *val = dba->val_fn(dev, dba->val_data);

	return snprintf(buf, PAGE_SIZE, "%s\n\n%s\n", *val ? "true" : "false", dba->desc);
}

static ssize_t device_bool_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct device_bool_attr *dba =
		container_of(attr, struct device_bool_attr, attr);
	bool *val = dba->val_fn(dev, dba->val_data);
	unsigned long new;
	int result = kstrtoul(buf, 10, &new);

	if (result != 0) {
		if (strstarts(buf, "false"))
			new = 0;
		else if (strstarts(buf, "true"))
			new = 1;
		else
			return -EINVAL;
	}

	*val = !!new;
	return size;
}

static ssize_t device_size_t_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct device_size_t_attr *dsa =
		container_of(attr, struct device_size_t_attr, attr);
	size_t *val = dsa->val_fn(dev, dsa->val_data);

	return snprintf(buf, PAGE_SIZE, "%zu (min: %zu, max: %zu)\n\n%s\n",
		*val, dsa->min, dsa->max, dsa->desc);
}

static ssize_t device_size_t_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct device_size_t_attr *dsa =
		container_of(attr, struct device_size_t_attr, attr);
	size_t *val = dsa->val_fn(dev, dsa->val_data);
	unsigned long new;
	int result = kstrtoul(buf, 10, &new);

	if (result != 0 || new > dsa->max || new < dsa->min)
		return -EINVAL;

	*val = new;
	return size;
}

static void *offset_from_i2c_drvdata(struct device *dev, long data)
{
	char *adev = dev_get_drvdata(dev);
	return adev + data;
}

#define DEV_SIZE_T_ATTR(_name, _mode, _desc, _min, _max, _val_fn, _val_data) \
struct device_size_t_attr dev_attr_##_name = { \
	.attr = __ATTR(_name, _mode, device_size_t_show, device_size_t_store), \
	.desc = _desc, \
	.min = _min, \
	.max = _max, \
	.val_fn = _val_fn, \
	.val_data = _val_data, \
}

#define IDEV_SIZE_T_ATTR(_name, _mode, _desc, _min, _max, _val_data) \
	DEV_SIZE_T_ATTR(_name, _mode, _desc, _min, _max, \
		offset_from_i2c_drvdata, \
		offsetof(struct aat2870_device, _val_data))

#define DEV_BOOL_ATTR(_name, _mode, _desc, _val_fn, _val_data) \
struct device_bool_attr dev_attr_##_name = { \
	.attr = __ATTR(_name, _mode, device_bool_show, device_bool_store), \
	.desc = _desc, \
	.val_fn = _val_fn, \
	.val_data = _val_data, \
}

#define IDEV_BOOL_ATTR(_name, _mode, _desc, _val_data) \
	DEV_BOOL_ATTR(_name, _mode, _desc, \
		offset_from_i2c_drvdata, \
		offsetof(struct aat2870_device, _val_data))

static IDEV_SIZE_T_ATTR(als_adapt_brightness_delay, 0644,
	"Delay in ms before adapting the lcd brightnesss to a change in ambient brightness",
	0, 10000, als.adapt_brightness_delay_ms);
static DEVICE_ATTR(als_brightness_levels, 0644,
	als_brightness_levels_show, als_brightness_levels_store);
static DEVICE_ATTR(als_gain, 0644, als_gain_show, als_gain_store);
static DEVICE_ATTR(als_level, 0444, als_level_show, NULL);
static IDEV_SIZE_T_ATTR(als_polling_interval_auto, 0644,
	"Polling interval of the ambient light sensor in ms when in auto-brightness mode",
	250, 5000, als.polling_interval_ms[MODE_AUTO]);
static IDEV_SIZE_T_ATTR(als_polling_interval_fixed, 0644,
	"Polling interval of the ambient light sensor in ms when in fixed-brightness mode",
	250, 60000, als.polling_interval_ms[MODE_FIXED]);
static IDEV_SIZE_T_ATTR(als_polling_interval_screen_off, 0644,
	"Polling interval of the ambient light sensor in ms when screen is off",
	250, 600000, als.polling_interval_ms[MODE_SCREEN_OFF]);
static DEVICE_ATTR(backlight_mode, 0644, backlight_mode_show, backlight_mode_store);
static IDEV_BOOL_ATTR(fade_enabled, 0644,
	"Enable or disable fading.",
	fade.enabled);
static IDEV_SIZE_T_ATTR(fade_full_ival, 0644,
	"Interval for a fade in ms",
	50, 10000, fade.full_ival_ms);
static IDEV_SIZE_T_ATTR(fade_ld_offset, 0644,
	"An offset used for fade step time calculations.\n"
	"The lower this value is the longer is a fade step "
	"at a lower brightness level\n"
	"This value is also used in the calculations for als_brightness_levels.",
	LD_OFFSET_MIN, LD_OFFSET_MAX, fade.ld_offset);
static IDEV_SIZE_T_ATTR(fade_min_step_ival, 0644,
	"Minimum interval for fade steps in ms",
	10, 100, fade.min_step_ival_ms);
static DEVICE_ATTR(onoff, 0644, onoff_show, onoff_store);

static struct device_attribute *attrs[] = {
	&dev_attr_als_adapt_brightness_delay.attr,
	&dev_attr_als_brightness_levels,
	&dev_attr_als_gain,
	&dev_attr_als_level,
	&dev_attr_als_polling_interval_auto.attr,
	&dev_attr_als_polling_interval_fixed.attr,
	&dev_attr_als_polling_interval_screen_off.attr,
	&dev_attr_backlight_mode,
	&dev_attr_fade_enabled.attr,
	&dev_attr_fade_full_ival.attr,
	&dev_attr_fade_ld_offset.attr,
	&dev_attr_fade_min_step_ival.attr,
	&dev_attr_onoff,
};

static int aat2870_remove(struct i2c_client *client)
{
	struct aat2870_device *adev = i2c_get_clientdata(client);
	struct als *als = &adev->als;
	size_t i;

	unregister_early_suspend(&adev->early_suspend);
	srcu_cleanup_notifier_head(&als->level_update_notifier);

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

static int aat2870_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct aat2870_device *adev;
	struct als *als;
	struct fade *fade;
	size_t i;

	dev_info(&client->dev, "%s\n", __func__);

	aat2870_i2c_client = client;

	adev = kzalloc(sizeof(struct aat2870_device), GFP_KERNEL);

	if (adev == NULL) {
		dev_err(&client->dev, "fail alloc for aat2870_device\n");
		return 0;
	}
	als = &adev->als;
	fade = &adev->fade;

	mutex_init(&adev->mutex);
	mutex_init(&adev->fade.mutex);

	adev->bl_dev = backlight_device_register(AAT2870_I2C_BL_NAME,
		&client->dev, NULL, &bl_ops, &bl_props);

	adev->client = client;
	i2c_set_clientdata(client, adev);

	adev->state = WAKE_STATE;
	adev->mode = MODE_FIXED;

	if (gpio_request(LCD_CP_EN, "lcdcs") < 0) {
		return -ENOSYS;
	}

	gpio_direction_output(LCD_CP_EN, 1);

	if (gpio_request(HUB_PANEL_LCD_RESET_N, "lcd reset") < 0) {
		return -ENOSYS;
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
	adev->bl_resumed = 1;

	INIT_DELAYED_WORK(&als->store_als_stage1_work, store_als_stage1);
	INIT_DELAYED_WORK(&als->store_als_stage2_work, store_als_stage2);
	INIT_DELAYED_WORK(&fade->fade_set_brightness_work, fade_set_brightness);
	INIT_DELAYED_WORK(&fade->fade_brightness_start_work, fade_brightness_start);
	INIT_DELAYED_WORK(&adev->set_brightness_delayed_work, set_brightness_delayed);

	fade->min_step_ival_ms = 20;
	fade->full_ival_ms = 500;
	als->adapt_brightness_delay_ms = 500;
	fade->enabled = true;
	fade->ld_offset = LD_OFFSET_DFL;

	als->gain_mode = GAIN_MODE_HIGH;
	als->gain_resistor = GAIN_RESISTOR_2;
	als->gain = 7;

	fill_als_brightness_levels(adev, 0x06, 0x7F);
	memcpy(als->polling_interval_ms, default_als_polling_interval_ms,
		sizeof(als->polling_interval_ms));

	als->set_brightness_nb.notifier_call = set_brightness_notifier;
	srcu_init_notifier_head(&als->level_update_notifier);

	gpio_request(LCD_CP_EN, "LCD_CP_EN");
	set_mode(adev->client, MODE_FIXED);

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

MODULE_DESCRIPTION("AAT2870 Backlight Control");
MODULE_AUTHOR("Stefan Demharter, Yool-Je Cho <yoolje.cho@lge.com>");
MODULE_LICENSE("GPL");
