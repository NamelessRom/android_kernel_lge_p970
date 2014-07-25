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

#ifndef __AAT2870_BL_UTIL_H__
#define __AAT2870_BL_UTIL_H__

#define for_array(_idx, _array) for (_idx = 0; _idx < ARRAY_SIZE(_array); ++_idx)
#define for_array_rev(_idx, _array) for (_idx = ARRAY_SIZE(_array); _idx-- != 0;)

#define ENUM_SIZE(_enum) ((_enum ## _MAX) - (_enum ## _MIN) + 1)

#endif /*__AAT2870_BL_UTIL_H__ */
