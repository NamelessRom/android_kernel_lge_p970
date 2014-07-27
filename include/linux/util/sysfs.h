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

#ifndef __UTIL_SYSFS_H__
#define __UTIL_SYSFS_H__

#include <linux/device.h>

#define REQUIRE_ATTR_TYPE(x) WARN_ON(p->val_attr->attr_type != x)

enum ATTR_TYPE {
	ATTR_TYPE_BOOL,
	ATTR_TYPE_ENUM,
	ATTR_TYPE_INT,
	ATTR_TYPE_INT_ARRAY,
	ATTR_TYPE_INT_MIN_MAX,
	ATTR_TYPE_INFO,
};

enum OBJECT_TYPE {
	OBJECT_TYPE_KOBJECT,
	OBJECT_TYPE_DEVICE,
};

struct kobj_props {
	enum OBJECT_TYPE obj_type;
	struct kobject *kobj;
	struct val_attr *val_attr;
	void *val;
};

struct bool_attr {
	int (*update)(struct kobj_props *p, bool new_val);
};

struct int_attr {
	int min;
	int max;

	int (*update)(struct kobj_props *p, int new_val);
};

struct int_min_max_attr {
	struct int_attr *int_attr;
};

struct int_array_attr {
	size_t size;

	int (*update)(struct kobj_props *p, int *new_val, size_t size);
};

struct info_attr {
	const char *info;
};

struct enum_attr {
	int min;
	int max;
	const char **enum_short_str_static;
	const char **enum_long_str_static;
	size_t (*enum_str[2])(struct kobj_props *p, int val, char *buf, size_t size);
	int (*update)(struct kobj_props *p, int new_val);
};

struct val_attr {
	enum ATTR_TYPE attr_type;

	const char *name;

	void *(*val_ptr)(struct kobject *kobj, struct val_attr *val_attr);
	long data;

	union {
		struct bool_attr bool_attr;
		struct int_attr int_attr;
		struct int_min_max_attr int_min_max_attr;
		struct int_array_attr int_array_attr;
		struct info_attr info_attr;
		struct enum_attr enum_attr;
	} attr;
};

struct dev_val_attr {
	struct device_attribute dev_attr;
	struct val_attr val_attr;
};

struct kobj_val_attr {
	struct kobj_attribute kobj_attr;
	struct val_attr val_attr;
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
 * _enum_short_str_static - a static string array with an entry for each of the enum values
 * _enum_long_str_static - a more detailled version of _enum_short_str_static
 * _enum_str - two function to generate the string for an enum,
 *             if NULL, the static variants are used,
 *             and if those are NULL, the integer representation is used
 * _val_ptr - a function which is called to extract the parameter
 * _data - a value which is passed to _val_ptr (usually an offset)
 * _update - a (optional) function which is called to set the value,
 *           if not set, it's just assigned
 * _info - a string with some information
 */

#define OBJ_VAL_ATTR_FILL(_obj, _name, _mode) \
	._obj ## _attr = __ATTR(_name, _mode, _obj ## _val_show, _obj ## _val_store) \

#define VAL_ATTR_FILL(_obj, _name, _val_ptr, _data) \
	.name = #_name, \
	.val_ptr = _val_ptr, \
	.data = _data \

#define VAL_ENUM_ATTR(_obj, _name, _mode, _min, _max, \
	_enum_short_str_static, _enum_long_str_static, \
	_enum_str, _enum_long_str, _val_ptr, _data, _update) \
struct _obj ## _val_attr _obj ## _val_attr_ ## _name = { \
	OBJ_VAL_ATTR_FILL(_obj, _name, _mode), \
	.val_attr = { \
		VAL_ATTR_FILL(_obj, _name, _val_ptr, _data), \
		.attr_type = ATTR_TYPE_ENUM, \
		.attr.enum_attr = { \
			.min = _min, \
			.max = _max, \
			.enum_short_str_static = _enum_short_str_static, \
			.enum_long_str_static = _enum_long_str_static, \
			.enum_str = { _enum_str, _enum_long_str, }, \
			.update = _update, \
		}, \
	}, \
}

#define VAL_ENUM_SHORT_ATTR(_obj, _name, _mode, _enum, _val_ptr, _data, _update) \
	VAL_ENUM_ATTR(_obj, _name, _mode, _enum ## _MIN, _enum ## _MAX, \
		_enum ## _str, NULL, NULL, NULL, \
		_val_ptr, _data, _update)

#define VAL_ENUM_LONG_ATTR(_obj, _name, _mode, _enum, _val_ptr, _data, _update) \
	VAL_ENUM_ATTR(_obj, _name, _mode, _enum ## _MIN, _enum ## _MAX, \
		_enum ## _str, _enum ## _long_str, NULL, NULL, \
		_val_ptr, _data, _update)

#define VAL_ENUM_FN_ATTR(_obj, _name, _mode, _enum, _enum_str, _enum_long_str, \
	_val_ptr, _data, _update) \
	VAL_ENUM_ATTR(_obj, _name, _mode, _enum ## _MIN, _enum ## _MAX, \
		NULL, NULL, _enum_str, _enum_long_str, \
		_val_ptr, _data, _update)

#define VAL_INT_ATTR(_obj, _name, _mode, __min, __max, \
	_val_ptr, _data, _update) \
struct _obj ## _val_attr _obj ## _val_attr_ ## _name = { \
	OBJ_VAL_ATTR_FILL(_obj, _name, _mode), \
	.val_attr = { \
		VAL_ATTR_FILL(_obj, _name, _val_ptr, _data), \
		.attr_type = ATTR_TYPE_INT, \
		.attr.int_attr = { \
			.min = __min, \
			.max = __max, \
			.update = _update, \
		} \
	}, \
}

