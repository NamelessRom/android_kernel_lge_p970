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

#include "aat2870_bl_sysfs.h"
#include "aat2870_bl_util.h"

#define REQUIRE_ATTR_TYPE(x) BUG_ON(p->attr->attr_type != x)

static ssize_t bool_show(struct kobj_props *p, char *buf)
{
	REQUIRE_ATTR_TYPE(ATTR_TYPE_BOOL);

	return scnprintf(buf, PAGE_SIZE, "%s\n", *(bool *)p->val ? "true" : "false");
}

static ssize_t bool_store(struct kobj_props *p, const char *buf, size_t size)
{
	struct bool_attr *attr = container_of(p->attr, struct bool_attr, val_attr);
	unsigned long new;
	bool i;
	int result = kstrtoul(buf, 10, &new);

	REQUIRE_ATTR_TYPE(ATTR_TYPE_BOOL);

	if (result != 0) {
		if (sysfs_streq(buf, "false"))
			i = false;
		else if (sysfs_streq(buf, "true"))
			i = true;
		else
			return -EINVAL;
	} else
		i = !!new;

	if (attr->val_attr.update)
		attr->val_attr.update(p, &i);
	else
		*(bool *)p->val = i;

	return size;
}

static ssize_t int_show(struct kobj_props *p, char *buf)
{
	REQUIRE_ATTR_TYPE(ATTR_TYPE_INT);

	return scnprintf(buf, PAGE_SIZE, "%d\n", *(int *)p->val);
}

static ssize_t int_store(struct kobj_props *p, const char *buf, size_t size)
{
	struct int_attr *attr = container_of(p->attr, struct int_attr, val_attr);
	long new;
	int result = kstrtol(buf, 10, &new);

	REQUIRE_ATTR_TYPE(ATTR_TYPE_INT);

	if (result != 0 || new > attr->max || new < attr->min) {
		return -EINVAL;
	}

	if (attr->val_attr.update)
		attr->val_attr.update(p, &new);
	else
		*(int *)p->val = new;

	return size;
}

typedef size_t (*enum_str_fn)(struct kobj_props *p, int val, char *buf, size_t size);

static size_t enum_short_str_static(struct kobj_props *p, int val, char *buf, size_t size)
{
	struct enum_attr *attr = container_of(p->attr, struct enum_attr, val_attr);

	REQUIRE_ATTR_TYPE(ATTR_TYPE_ENUM);

	if (!attr->enum_str_static)
		return scnprintf(buf, size, "%d", val);

	return scnprintf(buf, size, "%s", attr->enum_str_static[val]);
}

static size_t enum_long_str_static(struct kobj_props *p, int val, char *buf, size_t size)
{
	struct enum_attr *attr = container_of(p->attr, struct enum_attr, val_attr);

	REQUIRE_ATTR_TYPE(ATTR_TYPE_ENUM);

	return scnprintf(buf, size, "%s",
		(attr->enum_long_str_static ? attr->enum_long_str_static[val] : ""));
}

static ssize_t enum_show(struct kobj_props *p, char *buf)
{
	struct enum_attr *attr = container_of(p->attr, struct enum_attr, val_attr);
	int i;
	ssize_t o = 0;
	enum_str_fn enum_str[2] = {
		attr->enum_str[0] ? attr->enum_str[0] : enum_short_str_static,
		attr->enum_str[1] ? attr->enum_str[1] : enum_long_str_static,
	};
	int val = *(int *)p->val;

	REQUIRE_ATTR_TYPE(ATTR_TYPE_ENUM);

	for (i = attr->min; i <= attr->max; ++i) {
		bool hl = val == i;

		o += scnprintf(buf + o, PAGE_SIZE - o, hl ? "[" : " ");
		o += enum_str[0](p, i, buf + o, PAGE_SIZE - o),
		o += scnprintf(buf + o, PAGE_SIZE - o, hl ? "] " : "  ");
		o += enum_str[1](p, i, buf + o, PAGE_SIZE - o);
		o += scnprintf(buf + o, PAGE_SIZE - o, "\n");
	}

	return o;
}

