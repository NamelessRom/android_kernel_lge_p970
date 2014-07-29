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

#ifndef __AAT2870_BL_H__
#define __AAT2870_BL_H__

#include <linux/i2c.h>

#define LCD_CP_EN 62
#define HUB_PANEL_LCD_RESET_N 34
#define HUB_PANEL_LCD_CS 54
#define AAT2870_I2C_BL_NAME "aat2870_i2c_bl"

enum REG {
	REG_EN_CH = 0x00, /* Enable backlight channels */
	REG_ALS_CFG0 = 0x0E, /* gain resistor, als enable, logarithmic i/o */
	REG_ALS_CFG1 = 0x0F, /* sbias voltage, enable */
	REG_ALS_CFG2 = 0x10, /* polling mode, interval, gain */
	REG_AMB = 0x11, /* ambient light level digital output */
	REG_ALS0 = 0x12, /* ALS brightness level for als_level 0 */
	/* ... */
	REG_ALS15 = 0x21, /* ALS brightness level for als_level 15 */
	REG_LDOAB = 0x24, /* LDO A+B output voltage level */
	REG_LDOCD = 0x25, /* LDO C+D output voltage level */
	REG_EN_LDO = 0x26, /* LDO A-D output enable */
};

/* brightness regs are from REG18 to REG33 */
#define BRIGHTNESS_REGS (REG_ALS15 - REG_ALS0 + 1)

int i2c_read_reg(struct i2c_client *client,
		unsigned char reg, unsigned char *val);
int i2c_write_reg(struct i2c_client *client,
		enum REG reg, unsigned char val);

#endif /* __AAT2870_BL_H__ */
