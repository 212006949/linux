/*
 * include/asm-arm/arch-omap/omap34xx.h
 *
 * This file contains the processor specific definitions of the TI OMAP34XX.
 *
 * Copyright (C) 2007 Texas Instruments.
 * Copyright (C) 2007 Nokia Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef __ASM_ARCH_OMAP34XX_H
#define __ASM_ARCH_OMAP34XX_H

/*
 * Please place only base defines here and put the rest in device
 * specific headers.
 */

#define L4_34XX_BASE		0x48000000
#define L4_WK_34XX_BASE		0x48300000
#define L4_WK_OMAP_BASE		L4_WK_OMAP_BASE
#define L4_PER_34XX_BASE	0x49000000
#define L4_PER_OMAP_BASE	L4_PER_34XX_BASE
#define L4_34XX_EMU_BASE	0x54000000
#define L4_EMU_BASE		L4_34XX_EMU_BASE
#define L3_34XX_BASE		0x68000000
#define L3_OMAP_BASE		L3_34XX_BASE

#define OMAP3430_32KSYNCT_BASE	0x48320000
#define OMAP3430_CM_BASE	0x48004800
#define OMAP3430_PRM_BASE	0x48306800
#define OMAP343X_SMS_BASE	0x6C000000
#define OMAP343X_SDRC_BASE	0x6D000000
#define OMAP34XX_GPMC_BASE	0x6E000000
#define OMAP3430_SCM_BASE	0x48002000
#define OMAP3430_CTRL_BASE	OMAP3430_SCM_BASE

#define OMAP34XX_IC_BASE	0x48200000
#define OMAP34XX_IVA_INTC_BASE	0x40000000
#define IRQ_SIR_IRQ		0x0040


#if defined(CONFIG_ARCH_OMAP3430)

/*
 * REVISIT: OMAP3430 has two CONTROL_DEVCONF registers, CONTROL_DEVCONF0
 * and CONTROL_DEVCONF1.  We should probably split those defines, along
 * with any other System Control Module registers and read/write fns,
 * out to a separate scm.h file, and do this for 24xx also.
 */
#define OMAP2_32KSYNCT_BASE		OMAP3430_32KSYNCT_BASE
#define OMAP2_CM_BASE			OMAP3430_CM_BASE
#define OMAP2_PRM_BASE			OMAP3430_PRM_BASE
#define OMAP2_SDRC_BASE			OMAP343X_SDRC_BASE
#define OMAP2_SMS_BASE			OMAP343X_SMS_BASE
#define OMAP2_L4_BASE			L4_34XX_BASE
#define OMAP2_VA_IC_BASE		IO_ADDRESS(OMAP34XX_IC_BASE)
#define OMAP2_CTRL_BASE			OMAP3430_CTRL_BASE
#define OMAP34XX_CONTROL_DEVCONF0	(L4_34XX_BASE + 0x2274)
#define OMAP34XX_CONTROL_DEVCONF1	(L4_34XX_BASE + 0x22D8)

#endif

#define OMAP34XX_DSP_BASE	0x58000000
#define OMAP34XX_DSP_MEM_BASE	(OMAP34XX_DSP_BASE + 0x0)
#define OMAP34XX_DSP_IPI_BASE	(OMAP34XX_DSP_BASE + 0x1000000)
#define OMAP34XX_DSP_MMU_BASE	(OMAP34XX_DSP_BASE + 0x2000000)
#endif /* __ASM_ARCH_OMAP34XX_H */

