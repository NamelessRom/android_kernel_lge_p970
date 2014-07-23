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

ssize_t bool_show(struct kobject *kobj, struct bool_attr *attr,
		bool *val, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", *val ? "true" : "false");
}

ssize_t bool_store(struct kobject *kobj, struct bool_attr *attr,
		bool *val, const char* buf, size_t size)
{
	unsigned long new;
	bool i;
	int result = kstrtoul(buf, 10, &new);

	if (result != 0) {
		if (sysfs_streq(buf, "false"))
			i = false;
		else if (sysfs_streq(buf, "true"))
			i = true;
		else
			return -EINVAL;
	} else
		i = !!new;

	if (attr->update)
		attr->update(kobj, val, i);
	else
		*val = i;

	return size;
}

ssize_t int_show(struct kobject *kobj, struct int_attr *attr,
		int *val, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", *val);
}

ssize_t int_store(struct kobject *kobj, struct int_attr *attr,
		int *val, const char *buf, size_t size)
{
	long new;
	int result = kstrtol(buf, 10, &new);

	if (result != 0 || new > attr->max || new < attr->min) {
		return -EINVAL;
	}

	if (attr->update)
		attr->update(kobj, val, new);
	else
		*val = new;

	return size;
}

size_t enum_str_static(struct kobject *kobj, struct enum_attr *attr,
		int val, char *buf, size_t size)
{
	return scnprintf(buf, size, "%s", attr->enum_str_static[val]);
}

size_t enum_long_str_static(struct kobject *kobj, struct enum_attr *attr,
		int val, char *buf, size_t size)
{
	return scnprintf(buf, size, "%s",
		(attr->enum_long_str_static ? attr->enum_long_str_static[val] : ""));
}

ssize_t enum_show(struct kobject *kobj, struct enum_attr *attr,
		int *val, char *buf)
{
	int i;
	ssize_t o = 0;

	for (i = attr->min; i <= attr->max; ++i) {
		bool hl = *val == i;

		o += scnprintf(buf + o, PAGE_SIZE - o, hl ? "[" : " ");
		o += attr->enum_str[0](kobj, attr, i, buf + o, PAGE_SIZE - o),
		o += scnprintf(buf + o, PAGE_SIZE - o, hl ? "] " : "  ");
		o += attr->enum_str[1](kobj, attr, i, buf + o, PAGE_SIZE - o);
		o += scnprintf(buf + o, PAGE_SIZE - o, "\n");
	}

	return o;
}

ssize_t enum_store(struct kobject *kobj, struct enum_attr *attr,
		int *val, const char *buf, size_t size)
{
	int i;
	char b[128];
	int j;

	b[sizeof(b) - 2] = '\0';

	for_array (j, attr->enum_str) {
		for (i = attr->min; i <= attr->max; ++i) {
			(attr->enum_str[j])(kobj, attr, i, b, sizeof(b) - 1);
			if (sysfs_streq(buf, b)) {
				goto found;
			}
		}
	}

	return -EINVAL;

found:

	if (attr->update)
		attr->update(kobj, val, i);
	else
		*val = i;

	return size;
}

ssize_t info_show(struct kobject *kobj, struct info_attr *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", attr->info);
}

size_t enum_int_str(struct kobject *kobj, struct enum_attr *attr,
		int val, char *buf, size_t size)
{
	return scnprintf(buf, size, "%d", val);
}


ssize_t dev_bool_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct bool_attr *a =
		container_of(attr, struct bool_attr, attr.dev_attr);

	return bool_show(&dev->kobj, a, a->val_ptr(&dev->kobj, &a->data), buf);
}

ssize_t dev_enum_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct enum_attr *a =
		container_of(attr, struct enum_attr, attr.dev_attr);

	return enum_show(&dev->kobj, a, a->val_ptr(&dev->kobj, &a->data), buf);
}

