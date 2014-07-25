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

enum ATTR_TYPE {
	ATTR_TYPE_BOOL,
	ATTR_TYPE_ENUM,
	ATTR_TYPE_INT,
	ATTR_TYPE_INFO,
};

struct kobj_props {
	struct kobject *kobj;
	struct val_attr *attr;
	void *val;
};

struct val_attr {
	enum ATTR_TYPE attr_type;

	union {
		struct kobj_attribute kobj_attr;
		struct device_attribute dev_attr;
	} attr;

	void *(*val_ptr)(struct kobject *kobj, struct val_attr *val_attr);
	void (*update)(struct kobj_props *p, void *new_val);
	long data;
};

struct bool_attr {
	struct val_attr val_attr;
};

struct int_attr {
	struct val_attr val_attr;
	struct val_attr min_attr;
	struct val_attr max_attr;

	int min;
	int max;
};

struct info_attr {
	struct val_attr val_attr;

	const char *info;
};

struct enum_attr {
	struct val_attr val_attr;

	int min;
	int max;
	const char **enum_str_static;
	const char **enum_long_str_static;
	size_t (*enum_str[2])(struct kobj_props *p, int val, char *buf, size_t size);
};

/* Wrapper functions for `dev` which utilize the helper functions above */
ssize_t dev_val_show(struct device *dev, struct device_attribute *attr,
		char *buf);
ssize_t dev_val_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t size);

/* Wrapper functions for `kobj` which utilize the helper functions above */
ssize_t kobj_val_show(struct kobject *kobj, struct kobj_attribute *attr,
		char *buf);
ssize_t kobj_val_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t size);

void *min_int(struct kobject *kobj, struct val_attr *a);
void *max_int(struct kobject *kobj, struct val_attr *a);

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

#define VAL_ATTR(_obj, _attr_type, _name, _mode, _show, _store, _val_ptr, _data, _update) { \
	.attr_type = _attr_type, \
	.attr._obj ## _attr = __ATTR(_name, _mode, _show, _store), \
	.data = _data, \
	.val_ptr = _val_ptr, \
	.update = _update, \
}

#define ENUM_ATTR(_obj, _name, _mode, _min, _max, \
	_enum_str_static, _enum_long_str_static, \
	_enum_str, _enum_long_str, _val_ptr, _data, _update) \
struct enum_attr _obj ## _enum_attr_ ## _name = { \
	.val_attr = \
		VAL_ATTR(_obj, ATTR_TYPE_ENUM, _name, _mode, \
			_obj ## _val_show, _obj ## _val_store, _val_ptr, _data, _update), \
	.min = _min, \
	.max = _max, \
	.enum_str_static = _enum_str_static, \
	.enum_long_str_static = _enum_long_str_static, \
	.enum_str = { \
		_enum_str, \
		_enum_long_str, \
	}, \
}

#define ENUM_SHORT_ATTR(_obj, _name, _mode, _enum, _val_ptr, _data, _update) \
	ENUM_ATTR(_obj, _name, _mode, _enum ## _MIN, _enum ## _MAX, \
		_enum ## _str, NULL, NULL, NULL, \
		_val_ptr, _data, _update)

#define ENUM_LONG_ATTR(_obj, _name, _mode, _enum, _val_ptr, _data, _update) \
	ENUM_ATTR(_obj, _name, _mode, _enum ## _MIN, _enum ## _MAX, \
		_enum ## _str, _enum ## _long_str, NULL, NULL, \
		_val_ptr, _data, _update)

#define ENUM_FN_ATTR(_obj, _name, _mode, _enum, _enum_str, _enum_long_str, \
	_val_ptr, _data, _update) \
	ENUM_ATTR(_obj, _name, _mode, _enum ## _MIN, _enum ## _MAX, \
		NULL, NULL, _enum_str, _enum_long_str, \
		_val_ptr, _data, _update)

#define INT_ATTR(_obj, _name, _mode, __min, __max, \
	_val_ptr, _data, _update) \
struct int_attr _obj ## _int_attr_ ## _name = { \
	.val_attr = VAL_ATTR(_obj, ATTR_TYPE_INT, _name, _mode, \
		_obj ## _val_show, _obj ## _val_store, _val_ptr, _data, _update), \
	.min_attr = VAL_ATTR(_obj, ATTR_TYPE_INT, _name ## _min, _mode & 0444, \
		_obj ## _val_show, NULL, min_int, 0, NULL), \
	.max_attr = VAL_ATTR(_obj, ATTR_TYPE_INT, _name ## _max, _mode & 0444, \
		_obj ## _val_show, NULL, max_int, 0, NULL), \
	.min = __min, \
	.max = __max, \
}

#define INT_RO_ATTR(_obj, _name, _mode, _val_ptr, _data) \
struct int_attr _obj ## _int_ro_attr_ ## _name = { \
	.val_attr = \
		VAL_ATTR(_obj, ATTR_TYPE_INT, _name, _mode & 0444, \
			_obj ## _val_show, _obj ## _val_store, _val_ptr, _data, NULL), \
}

#define BOOL_ATTR(_obj, _name, _mode, _val_ptr, _data, _update) \
struct bool_attr _obj ## _bool_attr_ ## _name = { \
	.val_attr = VAL_ATTR(_obj, ATTR_TYPE_BOOL, _name, _mode, \
		_obj ## _val_show, _obj ## _val_store, _val_ptr, _data, _update), \
}

#define INFO_ATTR(_obj, _name, _mode, _info) \
struct info_attr _obj ## _info_attr_ ## _name = { \
	.val_attr = \
		VAL_ATTR(_obj, ATTR_TYPE_INFO, _name, _mode, \
			_obj ## _val_show, NULL, NULL, 0, NULL), \
	.info = _info, \
}

#define DEV_ATTR(_name, _mode, _show, _store) \
struct device_attribute dev_attr_ ## _name = \
	 __ATTR(_name, _mode, _show, _store)

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

void *val_from_kobj_offset(struct kobject *kobj, struct val_attr *attr);
#define KOBJ_OFFSET(_ctr, _val) val_from_kobj_offset, \
	(offsetof(_ctr, _val) - offsetof(_ctr, kobj))

#endif /*__AAT2870_BL_SYSFS_H__ */