static ssize_t enum_store(struct kobj_props *p, const char *buf, size_t size)
{
	struct enum_attr *attr = container_of(p->attr, struct enum_attr, val_attr);
	int i;
	char b[128];
	int j;
	enum_str_fn enum_str[2] = {
		attr->enum_str[0] ? attr->enum_str[0] : enum_short_str_static,
		attr->enum_str[1] ? attr->enum_str[1] : enum_long_str_static,
	};

	REQUIRE_ATTR_TYPE(ATTR_TYPE_ENUM);

	b[sizeof(b) - 1] = '\0';

	for_array (j, attr->enum_str) {
		for (i = attr->min; i <= attr->max; ++i) {
			enum_str[j](p, i, b, sizeof(b) - 1);
			if (sysfs_streq(buf, b)) {
				goto found;
			}
		}
	}

	return -EINVAL;

found:
	if (attr->val_attr.update)
		attr->val_attr.update(p, &i);
	else
		*(int *)p->val = i;

	return size;
}

static ssize_t info_show(struct kobj_props *p, char *buf)
{
	struct info_attr *attr = container_of(p->attr, struct info_attr, val_attr);

	REQUIRE_ATTR_TYPE(ATTR_TYPE_INFO);

	return scnprintf(buf, PAGE_SIZE, "%s\n", attr->info);
}

void *min_int(struct kobject *kobj, struct val_attr *attr)
{
	struct int_attr *a =
		container_of(attr, struct int_attr, min_attr);

	BUG_ON(attr->attr_type != ATTR_TYPE_INT);

	return &a->min;
}

void *max_int(struct kobject *kobj, struct val_attr *attr)
{
	struct int_attr *a =
		container_of(attr, struct int_attr, max_attr);

	BUG_ON(attr->attr_type != ATTR_TYPE_INT);

	return &a->max;
}

ssize_t info_store(struct kobj_props *p, const char *buf, size_t size)
{
	BUG();
}

static ssize_t (* const fn_show[])(struct kobj_props *p, char *buf) = {
	[ATTR_TYPE_BOOL] = bool_show,
	[ATTR_TYPE_ENUM] = enum_show,
	[ATTR_TYPE_INT] = int_show,
	[ATTR_TYPE_INFO] = info_show,
};

static ssize_t (* const fn_store[])(struct kobj_props *p,
		const char *buf, size_t size) = {
	[ATTR_TYPE_BOOL] = bool_store,
	[ATTR_TYPE_ENUM] = enum_store,
	[ATTR_TYPE_INT] = int_store,
	[ATTR_TYPE_INFO] = info_store,
};

static ssize_t val_show(struct kobject *kobj, struct val_attr *attr, char *buf)
{
	struct kobj_props p = {
		.kobj = kobj,
		.attr = attr,
		.val = attr->val_ptr ? attr->val_ptr(kobj, attr) : NULL,
	};

	return fn_show[attr->attr_type](&p, buf);
}

static ssize_t val_store(struct kobject *kobj, struct val_attr *attr,
		const char *buf, size_t size)
{
	struct kobj_props p = {
		.kobj = kobj,
		.attr = attr,
		.val = attr->val_ptr ? attr->val_ptr(kobj, attr) : NULL,
	};

	return fn_store[attr->attr_type](&p, buf, size);
}

ssize_t dev_val_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return val_show(&dev->kobj,
		container_of(attr, struct val_attr, attr.dev_attr), buf);
}

ssize_t dev_val_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t size)
{
	return val_store(&dev->kobj,
		container_of(attr, struct val_attr, attr.dev_attr), buf, size);
}

ssize_t kobj_val_show(struct kobject *kobj, struct kobj_attribute *attr,
		char *buf)
{
	return val_show(kobj,
		container_of(attr, struct val_attr, attr.kobj_attr), buf);
}

ssize_t kobj_val_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t size)
{
	return val_store(kobj,
		container_of(attr, struct val_attr, attr.kobj_attr), buf, size);
}

void *val_from_kobj_offset(struct kobject *kobj, struct val_attr *attr)
{
	return ((char *)kobj) + attr->data;
}

