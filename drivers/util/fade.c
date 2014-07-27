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

#include <linux/util/fade.h>
#include <linux/util/ld.h>
#include <linux/util/sysfs.h>

#include <linux/device.h>

#define dbg(...) dev_dbg(fade->fade_props->parent, __VA_ARGS__)
#define info(...) dev_info(fade->fade_props->parent, __VA_ARGS__)
#define warn(...) dev_warn(fade->fade_props->parent, __VA_ARGS__)
#define err(...) dev_err(fade->fade_props->parent, __VA_ARGS__)

static uint32_t (*ld)(uint32_t) = util_ld;

/*
 * find next brightness s.t. fade_step_ms > min_step_ival_ms
 * or the target brightness is reached
 */
static uint32_t find_next_brightness(struct fade *fade)
{
	int32_t min_ival = msecs_to_jiffies(fade->min_step_ival_ms);
	int32_t full_ival = msecs_to_jiffies(fade->full_ival_ms);
	uint32_t start = fade->brightness_start;
	uint32_t cur = fade->brightness_next;
	uint32_t next = cur;
	uint32_t target = fade->brightness_target;
	int o = fade->ld_offset;
	/*
	 * fade_step_ms =
	 * (ld[next + o] - ld[cur + o]) / (ld[target + o] - ld[start + o]) * full_ival
	 */
	int32_t ld_next_o;

	if (start > target) {
		ld_next_o = ld(cur + o) -
			min_ival * (ld(start + o) - ld(target + o)) / full_ival;

		while (next != target && ld(next + o) > ld_next_o)
			--next;
	} else {
		ld_next_o = ld(cur + o) +
			min_ival * (ld(target + o) - ld(start + o)) / full_ival;

		while (next != target && ld(next + o) < ld_next_o)
			++next;
	}

	dbg("Fade: next %d ldo %d lim %d\n", next, ld(next + o), ld_next_o);

	return next;
}

/* Calculate the next fade step and schedule it (if necessary) */
static void fade_brightness_schedule(struct fade *fade)
{
	uint32_t start = fade->brightness_start;
	uint32_t cur = fade->brightness_next;
	uint32_t target = fade->brightness_target;
	/* If fade is not enabled immediately set target brightness */
	uint32_t next = target;
	uint32_t fade_step_ival = 0;
	int o = fade->ld_offset;

	if (target == cur) {
		/* target brightness reached, so stop */
		fade->state = FADE_STATE_STOPPED;
		return;
	}

	if (fade->enabled) {
		next = find_next_brightness(fade);

		if (target < start) {
			fade_step_ival = DIV_ROUND_CLOSEST(
				msecs_to_jiffies(fade->full_ival_ms) * (ld(cur + o) - ld(next + o)),
				(ld(start + o) - ld(target + o)));
		} else {
			fade_step_ival = DIV_ROUND_CLOSEST(
				msecs_to_jiffies(fade->full_ival_ms) * (ld(next + o) - ld(cur + o)),
				(ld(target + o) - ld(start + o)));
		}

		dbg("Fade step from %u to %u scheduled in %u ms\n",
			cur, next, jiffies_to_msecs(fade_step_ival));
	}

	fade->brightness_next = next;

	schedule_delayed_work(&fade->set_brightness, fade_step_ival);
}

/*
 * Set the brightness to the precalculated next brightness step
 * and schedule a new brightness step (if necessary)
 */
static void set_brightness(struct work_struct *ws)
{
	struct fade *fade =
		container_of(ws, struct fade, set_brightness.work);

	fade->fade_props->set_brightness_to(fade->fade_props, fade->brightness_next);

	mutex_lock(&fade->mutex);
	if (fade->state == FADE_STATE_IN_PROGRESS) {
		fade_brightness_schedule(fade);
	}
	mutex_unlock(&fade->mutex);
}

/* Start the brightness fade from fade->start to fade->target */
static void fade_brightness_start(struct work_struct *ws)
{
	struct fade *fade =
		container_of(ws, struct fade, fade_brightness_start.work);

	mutex_lock(&fade->mutex);
	if (fade->state == FADE_STATE_SCHEDULED) {
		fade->state = FADE_STATE_IN_PROGRESS;
		fade_brightness_schedule(fade);
	}
	mutex_unlock(&fade->mutex);
}

/* Stop any scheduled fade or fade in progress */
static void fade_stop_internal(struct fade *fade)
{
	if (fade->state != FADE_STATE_STOPPED) {
		dbg("Stopping fade\n");
		cancel_delayed_work(&fade->fade_brightness_start);
		cancel_delayed_work(&fade->set_brightness);
		fade->state = FADE_STATE_STOPPED;
	}
}

/*
 * Start a delayed fade from old brightness to new brightness
 * If a fade is still in progress, stop it if necessary or adapt it.
 */
