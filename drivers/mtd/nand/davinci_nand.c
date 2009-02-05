/*
 * davinci_nand.c - NAND Flash Driver for DaVinci family chips
 *
 * Copyright (C) 2006 Texas Instruments.
 *
 * ported to 2.6.23 (C) 2008 by
 * Sander Huijsen <Shuijsen@optelecom-nkf.com>
 * Troy Kisky <troy.kisky@boundarydevices.com>
 * Dirk Behme <Dirk.Behme@gmail.com>
 *
 * --------------------------------------------------------------------------
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
 * --------------------------------------------------------------------------
 *
 *  Overview:
 *   This is a device driver for the NAND flash device found on the
 *   DaVinci DM6446 EVM board which utilizes the Samsung k9k2g08 part.
 *   (small page NAND).  It should work for some other DaVinci NAND
 *   configurations, but it ignores the dm355 4-bit ECC hardware.
 *
 *   Currently assumes EM_WAIT connects all of the NAND devices in
 *   a "wire-OR" configuration.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/partitions.h>

#include <mach/cpu.h>
#include <mach/hardware.h>
#include <mach/nand.h>
#include <mach/mux.h>

#include <asm/mach-types.h>
#include <asm/mach/flash.h>


#ifdef CONFIG_MTD_PARTITIONS
static inline int mtd_has_partitions(void) { return 1; }
#else
static inline int mtd_has_partitions(void) { return 0; }
#endif

#ifdef CONFIG_MTD_CMDLINE_PARTS
static inline int mtd_has_cmdlinepart(void) { return 1; }
#else
static inline int mtd_has_cmdlinepart(void) { return 0; }
#endif


struct davinci_nand_info {
	struct mtd_info		mtd;
	struct nand_chip	chip;

	struct device		*dev;
	struct clk		*clk;
	bool			partitioned;

	void __iomem		*base;
	void __iomem		*vaddr;

	u32			ioaddr;
	u32			current_cs;

	u32			mask_chipsel;
	u32			mask_ale;
	u32			mask_cle;

	u32			core_chipsel;
};

static DEFINE_SPINLOCK(davinci_nand_lock);

#define to_davinci_nand(m) container_of(m, struct davinci_nand_info, mtd)


static inline unsigned int davinci_nand_readl(struct davinci_nand_info *info,
		int offset)
{
	return __raw_readl(info->base + offset);
}

static inline void davinci_nand_writel(struct davinci_nand_info *info,
		int offset, unsigned long value)
{
	__raw_writel(value, info->base + offset);
}

/*
 * Hardware specific access to control-lines
 *
 * REVISIT avoid casting addresses
 */
static void nand_davinci_hwcontrol(struct mtd_info *mtd, int cmd,
				   unsigned int ctrl)
{
	struct davinci_nand_info	*info = to_davinci_nand(mtd);
	u32				addr = info->current_cs;
	struct nand_chip		*nand = mtd->priv;

	/* Did the control lines change? */
	if (ctrl & NAND_CTRL_CHANGE) {
		if ((ctrl & NAND_CTRL_CLE) == NAND_CTRL_CLE)
			addr |= info->mask_cle;
		else if ((ctrl & NAND_CTRL_ALE) == NAND_CTRL_ALE)
			addr |= info->mask_ale;

		nand->IO_ADDR_W = (void __iomem __force *)addr;
	}

	if (cmd != NAND_CMD_NONE)
		iowrite8(cmd, nand->IO_ADDR_W);
}

static void nand_davinci_select_chip(struct mtd_info *mtd, int chip)
{
	struct davinci_nand_info	*info = to_davinci_nand(mtd);
	u32				addr = info->ioaddr;

	/* maybe kick in a second chipselect */
	if (chip > 0)
		addr |= info->mask_chipsel;
	info->current_cs = addr;

	info->chip.IO_ADDR_W = (void __iomem __force *)addr;
	info->chip.IO_ADDR_R = info->chip.IO_ADDR_W;
}

/*----------------------------------------------------------------------*/

