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

#ifndef __AAT2870_BL_SYSFS_H__
#define __AAT2870_BL_SYSFS_H__

#include <linux/device.h>

union attr {
	struct kobj_attribute kobj_attr;
	struct device_attribute dev_attr;
};

struct bool_attr {
	union attr attr;

	long data;
	bool *(*val_ptr)(struct kobject *kobj, long *data);
	void (*update)(struct kobject *kobj, bool *val, bool new_val);
};

struct int_attr {
	union attr attr;
	union attr min_attr;
	union attr max_attr;

	int min;
	int max;

	long data;
	int *(*val_ptr)(struct kobject *kobj, long *data);
	void (*update)(struct kobject *kobj, int *val, int new_val);
};

struct enum_attr {
	union attr attr;

	int min;
	int max;
	const char **enum_str_static;
	const char **enum_long_str_static;
	size_t (*enum_str[2])(struct kobject *kobj, struct enum_attr *attr,
			int val, char *buf, size_t size);

	long data;
	int *(*val_ptr)(struct kobject *kobj, long *data);
	void (*update)(struct kobject *kobj, int *val, int new_val);
};

size_t enum_str_static(struct kobject *kobj, struct enum_attr *attr,
		int val, char *buf, size_t size);
size_t enum_long_str_static(struct kobject *kobj, struct enum_attr *attr,
		int val, char *buf, size_t size);
size_t enum_int_str(struct kobject *kobj, struct enum_attr *attr,
		int val, char *buf, size_t size);

struct info_attr {
	union attr attr;

	const char *info;
};

/* General helper functions with the actual logic */
ssize_t bool_show(struct kobject *kobj, struct bool_attr *attr,
		bool *val, char *buf);
ssize_t bool_store(struct kobject *kobj, struct bool_attr *attr,
		bool *val, const char* buf, size_t size);
ssize_t enum_show(struct kobject *kobj, struct enum_attr *attr,
		int *val, char *buf);
ssize_t enum_store(struct kobject *kobj, struct enum_attr *attr,
		int *val, const char *buf, size_t size);
ssize_t info_show(struct kobject *kobj, struct info_attr *attr,
		char *buf);
ssize_t int_show(struct kobject *kobj, struct int_attr *attr,
		int *val, char *buf);
ssize_t int_store(struct kobject *kobj, struct int_attr *attr,
		int *val, const char *buf, size_t size);

/* Wrapper functions for `dev` which utilize the helper functions above */
ssize_t dev_bool_show(struct device *dev, struct device_attribute *attr,
		char *buf);
ssize_t dev_bool_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t size);
ssize_t dev_enum_show(struct device *dev, struct device_attribute *attr,
		char *buf);
ssize_t dev_enum_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t size);
ssize_t dev_info_show(struct device *dev, struct device_attribute *attr,
		char *buf);
ssize_t dev_int_show(struct device *dev, struct device_attribute *attr,
		char *buf);
ssize_t dev_min_int_show(struct device *dev, struct device_attribute *attr,
		char *buf);
ssize_t dev_max_int_show(struct device *dev, struct device_attribute *attr,
		char *buf);
ssize_t dev_int_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t size);

/* Wrapper functions for `kobj` which utilize the helper functions above */
ssize_t kobj_bool_show(struct kobject *kobj, struct kobj_attribute *attr,
		char *buf);
ssize_t kobj_bool_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t size);
ssize_t kobj_enum_show(struct kobject *kobj, struct kobj_attribute *attr,
		char *buf);
ssize_t kobj_enum_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t size);
ssize_t kobj_info_show(struct kobject *kobj, struct kobj_attribute *attr,
		char *buf);
ssize_t kobj_int_show(struct kobject *kobj, struct kobj_attribute *attr,
		char *buf);
ssize_t kobj_min_int_show(struct kobject *kobj, struct kobj_attribute *attr,
		char *buf);
ssize_t kobj_max_int_show(struct kobject *kobj, struct kobj_attribute *attr,
		char *buf);
ssize_t kobj_int_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t size);

/*
 * In the following macros there are the following parameters
 * _obj - either kobj or dev: Specifies if macro is for kobject or device
 * _mode - sysfs access mode
 * _min - minimum allowed value
 * _max - maximum allowed value
 * _enum_str_static - a static string array with an entry for each of the enum values
 * _enum_long_str_static - a more detailled version of _enum_str_static
 * _enum_str - two function to generate the string for an enum or enum_str_static
 *             and enum_long_str_static to use _enum_str_static and _enum_long_str_static
 * _val_ptr - a function which is called to extract the parameter from kobj or dev or attr
 * _data - a value which is passed to _val_ptr (usually an offset)
 * _update - a (optional) function which is called to set the value
 * _info - a string with some description
 */

#define ENUM_ATTR(_obj, _name, _mode, _min, _max, \
	_enum_str_static, _enum_long_str_static, \
	_enum_str, _enum_long_str, _val_ptr, _data, _update) \
