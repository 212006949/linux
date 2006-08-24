/*
 * linux/arch/arm/mach-davinci/board-evm.c
 *
 * TI DaVinci EVM board
 *
 * Copyright (C) 2006 Texas Instruments.
 *
 * ----------------------------------------------------------------------------
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
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 * ----------------------------------------------------------------------------
 *
 */

/**************************************************************************
 * Included Files
 **************************************************************************/

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/major.h>
#include <linux/root_dev.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/mtd/physmap.h>
#if defined(CONFIG_USB_MUSB_HDRC) || defined(CONFIG_USB_MUSB_HDRC_MODULE)
#include <linux/usb_musb.h>
#endif

#include <asm/setup.h>
#include <asm/io.h>
#include <asm/mach-types.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/mach/flash.h>

#include <asm/arch/irqs.h>
#include <asm/arch/common.h>
#include <asm/arch/hardware.h>
#include "clock.h"

static struct mtd_partition davinci_evm_partitions[] = {
	/* bootloader (U-Boot, etc) in first 4 sectors */
	{
	      .name		= "bootloader",
	      .offset		= 0,
	      .size		= 4 * SZ_64K,
	      .mask_flags	= MTD_WRITEABLE, /* force read-only */
	},
	/* bootloader params in the next 1 sectors */
	{
	      .name		= "params",
	      .offset		= MTDPART_OFS_APPEND,
	      .size		= SZ_64K,
	      .mask_flags	= 0,
	},
	/* kernel */
	{
	      .name		= "kernel",
	      .offset		= MTDPART_OFS_APPEND,
	      .size		= SZ_2M,
	      .mask_flags	= 0
	},
	/* file system */
	{
	      .name		= "filesystem",
	      .offset		= MTDPART_OFS_APPEND,
	      .size		= MTDPART_SIZ_FULL,
	      .mask_flags	= 0
	}
};

static struct physmap_flash_data davinci_evm_flash_data = {
	.width		= 2,
	.parts		= davinci_evm_partitions,
	.nr_parts	= ARRAY_SIZE(davinci_evm_partitions),
};

/* NOTE: CFI probe will correctly detect flash part as 32M, but EMIF
 * limits addresses to 16M, so using addresses past 16M will wrap */
static struct resource davinci_evm_flash_resource = {
	.start		= DAVINCI_CS0_PHYS,
	.end		= DAVINCI_CS0_PHYS + SZ_16M - 1,
	.flags		= IORESOURCE_MEM,
};

static struct platform_device davinci_evm_flash_device = {
	.name		= "physmap-flash",
	.id		= 0,
	.dev		= {
		.platform_data	= &davinci_evm_flash_data,
	},
	.num_resources	= 1,
	.resource	= &davinci_evm_flash_resource,
};

/*
 * USB
 */
#if defined(CONFIG_USB_MUSB_HDRC) || defined(CONFIG_USB_MUSB_HDRC_MODULE)

static struct musb_hdrc_platform_data usb_data = {
#if     defined(CONFIG_USB_MUSB_OTG)
	/* OTG requires a Mini-AB connector */
	.mode           = MUSB_OTG,
#elif   defined(CONFIG_USB_MUSB_PERIPHERAL)
	.mode           = MUSB_PERIPHERAL,
#elif   defined(CONFIG_USB_MUSB_HOST)
	.mode           = MUSB_HOST,
#endif
	/* irlml6401 switches 5V */
	.power          = 255,          /* sustains 3.0+ Amps (!) */
	.potpgt         = 4,            /* ~8 msec */

	/* REVISIT multipoint is a _chip_ capability; not board specific */
	.multipoint     = 1,
};

static struct resource usb_resources [] = {
	{
		/* physical address */
		.start          = DAVINCI_USB_OTG_BASE,
		.end            = DAVINCI_USB_OTG_BASE + 0x5ff,
		.flags          = IORESOURCE_MEM,
	},
	{
		.start          = IRQ_USBINT,
		.flags          = IORESOURCE_IRQ,
	},
};

static u64 usb_dmamask = DMA_32BIT_MASK;

static struct platform_device usb_dev = {
	.name           = "musb_hdrc",
	.id             = -1,
	.dev = {
		.platform_data		= &usb_data,
		.dma_mask		= &usb_dmamask,
		.coherent_dma_mask      = DMA_32BIT_MASK,
        },
	.resource       = usb_resources,
	.num_resources  = ARRAY_SIZE(usb_resources),
};

static inline void setup_usb(void)
{
	/* REVISIT:  everything except platform_data setup should be
	* shared between all DaVinci boards using the same core.
	*/
	int status;

	status = platform_device_register(&usb_dev);
	if (status != 0)
		pr_debug("setup_usb --> %d\n", status);
	else
		board_setup_psc(DAVINCI_GPSC_ARMDOMAIN, DAVINCI_LPSC_USB, 1);
}

#else
#define setup_usb(void)	do {} while(0)
#endif  /* CONFIG_USB_MUSB_HDRC */

static struct platform_device *davinci_evm_devices[] __initdata = {
	&davinci_evm_flash_device,
};

static void board_init(void)
{
	board_setup_psc(DAVINCI_GPSC_ARMDOMAIN, DAVINCI_LPSC_VPSSMSTR, 1);
	board_setup_psc(DAVINCI_GPSC_ARMDOMAIN, DAVINCI_LPSC_VPSSSLV, 1);
	board_setup_psc(DAVINCI_GPSC_ARMDOMAIN, DAVINCI_LPSC_TPCC, 1);
	board_setup_psc(DAVINCI_GPSC_ARMDOMAIN, DAVINCI_LPSC_TPTC0, 1);
	board_setup_psc(DAVINCI_GPSC_ARMDOMAIN, DAVINCI_LPSC_TPTC1, 1);

	/* Turn on WatchDog timer LPSC.  Needed for RESET to work */
	board_setup_psc(DAVINCI_GPSC_ARMDOMAIN, DAVINCI_LPSC_TIMER2, 1);
}

static void __init
davinci_evm_map_io(void)
{
	davinci_map_common_io();

	/* Initialize the DaVinci EVM board settigs */
	board_init ();
}

static __init void davinci_evm_init(void)
{
	platform_add_devices(davinci_evm_devices,
			     ARRAY_SIZE(davinci_evm_devices));

	davinci_serial_init();
	setup_usb();
}

static __init void davinci_evm_irq_init(void)
{
	davinci_init_common_hw();
	davinci_irq_init();
}

MACHINE_START(DAVINCI_EVM, "DaVinci EVM")
	/* Maintainer: MontaVista Software <source@mvista.com> */
	.phys_io      = IO_PHYS,
	.io_pg_offst  = (io_p2v(IO_PHYS) >> 18) & 0xfffc,
	.boot_params  = (DAVINCI_DDR_BASE + 0x100),
	.map_io	      = davinci_evm_map_io,
	.init_irq     = davinci_evm_irq_init,
	.timer	      = &davinci_timer,
	.init_machine = davinci_evm_init,
MACHINE_END