/*
 * 1-bit hardware ECC ... context maintained for each core chipselect
 */

static void nand_davinci_hwctl_1bit(struct mtd_info *mtd, int mode)
{
	struct davinci_nand_info *info;
	u32 retval;
	unsigned long flags;

	info = to_davinci_nand(mtd);

	/* Reset ECC hardware */
	retval = davinci_nand_readl(info, NANDF1ECC_OFFSET
			+ 4 * info->core_chipsel);

	spin_lock_irqsave(&davinci_nand_lock, flags);

	/* Restart ECC hardware */
	retval = davinci_nand_readl(info, NANDFCR_OFFSET);
	retval |= BIT(8 + info->core_chipsel);
	davinci_nand_writel(info, NANDFCR_OFFSET, retval);

	spin_unlock_irqrestore(&davinci_nand_lock, flags);
}

/*
 * Read DaVinci ECC register
 */
static inline u32 nand_davinci_readecc_1bit(struct mtd_info *mtd)
{
	struct davinci_nand_info *info = to_davinci_nand(mtd);

	/* Read register ECC and clear it */
	return davinci_nand_readl(info, NANDF1ECC_OFFSET);
}

/*
 * Read DaVinci ECC registers and rework into MTD format
 */
static int nand_davinci_calculate_1bit(struct mtd_info *mtd,
				      const u_char *dat, u_char *ecc_code)
{
	unsigned int ecc_val = nand_davinci_readecc_1bit(mtd);
	/* squeeze 0 middle bits out so that it fits in 3 bytes */
	unsigned int tmp = (ecc_val & 0x0fff) | ((ecc_val & 0x0fff0000) >> 4);
	/* invert so that erased block ecc is correct */
	tmp = ~tmp;
	ecc_code[0] = (u_char)(tmp);
	ecc_code[1] = (u_char)(tmp >> 8);
	ecc_code[2] = (u_char)(tmp >> 16);

	return 0;
}

static int nand_davinci_correct_1bit(struct mtd_info *mtd, u_char *dat,
				     u_char *read_ecc, u_char *calc_ecc)
{
	struct nand_chip *chip = mtd->priv;
	u_int32_t eccNand = read_ecc[0] | (read_ecc[1] << 8) |
					  (read_ecc[2] << 16);
	u_int32_t eccCalc = calc_ecc[0] | (calc_ecc[1] << 8) |
					  (calc_ecc[2] << 16);
	u_int32_t diff = eccCalc ^ eccNand;

	if (diff) {
		if ((((diff >> 12) ^ diff) & 0xfff) == 0xfff) {
			/* Correctable error */
			if ((diff >> (12 + 3)) < chip->ecc.size) {
				dat[diff >> (12 + 3)] ^= BIT((diff >> 12) & 7);
				return 1;
			} else {
				return -1;
			}
		} else if (!(diff & (diff - 1))) {
			/* Single bit ECC error in the ECC itself,
			   nothing to fix */
			return 1;
		} else {
			/* Uncorrectable error */
			return -1;
		}

	}
	return 0;
}

/*----------------------------------------------------------------------*/

/*
 * NOTE:  NAND boot requires ALE == EM_A[1], CLE == EM_A[2], so that's
 * how these chips are normally wired.  This translates to both 8 and 16
 * bit busses using ALE == BIT(3) in byte addresses, and CLE == BIT(4).
 *
 * For now we assume that configuration, or any other one which ignores
 * the two LSBs for NAND access ... so we can issue 32-bit reads/writes
 * and have that transparently morphed into multiple NAND operations.
 */
static void nand_davinci_read_buf(struct mtd_info *mtd, uint8_t *buf, int len)
{
	struct nand_chip *chip = mtd->priv;

	if ((0x03 & ((unsigned)buf)) == 0 && (0x03 & len) == 0)
		ioread32_rep(chip->IO_ADDR_R, buf, len >> 2);
	else if ((0x01 & ((unsigned)buf)) == 0 && (0x01 & len) == 0)
		ioread16_rep(chip->IO_ADDR_R, buf, len >> 1);
	else
		ioread8_rep(chip->IO_ADDR_R, buf, len);
}

