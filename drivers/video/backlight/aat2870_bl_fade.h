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

#ifndef __AAT2870_FADE_H__
#define __AAT2870_FADE_H__

#include <linux/kobject.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>

struct device;

enum FADE_STATE {
	FADE_STATE_STOPPED,
	FADE_STATE_SCHEDULED,
	FADE_STATE_IN_PROGRESS,
};

struct fade {
	bool enabled;
	uint32_t brightness_target;
	uint32_t brightness_next;
	uint32_t brightness_start;

	uint32_t brightness_min;
	uint32_t brightness_max;

	/* fade in progress or scheduled */
	enum FADE_STATE state;
	/* minimum interval per fade step */
	size_t min_step_ival_ms;
	/* interval for the full fade */
	size_t full_ival_ms;
	/* an offset used in calculations with the logarithm */
	size_t ld_offset;

	struct kobject kobj;

	struct mutex mutex;

	struct delayed_work set_brightness;
	struct delayed_work fade_brightness_start;

	void (*set_brightness_to)(struct fade *fade, uint32_t brightness);

	struct device *dev;
};

void fade_stop(struct fade *fade);
void fade_brightness_delayed(struct fade *fade, uint32_t delay_ms,
	uint32_t from, uint32_t to);

int fade_init(struct fade *fade, struct device *dev);
void fade_cleanup(struct fade *fade);

#endif /* __AAT2870_FADE_H__ */
