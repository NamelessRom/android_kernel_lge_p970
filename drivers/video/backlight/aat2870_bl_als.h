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

#ifndef __AAT2870_BL_ALS_H__
#define __AAT2870_BL_ALS_H__

#include "aat2870_bl.h"

#include <linux/notifier.h>
#include <linux/workqueue.h>
#include <linux/i2c.h>

#define ALS_IVAL_MIN 250 /* ms */
#define ALS_IVAL_MAX 600000 /* ms */

#define ALS_LVL_MAX 15

#define GAIN_MULT_PER_10000 625

enum GAIN_MODE {
	GAIN_MODE_LOW,
	GAIN_MODE_HIGH,
	GAIN_MODE_AUTO,

	GAIN_MODE_MIN = GAIN_MODE_LOW,
	GAIN_MODE_MAX = GAIN_MODE_AUTO,
};

enum GAIN_RESISTOR {
	GAIN_RESISTOR_0,
	GAIN_RESISTOR_1,
	GAIN_RESISTOR_2,
	GAIN_RESISTOR_3,

	GAIN_RESISTOR_MIN = GAIN_RESISTOR_0,
	GAIN_RESISTOR_MAX = GAIN_RESISTOR_3,
};

enum GAIN_PERCENTAGE {
	GAIN_PERCENTAGE_M_50 = -8, /* -50 % */
	GAIN_PERCENTAGE_43_75 = 7, /* +43.75 % */

	GAIN_PERCENTAGE_MIN = GAIN_PERCENTAGE_M_50,
	GAIN_PERCENTAGE_MAX = GAIN_PERCENTAGE_43_75,
};

struct als {
	struct i2c_client *client;

	/* the 16 brightness levels for the 16 als levels (0-127) */
	u8 brightness_levels[BRIGHTNESS_REGS];
	/* the last measured als level (0-15) */
	u8 level;
	/* the time of the last als measure */
	unsigned long level_jif;
	size_t polling_interval_ms;
	int ld_offset;

	enum GAIN_MODE gain_mode;
	enum GAIN_RESISTOR gain_resistor;
	enum GAIN_PERCENTAGE gain;

	struct delayed_work update_als_level_stage1;
	struct delayed_work update_als_level_stage2;
	bool measure_running;

	/* delay to adapt lcd brightness after a change in ambient brightness */
	size_t adapt_brightness_delay_ms;

	/* notifier for als level updates */
	struct srcu_notifier_head level_update_notifier;
	size_t listeners;

	struct kobject kobj;
	struct mutex mutex;

	/*
	 * Set upon standby and unset upon resume to ensure we are disabling the
	 * gpio again if we enabled it in standby
	 */
	bool stay_off;
};

int als_add_listener(struct als *als, struct notifier_block *listener);
int als_remove_listener(struct als *als, struct notifier_block *listener);

int als_init(struct als *als, struct i2c_client *client);
void als_cleanup(struct als *als);
void als_stay_off(struct als *als, bool stay_off);

#endif /*__AAT2870_BL_ALS_H__ */