static void nand_davinci_write_buf(struct mtd_info *mtd,
		const uint8_t *buf, int len)
{
	struct nand_chip *chip = mtd->priv;

	if ((0x03 & ((unsigned)buf)) == 0 && (0x03 & len) == 0)
		iowrite32_rep(chip->IO_ADDR_R, buf, len >> 2);
	else if ((0x01 & ((unsigned)buf)) == 0 && (0x01 & len) == 0)
		iowrite16_rep(chip->IO_ADDR_R, buf, len >> 1);
	else
		iowrite8_rep(chip->IO_ADDR_R, buf, len);
}

/*
 * Check hardware register for wait status. Returns 1 if device is ready,
 * 0 if it is still busy.
 */
static int nand_davinci_dev_ready(struct mtd_info *mtd)
{
	struct davinci_nand_info *info = to_davinci_nand(mtd);

	return davinci_nand_readl(info, NANDFSR_OFFSET) & BIT(0);
}

static void __init nand_dm6446evm_flash_init(struct davinci_nand_info *info)
{
	u32 regval, tmp;

	/*
	 * NAND FLASH timings @ PLL1 == 459 MHz
	 *  - AEMIF.CLK freq   = PLL1/6 = 459/6 = 76.5 MHz
	 *  - AEMIF.CLK period = 1/76.5 MHz = 13.1 ns
	 */
	regval = 0
		| (0 << 31)           /* selectStrobe */
		| (0 << 30)           /* extWait (never with NAND) */
		| (1 << 26)           /* writeSetup      10 ns */
		| (3 << 20)           /* writeStrobe     40 ns */
		| (1 << 17)           /* writeHold       10 ns */
		| (0 << 13)           /* readSetup       10 ns */
		| (3 << 7)            /* readStrobe      60 ns */
		| (0 << 4)            /* readHold        10 ns */
		| (3 << 2)            /* turnAround      ?? ns */
		| (0 << 0)            /* asyncSize       8-bit bus */
		;
	tmp = davinci_nand_readl(info, A1CR_OFFSET);
	if (tmp != regval) {
		dev_dbg(info->dev, "Warning: NAND config: Set A1CR " \
		       "reg to 0x%08x, was 0x%08x, should be done by " \
		       "bootloader.\n", regval, tmp);
		davinci_nand_writel(info, A1CR_OFFSET, regval);
	}
}

/*----------------------------------------------------------------------*/

