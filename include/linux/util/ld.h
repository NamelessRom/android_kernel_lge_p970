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

#ifndef __UTIL_LD_H__
#define __UTIL_LD_H__

#include <linux/types.h>

#define LD_OFFSET_MIN 1
#define LD_OFFSET_MAX 128
#define LD_OFFSET_DFL 4

/*
 * Calculate (ld(val) * 2^16) for 1 <= val <= 256
 * Values outside of this interval yield UINT_MAX
 */
uint32_t util_ld(uint32_t val);

/*
 * Calculate ceil(2^(ld_val / 2^16)) for ld_val <= 8 * 2^16
 * Values outside of this interval yield UINT_MAX
 */
uint32_t util_ceil_pow_2(uint32_t ld_val);

/*
 * Fill the vals from min to max in an exponential way and
 * respect the offset, i.e.
 * vals[i] = (min + off) * ((max + off) / (min + off)) ^ (i / 15) - off
 */
void util_fill_exp(uint32_t *vals, size_t size, uint32_t off, uint32_t min, uint32_t max);

#endif /* __UTIL_LD_H__ */