#define VAL_INT_MIN_MAX_ATTR(_obj, _name, _mode) \
struct _obj ## _val_attr _obj ## _val_attr_ ## _name ## _min_max = { \
	OBJ_VAL_ATTR_FILL(_obj, _name ## _min_max, (_mode) & 0444), \
	.val_attr = { \
		VAL_ATTR_FILL(_obj, _name ## _min_max, NULL, 0), \
		.attr_type = ATTR_TYPE_INT_MIN_MAX, \
		.attr.int_min_max_attr.int_attr = \
			&_obj ## _val_attr_ ## _name.val_attr.attr.int_attr, \
	}, \
}

#define VAL_INT_RO_ATTR(_obj, _name, _mode, _val_ptr, _data) \
	VAL_INT_ATTR(_obj, _name, _mode & 0444, 0, -1, _val_ptr, _data, NULL)

#define VAL_INT_ARRAY_ATTR(_obj, _name, _mode, _val_ptr, _data, _size, _update) \
struct _obj ## _val_attr _obj ## _val_attr_ ## _name = { \
	OBJ_VAL_ATTR_FILL(_obj, _name, _mode), \
	.val_attr = { \
		VAL_ATTR_FILL(_obj, _name, _val_ptr, _data), \
		.attr_type = ATTR_TYPE_INT_ARRAY, \
		.attr.int_array_attr = { \
			.size = _size, \
			.update = _update, \
		}, \
	}, \
}

#define VAL_BOOL_ATTR(_obj, _name, _mode, _val_ptr, _data, _update) \
struct _obj ## _val_attr _obj ## _val_attr_ ## _name = { \
	OBJ_VAL_ATTR_FILL(_obj, _name, _mode), \
	.val_attr = { \
		VAL_ATTR_FILL(_obj, _name, _val_ptr, _data), \
		.attr_type = ATTR_TYPE_BOOL, \
		.attr.bool_attr.update = _update, \
	}, \
}

#define VAL_INFO_ATTR(_obj, _name, _mode, _info) \
struct _obj ## _val_attr _obj ## _val_attr_ ## _name = { \
	OBJ_VAL_ATTR_FILL(_obj, _name, _mode), \
	.val_attr = { \
		VAL_ATTR_FILL(_obj, _name, NULL, 0), \
		.attr_type = ATTR_TYPE_INFO, \
		.attr.info_attr.info = _info, \
	}, \
}

#define DEV_ATTR(_name, _mode, _show, _store) \
struct device_attribute dev_attr_ ## _name = \
	 __ATTR(_name, _mode, _show, _store)

#define KOBJ_ATTR(_name, _mode, _show, _store) \
struct kobj_attribute kobj_attr_ ## _name = \
	 __ATTR(_name, _mode, _show, _store)

#define DEV_BOOL_ATTR(...) VAL_BOOL_ATTR(dev, __VA_ARGS__)
#define DEV_ENUM_ATTR(...) VAL_ENUM_ATTR(dev, __VA_ARGS__)
#define DEV_ENUM_SHORT_ATTR(...) VAL_ENUM_SHORT_ATTR(dev, __VA_ARGS__)
#define DEV_ENUM_LONG_ATTR(...) VAL_ENUM_LONG_ATTR(dev, __VA_ARGS__)
#define DEV_ENUM_FN_ATTR(...) VAL_ENUM_FN_ATTR(dev, __VA_ARGS__)
#define DEV_INFO_ATTR(...) VAL_INFO_ATTR(dev, __VA_ARGS__)
#define DEV_INT_ARRAY_ATTR(...) VAL_INT_ARRAY_ATTR(dev, __VA_ARGS__)
#define DEV_INT_ATTR(...) VAL_INT_ATTR(dev, __VA_ARGS__)
#define DEV_INT_MIN_MAX_ATTR(...) VAL_INT_MIN_MAX_ATTR(dev, __VA_ARGS__)
#define DEV_INT_RO_ATTR(...) VAL_INT_RO_ATTR(dev, __VA_ARGS__)

#define KOBJ_BOOL_ATTR(...) VAL_BOOL_ATTR(kobj, __VA_ARGS__)
#define KOBJ_ENUM_ATTR(...) VAL_ENUM_ATTR(kobj, __VA_ARGS__)
#define KOBJ_ENUM_SHORT_ATTR(...) VAL_ENUM_SHORT_ATTR(kobj, __VA_ARGS__)
#define KOBJ_ENUM_LONG_ATTR(...) VAL_ENUM_LONG_ATTR(kobj, __VA_ARGS__)
#define KOBJ_ENUM_FN_ATTR(...) VAL_ENUM_FN_ATTR(kobj, __VA_ARGS__)
#define KOBJ_INFO_ATTR(...) VAL_INFO_ATTR(kobj, __VA_ARGS__)
#define KOBJ_INT_ARRAY_ATTR(...) VAL_INT_ARRAY_ATTR(kobj, __VA_ARGS__)
#define KOBJ_INT_ATTR(...) VAL_INT_ATTR(kobj, __VA_ARGS__)
#define KOBJ_INT_MIN_MAX_ATTR(...) VAL_INT_MIN_MAX_ATTR(kobj, __VA_ARGS__)
#define KOBJ_INT_RO_ATTR(...) VAL_INT_RO_ATTR(kobj, __VA_ARGS__)

void *val_from_kobj_offset(struct kobject *kobj, struct val_attr *attr);
#define KOBJ_OFFSET(_ctr, _val) val_from_kobj_offset, \
	(offsetof(_ctr, _val) - offsetof(_ctr, kobj))

#endif /*__UTIL_SYSFS_H__ */
