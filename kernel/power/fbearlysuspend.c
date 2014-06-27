/* kernel/power/fbearlysuspend.c
 *
 * Copyright (C) 2005-2008 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/earlysuspend.h>
#include <linux/module.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/err.h>

#include "power.h"

static int fb_delay = 0;
static wait_queue_head_t fb_state_wq;
static DEFINE_SPINLOCK(fb_state_lock);
static enum {
	FB_STATE_STOPPED_DRAWING,
	FB_STATE_REQUEST_STOP_DRAWING,
	FB_STATE_DRAWING_OK,
} fb_state;

// prime@sdcmicro.com Temporary - Exports the SurfaceFlinger DRAW state [START]
int get_fb_state(void)
{
	return fb_state;
}
// prime@sdcmicro.com Temporary - Exports the SurfaceFlinger DRAW state [END]

/* tell userspace to stop drawing, wait for it to stop */
static void stop_drawing_early_suspend(struct early_suspend *h)
{
	int ret;
	unsigned long irq_flags;

	if(fb_delay)
		msleep(fb_delay);

	spin_lock_irqsave(&fb_state_lock, irq_flags);
	fb_state = FB_STATE_REQUEST_STOP_DRAWING;
	spin_unlock_irqrestore(&fb_state_lock, irq_flags);

	wake_up_all(&fb_state_wq);
	ret = wait_event_timeout(fb_state_wq,
				 fb_state == FB_STATE_STOPPED_DRAWING,
				 HZ);
	if (unlikely(fb_state != FB_STATE_STOPPED_DRAWING))
		pr_warning("stop_drawing_early_suspend: timeout waiting for "
			   "userspace to stop drawing\n");
}

/* tell userspace to start drawing */
static void start_drawing_late_resume(struct early_suspend *h)
{
	unsigned long irq_flags;

	spin_lock_irqsave(&fb_state_lock, irq_flags);
	fb_state = FB_STATE_DRAWING_OK;
	spin_unlock_irqrestore(&fb_state_lock, irq_flags);
	wake_up(&fb_state_wq);
}

static struct early_suspend stop_drawing_early_suspend_desc = {
	.level = EARLY_SUSPEND_LEVEL_STOP_DRAWING,
	.suspend = stop_drawing_early_suspend,
	.resume = start_drawing_late_resume,
};

static ssize_t wait_for_fb_sleep_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	char *s = buf;
	int ret;

	ret = wait_event_interruptible(fb_state_wq,
				       fb_state != FB_STATE_DRAWING_OK);
	if (ret && fb_state == FB_STATE_DRAWING_OK)
		return ret;
	else
		s += sprintf(buf, "sleeping");
	return s - buf;
}

static ssize_t wait_for_fb_wake_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	char *s = buf;
	int ret;
	unsigned long irq_flags;

	spin_lock_irqsave(&fb_state_lock, irq_flags);
	if (fb_state == FB_STATE_REQUEST_STOP_DRAWING) {
		fb_state = FB_STATE_STOPPED_DRAWING;
		wake_up(&fb_state_wq);
	}
	spin_unlock_irqrestore(&fb_state_lock, irq_flags);

	ret = wait_event_interruptible(fb_state_wq,
				       fb_state == FB_STATE_DRAWING_OK);
	if (ret && fb_state != FB_STATE_DRAWING_OK)
		return ret;
	else
		s += sprintf(buf, "awake");

	return s - buf;
}

static ssize_t fbdelay_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d", fb_delay);
}

static ssize_t fb_delay_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d", fb_delay);
}

static ssize_t fb_delay_store(struct kobject *kobj,
			     struct kobj_attribute *attr,
			     const char *buf, size_t n)
{
	int val;
	if (sscanf(buf, "%d", &val) != 1)
		return -EINVAL;

	if(val > 1000)
		val = 1000;
	if(val < 0)
		val = 0;
	
	fb_delay = val;
	
	return n;
}

#define power_ro_attr(_name) \
static struct kobj_attribute _name##_attr = {	\
	.attr	= {				\
		.name = __stringify(_name),	\
		.mode = 0444,			\
	},					\
	.show	= _name##_show,			\
	.store	= NULL,		\
}

power_ro_attr(wait_for_fb_sleep);
power_ro_attr(wait_for_fb_wake);
power_attr(fb_delay);

static struct attribute *g[] = {
	&wait_for_fb_sleep_attr.attr,
	&wait_for_fb_wake_attr.attr,
	&fb_delay_attr.attr,
	NULL,
};

static struct attribute_group attr_group = {
	.attrs = g,
};

static int __init android_power_init(void)
{
	int ret;

	init_waitqueue_head(&fb_state_wq);
	fb_state = FB_STATE_DRAWING_OK;

	ret = sysfs_create_group(power_kobj, &attr_group);
	if (ret) {
		pr_err("android_power_init: sysfs_create_group failed\n");
		return ret;
	}

	register_early_suspend(&stop_drawing_early_suspend_desc);
	return 0;
}

static void  __exit android_power_exit(void)
{
	unregister_early_suspend(&stop_drawing_early_suspend_desc);
	sysfs_remove_group(power_kobj, &attr_group);
}

module_init(android_power_init);
module_exit(android_power_exit);

