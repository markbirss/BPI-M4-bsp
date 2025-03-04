/*
 * pmic-g2237.h
 *
 * Copyright (C) 2017 Realtek Semiconductor Corporation
 * Copyright (C) 2017 Cheng-Yu Lee <cylee12@realtek.com>
 *
 */

#ifndef __PMIC_G2237_H__
#define __PMIC_G2237_H__

enum {
	PMIC_G2237_LPOFF_TO_DO = 0,
	PMIC_G2237_SOFTOFF,
	PMIC_G2237_DC4_ON,
	PMIC_G2237_DC4_NMODE,
	PMIC_G2237_DC4_SMODE,

	PMIC_G2237_DC1_ON,
	PMIC_G2237_DC1_NMODE,
	PMIC_G2237_DC1_SMODE,
	PMIC_G2237_DC1_NVO,
	PMIC_G2237_DC1_SVO,

	PMIC_G2237_DC2_ON,
	PMIC_G2237_DC2_NMODE,
	PMIC_G2237_DC2_SMODE,
	PMIC_G2237_DC2_NVO,
	PMIC_G2237_DC2_SVO,

	PMIC_G2237_DC3_ON,
	PMIC_G2237_DC3_NMODE,
	PMIC_G2237_DC3_SMODE,
	PMIC_G2237_DC3_NVO,
	PMIC_G2237_DC3_SVO,

	PMIC_G2237_DC5_ON,
	PMIC_G2237_DC5_NMODE,
	PMIC_G2237_DC5_SMODE,
	PMIC_G2237_DC5_NVO,
	PMIC_G2237_DC5_SVO,

	PMIC_G2237_LDO1_ON,
	PMIC_G2237_LDO1_NMODE,
	PMIC_G2237_LDO1_SMODE,
	PMIC_G2237_LDO1_NVO,
	PMIC_G2237_LDO1_SVO,

	PMIC_G2237_PWRKEY,
	PMIC_G2237_PWRKEY_LP,
	PMIC_G2237_PWRKEY_IT,
	PMIC_G2237_DCDECT,

	PMIC_G2237_MAX
};

#endif