void fade_brightness_delayed(struct fade *fade, uint32_t delay_ms,
	uint32_t from, uint32_t to)
{
	mutex_lock(&fade->mutex);
	if (from == to && fade->state == FADE_STATE_STOPPED) {
		/*
		 * nothing to do, as the target brightness is already reached
		 * and no fader is running
		 */
		goto out;
	}

	if (fade->state != FADE_STATE_STOPPED) {
		/* 1 = down, 0 = up */
		bool direction = fade->brightness_next > fade->brightness_target;
		bool new_direction = fade->brightness_next > to;
		if (direction == new_direction) {
			/*
			 * In that case the fader is already running in the right direction (or is scheduled),
			 * so just update start and target brightness
			 */
			fade->brightness_start = fade->brightness_next;
			fade->brightness_target = to;
			dbg("Setting new target brightness of running fade to %u\n",
				fade->brightness_target);
			goto out;
		}
		fade_stop_internal(fade);
	}

	info("%s brightness from %d to %d\n",
		fade->enabled ? "Fading" : "Setting", from, to);

	fade->brightness_target = to;
	fade->brightness_start = from;
	fade->brightness_next = from;
	fade->state = FADE_STATE_SCHEDULED;

	info("%s from %u to %u after %u ms (interval: %u ms)\n",
		fade->enabled ? "Starting fade" : "Setting brightness",
		from, to, delay_ms, fade->full_ival_ms);

	schedule_delayed_work(&fade->fade_brightness_start,
		msecs_to_jiffies(delay_ms));

out:
	mutex_unlock(&fade->mutex);
}
EXPORT_SYMBOL_GPL(fade_brightness_delayed);

void fade_stop(struct fade *fade)
{
	mutex_lock(&fade->mutex);
	fade_stop_internal(fade);
	mutex_unlock(&fade->mutex);
}
EXPORT_SYMBOL_GPL(fade_stop);

void fade_finish(struct fade *fade)
{
	mutex_lock(&fade->mutex);
	if (fade->state != FADE_STATE_STOPPED) {
		dbg("Setting brightness to target brightness\n");
		fade->fade_props->set_brightness_to(fade->fade_props, fade->brightness_target);
	}
	fade_stop_internal(fade);
	mutex_unlock(&fade->mutex);
}
EXPORT_SYMBOL_GPL(fade_finish);

static KOBJ_BOOL_ATTR(enabled, 0644, KOBJ_OFFSET(struct fade, enabled), NULL);

static KOBJ_INT_ATTR(full_ival, 0644, 50, 10000, KOBJ_OFFSET(struct fade, full_ival_ms), NULL);
static KOBJ_INT_MIN_MAX_ATTR(full_ival, 0444);

static KOBJ_INFO_ATTR(info, 0444,
	"Here you can control the kernel based fading for the parent device:\n"
	"\n"
	"ld_offset is used for the step time calculations.\n"
	"The lower this value is the longer is a fade step at a lower brightness level\n"
	"\n"
	"The fade interval is configured in full_ival in ms.\n"
	"The minimum interval for one fade step is configured in min_step_ival in ms.");

static KOBJ_INT_ATTR(ld_offset, 0644, LD_OFFSET_MIN, LD_OFFSET_MAX,
	KOBJ_OFFSET(struct fade, ld_offset), NULL);
static KOBJ_INT_MIN_MAX_ATTR(ld_offset, 0444);

static KOBJ_INT_ATTR(min_step_ival, 0644, 5, 100,
	KOBJ_OFFSET(struct fade, min_step_ival_ms), NULL);
static KOBJ_INT_MIN_MAX_ATTR(min_step_ival, 0444);

static struct attribute *attrs[] = {
	&kobj_val_attr_info.kobj_attr.attr,
	&kobj_val_attr_enabled.kobj_attr.attr,
	&kobj_val_attr_full_ival.kobj_attr.attr,
	&kobj_val_attr_full_ival_min_max.kobj_attr.attr,
	&kobj_val_attr_ld_offset.kobj_attr.attr,
	&kobj_val_attr_ld_offset_min_max.kobj_attr.attr,
	&kobj_val_attr_min_step_ival.kobj_attr.attr,
	&kobj_val_attr_min_step_ival_min_max.kobj_attr.attr,
	NULL,
};

static void fade_release(struct kobject *kobj)
{
	struct fade *fade = container_of(kobj, struct fade, kobj);

	dbg("fade released\n");

	if (fade->fade_props) {
		cancel_delayed_work_sync(&fade->set_brightness);
		cancel_delayed_work_sync(&fade->fade_brightness_start);

		fade->fade_props->on_release(fade->fade_props);
	}
}

static struct kobj_type kt = {
	.release = fade_release,
	.sysfs_ops = &kobj_sysfs_ops,
	.default_attrs = attrs,
};

int fade_init(struct fade *fade, struct fade_props *fade_props)
{
	int ret = kobject_init_and_add(&fade->kobj, &kt,
		&fade_props->parent->kobj, "fade");

	if (ret) {
		dev_err(fade_props->parent, "Couldn't create fade object. Error %d\n", ret);
		kobject_put(&fade->kobj);
		return ret;
	}

	fade->fade_props = fade_props;
	fade->min_step_ival_ms = 10;
	fade->full_ival_ms = 400;
	fade->enabled = true;
	fade->ld_offset = LD_OFFSET_DFL;

	INIT_DELAYED_WORK(&fade->set_brightness, set_brightness);
	INIT_DELAYED_WORK(&fade->fade_brightness_start, fade_brightness_start);

	mutex_init(&fade->mutex);
	fade->ld_offset = LD_OFFSET_DFL;

	kobject_uevent(&fade->kobj, KOBJ_ADD);

	return 0;
}
EXPORT_SYMBOL_GPL(fade_init);