static int __init nand_davinci_probe(struct platform_device *pdev)
{
	struct davinci_nand_pdata	*pdata = pdev->dev.platform_data;
	struct davinci_nand_info	*info;
	struct resource			*res1;
	struct resource			*res2;
	void __iomem			*vaddr;
	void __iomem			*base;
	int				ret;
	u32				val;
	nand_ecc_modes_t		ecc_mode;

	/* which external chipselect will we be managing? */
	if (pdev->id < 0 || pdev->id > 3)
		return -ENODEV;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info) {
		dev_err(&pdev->dev, "unable to allocate memory\n");
		ret = -ENOMEM;
		goto err_nomem;
	}

	platform_set_drvdata(pdev, info);

	res1 = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	res2 = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res1 || !res2) {
		dev_err(&pdev->dev, "resource missing\n");
		ret = -EINVAL;
		goto err_res;
	}

	vaddr = ioremap(res1->start, res1->end - res1->start);
	base = ioremap(res2->start, res2->end - res2->start);
	if (!vaddr || !base) {
		dev_err(&pdev->dev, "ioremap failed\n");
		ret = -EINVAL;
		goto err_ioremap;

	}

	info->dev		= &pdev->dev;
	info->base		= base;
	info->vaddr		= vaddr;

	info->mtd.priv		= &info->chip;
	info->mtd.name		= dev_name(&pdev->dev);
	info->mtd.owner		= THIS_MODULE;

	info->chip.IO_ADDR_R	= vaddr;
	info->chip.IO_ADDR_W	= vaddr;
	info->chip.chip_delay	= 0;
	info->chip.select_chip	= nand_davinci_select_chip;

	/* options such as NAND_USE_FLASH_BBT or 16-bit widths */
	info->chip.options	= pdata ? pdata->options : 0;

	info->ioaddr		= (u32 __force) vaddr;

	info->current_cs	= info->ioaddr;
	info->core_chipsel	= pdev->id;
	info->mask_chipsel	= pdata->mask_chipsel;

	/* use nandboot-capable ALE/CLE masks by default */
	if (pdata && pdata->mask_ale)
		info->mask_ale	= pdata->mask_cle;
	else
		info->mask_ale	= MASK_ALE;
	if (pdata && pdata->mask_cle)
		info->mask_cle	= pdata->mask_cle;
	else
		info->mask_cle	= MASK_CLE;

	/* Set address of hardware control function */
	info->chip.cmd_ctrl	= nand_davinci_hwcontrol;
	info->chip.dev_ready	= nand_davinci_dev_ready;

	/* Speed up buffer I/O */
	info->chip.read_buf     = nand_davinci_read_buf;
	info->chip.write_buf    = nand_davinci_write_buf;

	/* use board-specific ECC config; else, the best available */
	if (pdata)
		ecc_mode = pdata->ecc_mode;
	else if (cpu_is_davinci_dm355())
		ecc_mode = NAND_ECC_HW_SYNDROME;
	else
		ecc_mode = NAND_ECC_HW;

	/*
	 * REVISIT dm355 adds an ECC mode that corrects up to 4 error
	 * bits, using 10 ECC bytes every 512 bytes of data.  And that
	 * is what TI's original LSP uses... along with quite a hacked
	 * up "inline OOB" scheme storing those ECC bytes, which happens
	 * to use (in good blocks) bytes used by factory bad-block marks
	 * (in bad blocks).  There was evidently a technical issue (now
	 * fixed?):  Linux seemed to limit ECC data to 32 bytes.
	 */
	switch (ecc_mode) {
	case NAND_ECC_NONE:
	case NAND_ECC_SOFT:
		break;
	case NAND_ECC_HW:
		info->chip.ecc.calculate = nand_davinci_calculate_1bit;
		info->chip.ecc.correct = nand_davinci_correct_1bit;
		info->chip.ecc.hwctl = nand_davinci_hwctl_1bit;
		info->chip.ecc.size = 512;
		info->chip.ecc.bytes = 3;
		break;
	case NAND_ECC_HW_SYNDROME:
		/* FIXME implement */
		info->chip.ecc.size = 512;
		info->chip.ecc.bytes = 10;

		dev_warn(&pdev->dev, "4-bit ECC nyet supported\n");
		/* FALL THROUGH */
	default:
		ret = -EINVAL;
		goto err_ecc;
	}
	info->chip.ecc.mode = ecc_mode;

	info->clk = clk_get(&pdev->dev, "AEMIFCLK");
	if (IS_ERR(info->clk)) {
		ret = PTR_ERR(info->clk);
		dev_dbg(&pdev->dev, "unable to get AEMIFCLK, err %d\n", ret);
		goto err_clk;
	}

	ret = clk_enable(info->clk);
	if (ret < 0) {
		dev_dbg(&pdev->dev, "unable to enable AEMIFCLK, err %d\n", ret);
		goto err_clk_enable;
	}

	/* EMIF timings should normally be set by the boot loader,
	 * especially after boot-from-NAND.  The *only* reason to
	 * have this special casing for the DM6446 EVM is to work
	 * with boot-from-NOR ... with CS0 manually re-jumpered
	 * (after startup) so it addresses the NAND flash, not NOR.
	 * Even for dev boards, that's unusually rude...
	 */
	if (machine_is_davinci_evm())
		nand_dm6446evm_flash_init(info);

	spin_lock_irq(&davinci_nand_lock);

	/* put CSxNAND into NAND mode */
	val = davinci_nand_readl(info, NANDFCR_OFFSET);
	val |= BIT(info->core_chipsel);
	davinci_nand_writel(info, NANDFCR_OFFSET, val);

	spin_unlock_irq(&davinci_nand_lock);

	/* Scan to find existence of the device(s) */
	ret = nand_scan(&info->mtd, pdata->mask_chipsel ? 2 : 1);
	if (ret < 0) {
		dev_dbg(&pdev->dev, "no NAND chip(s) found\n");
		goto err_scan;
	}

	if (mtd_has_partitions()) {
		struct mtd_partition	*mtd_parts = NULL;
		int			mtd_parts_nb = 0;

		if (mtd_has_cmdlinepart()) {
			static const char *probes[] __initconst =
				{ "cmdlinepart", NULL };

			const char		*master_name;

			/* Set info->mtd.name = 0 temporarily */
			master_name		= info->mtd.name;
			info->mtd.name		= (char *)0;

			/* info->mtd.name == 0, means: don't bother checking
			   <mtd-id> */
			mtd_parts_nb = parse_mtd_partitions(&info->mtd, probes,
							    &mtd_parts, 0);

			/* Restore info->mtd.name */
			info->mtd.name = master_name;
		}

		if (mtd_parts_nb <= 0 && pdata) {
			mtd_parts = pdata->parts;
			mtd_parts_nb = pdata->nr_parts;
		}

		/* Register any partitions */
		if (mtd_parts_nb > 0) {
			ret = add_mtd_partitions(&info->mtd,
					mtd_parts, mtd_parts_nb);
			if (ret == 0)
				info->partitioned = true;
		}

	} else if (pdata && pdata->nr_parts) {
		dev_warn(&pdev->dev, "ignoring %d default partitions on %s\n",
				pdata->nr_parts, info->mtd.name);
	}

	/* If there's no partition info, just package the whole chip
	 * as a single MTD device.
	 *
	 * NOTE:  When using the DM355 with large block NAND chips, don't
	 * use this driver to change data the ROM Boot Loader (RBL) reads
	 * from one of the first 24 blocks.  See DM355 errata for details.
	 */
	if (!info->partitioned)
		ret = add_mtd_device(&info->mtd) ? -ENODEV : 0;

	if (ret < 0)
		goto err_scan;

	val = davinci_nand_readl(info, NRCSR_OFFSET);
	dev_info(&pdev->dev, "controller rev. %d.%d\n",
	       (val >> 8) & 0xff, val & 0xff);

	return 0;

