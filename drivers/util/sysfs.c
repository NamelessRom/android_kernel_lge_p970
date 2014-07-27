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

#include <linux/types.h>
#include <linux/util/sysfs.h>

#define warn(fmt, ...) switch (p->obj_type) { \
case OBJECT_TYPE_DEVICE: \
	dev_warn(container_of(p->kobj, struct device, kobj), "%s: " fmt, \
		p->val_attr->name, ## __VA_ARGS__); \
	break; \
default: \
	pr_warn("%s: %s: " fmt, kobject_name(p->kobj), p->val_attr->name, ## __VA_ARGS__); \
	break; \
}

static ssize_t bool_show(struct kobj_props *p, char *buf)
{
	REQUIRE_ATTR_TYPE(ATTR_TYPE_BOOL);

	return scnprintf(buf, PAGE_SIZE, "%s\n", *(bool *)p->val ? "true" : "false");
}

static ssize_t bool_store(struct kobj_props *p, const char *buf, size_t count)
{
	struct bool_attr *attr = &p->val_attr->attr.bool_attr;
	unsigned long new;
	bool i;
	int result = kstrtoul(buf, 10, &new);

	REQUIRE_ATTR_TYPE(ATTR_TYPE_BOOL);

	if (result != 0) {
		if (sysfs_streq(buf, "false"))
			i = false;
		else if (sysfs_streq(buf, "true"))
			i = true;
		else {
			warn("input is not a bool\n");
			return -EINVAL;
		}
	} else
		i = !!new;

	if (attr->update)
		return attr->update(p, i) ? -EINVAL : count;

	*(bool *)p->val = i;

	return count;
}

static ssize_t int_show(struct kobj_props *p, char *buf)
{
	REQUIRE_ATTR_TYPE(ATTR_TYPE_INT);

	return scnprintf(buf, PAGE_SIZE, "%d\n", *(int *)p->val);
}

static ssize_t int_min_max_show(struct kobj_props *p, char *buf)
{
	struct int_attr *attr = p->val_attr->attr.int_min_max_attr.int_attr;

	REQUIRE_ATTR_TYPE(ATTR_TYPE_INT_MIN_MAX);

	return scnprintf(buf, PAGE_SIZE, "%d %d\n", attr->min, attr->max);
}

static ssize_t int_store(struct kobj_props *p, const char *buf, size_t count)
{
	struct int_attr *attr = &p->val_attr->attr.int_attr;
	long new;
	int result = kstrtol(buf, 10, &new);

	REQUIRE_ATTR_TYPE(ATTR_TYPE_INT);

	if (result != 0) {
		warn("input is not an integer\n");
		return -EINVAL;
	} else if (new > attr->max || new < attr->min) {
		warn("input %d not in range [%d, %d]\n", (int)new, attr->min, attr->max);
		return -EINVAL;
	}

	if (attr->update)
		return attr->update(p, new) ? -EINVAL : count;

	*(int *)p->val = new;

	return count;
}

static ssize_t int_array_show(struct kobj_props *p, char *buf)
{
	struct int_array_attr *attr = &p->val_attr->attr.int_array_attr;
	int i;
	int o = 0;

	REQUIRE_ATTR_TYPE(ATTR_TYPE_INT_ARRAY);

	for (i = 0; i < attr->size; ++i) {
		o += scnprintf(buf + o, PAGE_SIZE - o, "%d%s", ((int *)p->val)[i],
			i != attr->size - 1 ? " " : "\n");
	}

	return o;
}

static ssize_t int_array_store(struct kobj_props *p, const char *buf, size_t count)
{
	struct int_array_attr *attr = &p->val_attr->attr.int_array_attr;
	int o = 0;
	int i;
	int vals[32];

	REQUIRE_ATTR_TYPE(ATTR_TYPE_INT_ARRAY);

	if (WARN_ON(attr->size > ARRAY_SIZE(vals))) {
		warn("BUG: array size of %d not supported (> %d)\n",
			attr->size, ARRAY_SIZE(vals));

		return -EINVAL;
	}

	for (i = 0; i < attr->size; ++i) {
		int read = 0;
		uint32_t val;
		sscanf(buf + o, "%d%n", &val, &read);

		if (read == 0)
			break;

		vals[i] = val;
		o += read;

		if (count != o && buf[o] != ' ' && buf[o] != '\n') {
			warn("format for array not understood\n");
			return -EINVAL;
		}

		o += 1;
	}

	if (attr->update)
		return attr->update(p, vals, i) ? -EINVAL : count;

	if (i < attr->size) {
		warn("not enough values (got: %d, need: %d)\n", i, attr->size);
		return -EINVAL;
	}

	for (i = 0; i < attr->size; ++i)
		((int *)p->val)[i] = vals[i];

	return count;
}

typedef size_t (*enum_str_fn)(struct kobj_props *p, int val, char *buf, size_t size);

static size_t enum_short_str_static(struct kobj_props *p, int val, char *buf, size_t size)
{
	struct enum_attr *attr = &p->val_attr->attr.enum_attr;

	REQUIRE_ATTR_TYPE(ATTR_TYPE_ENUM);

	if (!attr->enum_short_str_static)
		return scnprintf(buf, size, "%d", val);

	return scnprintf(buf, size, "%s", attr->enum_short_str_static[val]);
}

static size_t enum_long_str_static(struct kobj_props *p, int val, char *buf, size_t size)
{
	struct enum_attr *attr = &p->val_attr->attr.enum_attr;

	REQUIRE_ATTR_TYPE(ATTR_TYPE_ENUM);

	return scnprintf(buf, size, "%s",
		(attr->enum_long_str_static ? attr->enum_long_str_static[val] : ""));
}

static ssize_t enum_show(struct kobj_props *p, char *buf)
{
	struct enum_attr *attr = &p->val_attr->attr.enum_attr;
	int i;
	ssize_t o = 0;
	enum_str_fn enum_str[2] = {
		attr->enum_str[0] ? attr->enum_str[0] : enum_short_str_static,
		attr->enum_str[1] ? attr->enum_str[1] : enum_long_str_static,
	};
	int val = *(int *)p->val;

	REQUIRE_ATTR_TYPE(ATTR_TYPE_ENUM);

	BUG_ON(attr->max < attr->min);

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

static ssize_t enum_store(struct kobj_props *p, const char *buf, size_t count)
{
	struct enum_attr *attr = &p->val_attr->attr.enum_attr;
	int i;
	char b[128];
	int j;
	enum_str_fn enum_str[2] = {
		attr->enum_str[0] ? attr->enum_str[0] : enum_short_str_static,
		attr->enum_str[1] ? attr->enum_str[1] : enum_long_str_static,
	};

	REQUIRE_ATTR_TYPE(ATTR_TYPE_ENUM);

	b[sizeof(b) - 1] = '\0';

	for (j = 0; j < ARRAY_SIZE(enum_str); ++j) {
		for (i = attr->min; i <= attr->max; ++i) {
			enum_str[j](p, i, b, sizeof(b) - 1);

			if (sysfs_streq(buf, b)) {
				goto found;
			}
		}
	}

	warn("invalid enum value\n");
	return -EINVAL;

found:
	if (attr->update)
		return attr->update(p, i) ? -EINVAL : count;

	*(int *)p->val = i;

	return count;
}

static ssize_t info_show(struct kobj_props *p, char *buf)
{
	REQUIRE_ATTR_TYPE(ATTR_TYPE_INFO);

	return scnprintf(buf, PAGE_SIZE, "%s\n", p->val_attr->attr.info_attr.info);
}

static ssize_t bug_store(struct kobj_props *p, const char *buf, size_t count)
{
	warn("%s: BUG: sysfs input shouldn't be possible\n", p->val_attr->name);

	BUG();

	return -EINVAL;
}

static ssize_t (* const fn_show[])(struct kobj_props *p, char *buf) = {
	[ATTR_TYPE_BOOL] = bool_show,
	[ATTR_TYPE_ENUM] = enum_show,
	[ATTR_TYPE_INT] = int_show,
	[ATTR_TYPE_INT_ARRAY] = int_array_show,
	[ATTR_TYPE_INT_MIN_MAX] = int_min_max_show,
	[ATTR_TYPE_INFO] = info_show,
};

static ssize_t (* const fn_store[])(struct kobj_props *p,
		const char *buf, size_t count) = {
	[ATTR_TYPE_BOOL] = bool_store,
	[ATTR_TYPE_ENUM] = enum_store,
	[ATTR_TYPE_INT_ARRAY] = int_array_store,
	[ATTR_TYPE_INT] = int_store,
	[ATTR_TYPE_INFO] = bug_store,
	[ATTR_TYPE_INT_MIN_MAX] = bug_store,
};

static ssize_t val_show(struct kobject *kobj, enum OBJECT_TYPE obj_type,
		struct val_attr *attr, char *buf)
{
	struct kobj_props p = {
		.obj_type = obj_type,
		.kobj = kobj,
		.val_attr = attr,
		.val = attr->val_ptr ? attr->val_ptr(kobj, attr) : NULL,
	};

	return fn_show[attr->attr_type](&p, buf);
}

static ssize_t val_store(struct kobject *kobj, enum OBJECT_TYPE obj_type,
		struct val_attr *attr, const char *buf, size_t count)
{
	struct kobj_props p = {
		.obj_type = obj_type,
		.kobj = kobj,
		.val_attr = attr,
		.val = attr->val_ptr ? attr->val_ptr(kobj, attr) : NULL,
	};

	return fn_store[attr->attr_type](&p, buf, count);
}

ssize_t dev_val_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return val_show(&dev->kobj, OBJECT_TYPE_DEVICE,
		&container_of(attr, struct dev_val_attr, dev_attr)->val_attr, buf);
}
EXPORT_SYMBOL_GPL(dev_val_show);

ssize_t dev_val_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	return val_store(&dev->kobj, OBJECT_TYPE_DEVICE,
		&container_of(attr, struct dev_val_attr, dev_attr)->val_attr, buf, count);
}
EXPORT_SYMBOL_GPL(dev_val_store);

ssize_t kobj_val_show(struct kobject *kobj, struct kobj_attribute *attr,
		char *buf)
{
	return val_show(kobj, OBJECT_TYPE_KOBJECT,
		&container_of(attr, struct kobj_val_attr, kobj_attr)->val_attr, buf);
}
EXPORT_SYMBOL_GPL(kobj_val_show);

ssize_t kobj_val_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	return val_store(kobj, OBJECT_TYPE_KOBJECT,
		&container_of(attr, struct kobj_val_attr, kobj_attr)->val_attr, buf, count);
}
EXPORT_SYMBOL_GPL(kobj_val_store);

void *val_from_kobj_offset(struct kobject *kobj, struct val_attr *attr)
{
	return ((char *)kobj) + attr->data;
}
EXPORT_SYMBOL_GPL(val_from_kobj_offset);