struct enum_attr _obj ## _enum_attr_ ## _name = { \
	.attr._obj ## _attr = \
		__ATTR(_name, _mode, _obj ## _enum_show, _obj ## _enum_store), \
	.min = _min, \
	.max = _max, \
	.enum_str_static = _enum_str_static, \
	.enum_long_str_static = _enum_long_str_static, \
	.enum_str = { \
		_enum_str, \
		_enum_long_str, \
	}, \
	.val_ptr = _val_ptr, \
	.data = _data, \
	.update = _update, \
}

#define ENUM_SHORT_ATTR(_obj, _name, _mode, _enum, _val_ptr, _data, _update) \
	ENUM_ATTR(_obj, _name, _mode, _enum ## _MIN, _enum ## _MAX, \
		_enum ## _str, NULL, enum_str_static, enum_long_str_static, \
		_val_ptr, _data, _update)

#define ENUM_LONG_ATTR(_obj, _name, _mode, _enum, _val_ptr, _data, _update) \
	ENUM_ATTR(_obj, _name, _mode, _enum ## _MIN, _enum ## _MAX, \
		_enum ## _str, _enum ## _long_str, enum_str_static, enum_long_str_static, \
		_val_ptr, _data, _update)

#define ENUM_FN_ATTR(_obj, _name, _mode, _enum, _enum_str, _enum_long_str, \
	_val_ptr, _data, _update) \
	ENUM_ATTR(_obj, _name, _mode, _enum ## _MIN, _enum ## _MAX, \
		NULL, NULL, _enum_str, _enum_long_str, \
		_val_ptr, _data, _update)

#define INT_ATTR(_obj, _name, _mode, __min, __max, \
	_val_ptr, _data, _update) \
struct int_attr _obj ## _int_attr_ ## _name = { \
	.attr._obj ## _attr = \
		__ATTR(_name, _mode, _obj ## _int_show, _obj ## _int_store), \
	.min_attr._obj ## _attr = __ATTR(_name ## _min, _mode & 0444, _obj ## _min_int_show, NULL), \
	.max_attr._obj ## _attr = __ATTR(_name ## _max, _mode & 0444, _obj ## _max_int_show, NULL), \
	.min = __min, \
	.max = __max, \
	.val_ptr = _val_ptr, \
	.data = _data, \
	.update = _update, \
}

#define INT_RO_ATTR(_obj, _name, _mode, _val_ptr, _data) \
struct int_attr _obj ## _int_ro_attr_ ## _name = { \
	.attr._obj ## _attr = \
		__ATTR(_name, _mode & 0444, _obj ## _int_show, NULL), \
	.val_ptr = _val_ptr, \
	.data = _data, \
}

#define BOOL_ATTR(_obj, _name, _mode, _val_ptr, _data, _update) \
struct bool_attr _obj ## _bool_attr_ ## _name = { \
	.attr._obj ## _attr = \
		__ATTR(_name, _mode, _obj ## _bool_show, _obj ## _bool_store), \
	.val_ptr = _val_ptr, \
	.data = _data, \
	.update = _update, \
}

#define INFO_ATTR(_obj, _name, _mode, _info) \
struct info_attr _obj ## _info_attr_ ## _name = { \
	.attr._obj ## _attr = \
		__ATTR(_name, _mode, _obj ## _info_show, NULL), \
	.info = _info, \
}

#define KOBJ_ATTR(_name, _mode, _show, _store) \
struct kobj_attribute kobj_attr_ ## _name = \
	 __ATTR(_name, _mode, _show, _store)

#define DEV_ENUM_ATTR(...) ENUM_ATTR(dev, __VA_ARGS__)
#define KOBJ_ENUM_ATTR(...) ENUM_ATTR(kobj, __VA_ARGS__)
#define DEV_ENUM_SHORT_ATTR(...) ENUM_SHORT_ATTR(dev, __VA_ARGS__)
#define KOBJ_ENUM_SHORT_ATTR(...) ENUM_SHORT_ATTR(kobj, __VA_ARGS__)
#define DEV_ENUM_LONG_ATTR(...) ENUM_LONG_ATTR(dev, __VA_ARGS__)
#define KOBJ_ENUM_LONG_ATTR(...) ENUM_LONG_ATTR(kobj, __VA_ARGS__)
#define DEV_ENUM_FN_ATTR(...) ENUM_FN_ATTR(dev, __VA_ARGS__)
#define KOBJ_ENUM_FN_ATTR(...) ENUM_FN_ATTR(kobj, __VA_ARGS__)
#define DEV_INT_ATTR(...) INT_ATTR(dev, __VA_ARGS__)
#define KOBJ_INT_ATTR(...) INT_ATTR(kobj, __VA_ARGS__)
#define DEV_INT_RO_ATTR(...) INT_RO_ATTR(dev, __VA_ARGS__)
#define KOBJ_INT_RO_ATTR(...) INT_RO_ATTR(kobj, __VA_ARGS__)
#define DEV_INFO_ATTR(...) INFO_ATTR(dev, __VA_ARGS__)
#define KOBJ_INFO_ATTR(...) INFO_ATTR(kobj, __VA_ARGS__)
#define DEV_BOOL_ATTR(...) BOOL_ATTR(dev, __VA_ARGS__)
#define KOBJ_BOOL_ATTR(...) BOOL_ATTR(kobj, __VA_ARGS__)

#endif /*__AAT2870_BL_SYSFS_H__ */