err_scan:
	clk_disable(info->clk);

err_clk_enable:
	clk_put(info->clk);

err_ecc:
err_clk:
	if (base)
		iounmap(base);
	if (vaddr)
		iounmap(vaddr);

err_ioremap:
	kfree(info);

err_nomem:
err_res:
	return ret;
}

static int __exit nand_davinci_remove(struct platform_device *pdev)
{
	struct davinci_nand_info *info = platform_get_drvdata(pdev);
	int status;

	if (mtd_has_partitions() && info->partitioned)
		status = del_mtd_partitions(&info->mtd);
	else
		status = del_mtd_device(&info->mtd);

	iounmap(info->base);
	iounmap(info->vaddr);

	nand_release(&info->mtd);

	clk_disable(info->clk);
	clk_put(info->clk);

	kfree(info);

	return 0;
}

static struct platform_driver nand_davinci_driver = {
	.remove		= __exit_p(nand_davinci_remove),
	.driver		= {
		.name	= "davinci_nand",
	},
};
MODULE_ALIAS("platform:davinci_nand");

static int __init nand_davinci_init(void)
{
	return platform_driver_probe(&nand_davinci_driver, nand_davinci_probe);
}
module_init(nand_davinci_init);

static void __exit nand_davinci_exit(void)
{
	platform_driver_unregister(&nand_davinci_driver);
}
module_exit(nand_davinci_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Texas Instruments");
MODULE_DESCRIPTION("Davinci NAND flash driver");

