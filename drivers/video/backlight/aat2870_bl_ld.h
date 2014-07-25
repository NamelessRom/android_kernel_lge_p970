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

#ifndef __AAT2870_BL_LD_H__
#define __AAT2870_BL_LD_H__

#define LD_OFFSET_MIN 1
#define LD_OFFSET_MAX 128
#define LD_OFFSET_DFL 4

/*
 * This driver uses some fixed point arithmetic to calculate
 * the fading brightness levels. The following "ld" table
 * is used to fade the brightness in an exponential way
 * to match the logarithmic response of the human eye.
 */
extern const uint32_t ld[];
uint32_t ceil_pow_2(uint32_t ld_val);

#endif /* __AAT2870_BL_LD_H__ */