ssize_t dev_int_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct int_attr *a =
		container_of(attr, struct int_attr, attr.dev_attr);

	return int_show(&dev->kobj, a, a->val_ptr(&dev->kobj, &a->data), buf);
}

ssize_t dev_min_int_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct int_attr *a =
		container_of(attr, struct int_attr, min_attr.dev_attr);

	return int_show(&dev->kobj, a, &a->min, buf);
}

ssize_t dev_max_int_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct int_attr *a =
		container_of(attr, struct int_attr, max_attr.dev_attr);

	return int_show(&dev->kobj, a, &a->max, buf);
}

ssize_t dev_info_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct info_attr *a =
		container_of(attr, struct info_attr, attr.dev_attr);

	return info_show(&dev->kobj, a, buf);
}

ssize_t dev_bool_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t size)
{
	struct bool_attr *a =
		container_of(attr, struct bool_attr, attr.dev_attr);

	return bool_store(&dev->kobj, a, a->val_ptr(&dev->kobj, &a->data), buf, size);
}

ssize_t dev_enum_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t size)
{
	struct enum_attr *a =
		container_of(attr, struct enum_attr, attr.dev_attr);

	return enum_store(&dev->kobj, a, a->val_ptr(&dev->kobj, &a->data), buf, size);
}

ssize_t dev_int_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t size)
{
	struct int_attr *a =
		container_of(attr, struct int_attr, attr.dev_attr);

	return int_store(&dev->kobj, a, a->val_ptr(&dev->kobj, &a->data), buf, size);
}


ssize_t kobj_bool_show(struct kobject *kobj, struct kobj_attribute *attr,
		char *buf)
{
	struct bool_attr *a =
		container_of(attr, struct bool_attr, attr.kobj_attr);

	return bool_show(kobj, a, a->val_ptr(kobj, &a->data), buf);
}

ssize_t kobj_enum_show(struct kobject *kobj, struct kobj_attribute *attr,
		char *buf)
{
	struct enum_attr *a =
		container_of(attr, struct enum_attr, attr.kobj_attr);

	return enum_show(kobj, a, a->val_ptr(kobj, &a->data), buf);
}

ssize_t kobj_int_show(struct kobject *kobj, struct kobj_attribute *attr,
		char *buf)
{
	struct int_attr *a =
		container_of(attr, struct int_attr, attr.kobj_attr);

	return int_show(kobj, a, a->val_ptr(kobj, &a->data), buf);
}

ssize_t kobj_min_int_show(struct kobject *kobj, struct kobj_attribute *attr,
		char *buf)
{
	struct int_attr *a =
		container_of(attr, struct int_attr, min_attr.kobj_attr);

	return int_show(kobj, a, &a->min, buf);
}

ssize_t kobj_max_int_show(struct kobject *kobj, struct kobj_attribute *attr,
		char *buf)
{
	struct int_attr *a =
		container_of(attr, struct int_attr, max_attr.kobj_attr);

	return int_show(kobj, a, &a->max, buf);
}

ssize_t kobj_info_show(struct kobject *kobj, struct kobj_attribute *attr,
		char *buf)
{
	struct info_attr *a =
		container_of(attr, struct info_attr, attr.kobj_attr);

	return info_show(kobj, a, buf);
}

ssize_t kobj_bool_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t size)
{
	struct bool_attr *a =
		container_of(attr, struct bool_attr, attr.kobj_attr);

	return bool_store(kobj, a, a->val_ptr(kobj, &a->data), buf, size);
}

ssize_t kobj_enum_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t size)
{
	struct enum_attr *a =
		container_of(attr, struct enum_attr, attr.kobj_attr);

	return enum_store(kobj, a, a->val_ptr(kobj, &a->data), buf, size);
}

ssize_t kobj_int_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t size)
{
	struct int_attr *a =
		container_of(attr, struct int_attr, attr.kobj_attr);

	return int_store(kobj, a, a->val_ptr(kobj, &a->data), buf, size);
}
