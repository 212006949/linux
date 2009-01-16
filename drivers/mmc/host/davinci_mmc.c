/*
 * davinci_mmc.c - TI DaVinci MMC controller file
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 * ----------------------------------------------------------------------------
 * Modifications:
 * ver. 1.0: Oct 2005, Purushotam Kumar   Initial version
 * ver 1.1:  Nov  2005, Purushotam Kumar  Solved bugs
 * ver 1.2:  Jan  2006, Purushotam Kumar   Added card remove insert support
 */

#include <linux/module.h>
#include <linux/ioport.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/mmc/host.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>

#include <mach/board.h>
#include <mach/cpu.h>
#include <mach/edma.h>
#include <mach/hardware.h>
#include <mach/irqs.h>

/*
 * Register Definitions
 */
#define DAVINCI_MMCCTL       0x00 /* Control Register                  */
#define DAVINCI_MMCCLK       0x04 /* Memory Clock Control Register     */
#define DAVINCI_MMCST0       0x08 /* Status Register 0                 */
#define DAVINCI_MMCST1       0x0C /* Status Register 1                 */
#define DAVINCI_MMCIM        0x10 /* Interrupt Mask Register           */
#define DAVINCI_MMCTOR       0x14 /* Response Time-Out Register        */
#define DAVINCI_MMCTOD       0x18 /* Data Read Time-Out Register       */
#define DAVINCI_MMCBLEN      0x1C /* Block Length Register             */
#define DAVINCI_MMCNBLK      0x20 /* Number of Blocks Register         */
#define DAVINCI_MMCNBLC      0x24 /* Number of Blocks Counter Register */
#define DAVINCI_MMCDRR       0x28 /* Data Receive Register             */
#define DAVINCI_MMCDXR       0x2C /* Data Transmit Register            */
#define DAVINCI_MMCCMD       0x30 /* Command Register                  */
#define DAVINCI_MMCARGHL     0x34 /* Argument Register                 */
#define DAVINCI_MMCRSP01     0x38 /* Response Register 0 and 1         */
#define DAVINCI_MMCRSP23     0x3C /* Response Register 0 and 1         */
#define DAVINCI_MMCRSP45     0x40 /* Response Register 0 and 1         */
#define DAVINCI_MMCRSP67     0x44 /* Response Register 0 and 1         */
#define DAVINCI_MMCDRSP      0x48 /* Data Response Register            */
#define DAVINCI_MMCETOK      0x4C
#define DAVINCI_MMCCIDX      0x50 /* Command Index Register            */
#define DAVINCI_MMCCKC       0x54
#define DAVINCI_MMCTORC      0x58
#define DAVINCI_MMCTODC      0x5C
#define DAVINCI_MMCBLNC      0x60
#define DAVINCI_SDIOCTL      0x64
#define DAVINCI_SDIOST0      0x68
#define DAVINCI_SDIOEN       0x6C
#define DAVINCI_SDIOST       0x70
#define DAVINCI_MMCFIFOCTL   0x74 /* FIFO Control Register             */

/* DAVINCI_MMCCTL definitions */
#define MMCCTL_DATRST         (1 << 0)
#define MMCCTL_CMDRST         (1 << 1)
#define MMCCTL_WIDTH_4_BIT    (1 << 2)
#define MMCCTL_DATEG_DISABLED (0 << 6)
#define MMCCTL_DATEG_RISING   (1 << 6)
#define MMCCTL_DATEG_FALLING  (2 << 6)
#define MMCCTL_DATEG_BOTH     (3 << 6)
#define MMCCTL_PERMDR_LE      (0 << 9)
#define MMCCTL_PERMDR_BE      (1 << 9)
#define MMCCTL_PERMDX_LE      (0 << 10)
#define MMCCTL_PERMDX_BE      (1 << 10)

/* DAVINCI_MMCCLK definitions */
#define MMCCLK_CLKEN          (1 << 8)
#define MMCCLK_CLKRT_MASK     (0xFF << 0)

/* IRQ bit definitions, for DAVINCI_MMCST0 and DAVINCI_MMCIM */
#define MMCST0_DATDNE         BIT(0)	/* data done */
#define MMCST0_BSYDNE         BIT(1)	/* busy done */
#define MMCST0_RSPDNE         BIT(2)	/* command done */
#define MMCST0_TOUTRD         BIT(3)	/* data read timeout */
#define MMCST0_TOUTRS         BIT(4)	/* command response timeout */
#define MMCST0_CRCWR          BIT(5)	/* data write CRC error */
#define MMCST0_CRCRD          BIT(6)	/* data read CRC error */
#define MMCST0_CRCRS          BIT(7)	/* command response CRC error */
#define MMCST0_DXRDY          BIT(9)	/* data transmit ready (fifo empty) */
#define MMCST0_DRRDY          BIT(10)	/* data receive ready (data in fifo)*/
#define MMCST0_DATED          BIT(11)	/* DAT3 edge detect */
#define MMCST0_TRNDNE         BIT(12)	/* transfer done */

/* DAVINCI_MMCST1 definitions */
#define MMCST1_BUSY           (1 << 0)

/* DAVINCI_MMCCMD definitions */
#define MMCCMD_CMD_MASK       (0x3F << 0)
#define MMCCMD_PPLEN          (1 << 7)
#define MMCCMD_BSYEXP         (1 << 8)
#define MMCCMD_RSPFMT_MASK    (3 << 9)
#define MMCCMD_RSPFMT_NONE    (0 << 9)
#define MMCCMD_RSPFMT_R1456   (1 << 9)
#define MMCCMD_RSPFMT_R2      (2 << 9)
#define MMCCMD_RSPFMT_R3      (3 << 9)
#define MMCCMD_DTRW           (1 << 11)
#define MMCCMD_STRMTP         (1 << 12)
#define MMCCMD_WDATX          (1 << 13)
#define MMCCMD_INITCK         (1 << 14)
#define MMCCMD_DCLR           (1 << 15)
#define MMCCMD_DMATRIG        (1 << 16)

/* DAVINCI_MMCFIFOCTL definitions */
#define MMCFIFOCTL_FIFORST    (1 << 0)
#define MMCFIFOCTL_FIFODIR_WR (1 << 1)
#define MMCFIFOCTL_FIFODIR_RD (0 << 1)
#define MMCFIFOCTL_FIFOLEV    (1 << 2) /* 0 = 128 bits, 1 = 256 bits */
#define MMCFIFOCTL_ACCWD_4    (0 << 3) /* access width of 4 bytes    */
#define MMCFIFOCTL_ACCWD_3    (1 << 3) /* access width of 3 bytes    */
#define MMCFIFOCTL_ACCWD_2    (2 << 3) /* access width of 2 bytes    */
#define MMCFIFOCTL_ACCWD_1    (3 << 3) /* access width of 1 byte     */


/* MMCSD Init clock in Hz in opendain mode */
#define MMCSD_INIT_CLOCK		200000
#define DRIVER_NAME			"davinci_mmc"

/*
 * One scatterlist dma "segment" is at most MAX_CCNT rw_threshold units,
 * and we handle up to NR_SG segments.  MMC_BLOCK_BOUNCE kicks in only
 * for drivers with max_hw_segs == 1, making the segments bigger (64KB)
 * than the page or two that's otherwise typical.
 *
 * FIXME make NR_SG = 16 behave to get the same throughput boost from
 * EDMA transfer linkage (for read *AND* write) but without extra page
 * copies.  The existing code hard-wires a *single* transfer link, so
 * it will behave poorly on typical segments (one or two pages); and
 * looks rather dubious.
 */
#define MAX_CCNT	((1 << 16) - 1)

#define NR_SG		1

static unsigned rw_threshold = 32;
module_param(rw_threshold, uint, S_IRUGO);
MODULE_PARM_DESC(rw_threshold,
		"Read/Write threshold, can be 16/32. Default = 32");

static unsigned __initdata use_dma = 1;
module_param(use_dma, uint, 0);
MODULE_PARM_DESC(use_dma, "Whether to use DMA or not. Default = 1");

struct mmc_davinci_host {
	struct mmc_command *cmd;
	struct mmc_data *data;
	struct mmc_host *mmc;
	struct clk *clk;
	unsigned int mmc_input_clk;
	void __iomem *base;
	struct resource *mem_res;
	int irq;
	unsigned char bus_mode;

#define DAVINCI_MMC_DATADIR_NONE	0
#define DAVINCI_MMC_DATADIR_READ	1
#define DAVINCI_MMC_DATADIR_WRITE	2
	unsigned char data_dir;

	/* buffer is used during PIO of one scatterlist segment, and
	 * is updated along with buffer_bytes_left.  bytes_left applies
	 * to all N blocks of the PIO transfer.
	 */
	u8 *buffer;
	u32 buffer_bytes_left;
	u32 bytes_left;

	u8 rxdma, txdma;
	bool use_dma;
	bool do_dma;

	/* Scatterlist DMA uses one or more parameter RAM entries:
	 * the main one (associated with rxdma or txdma) plus zero or
	 * more links.  The entries for a given transfer differ only
	 * by memory buffer (address, length) and link field.
	 */
	struct edmacc_param	tx_template;
	struct edmacc_param	rx_template;

	/* For PIO we walk scatterlists one segment at a time. */
	unsigned int		sg_len;
	int			sg_idx;
};


#define DAVINCI_MMCSD_READ_FIFO(pDst, pRegs, cnt) asm( \
	"	cmp	%3,#16\n" \
	"1:	ldrhs	r0,[%1,%2]\n" \
	"	ldrhs	r1,[%1,%2]\n" \
	"	ldrhs	r2,[%1,%2]\n" \
	"	ldrhs	r3,[%1,%2]\n" \
	"	stmhsia	%0!,{r0,r1,r2,r3}\n" \
	"	beq	3f\n" \
	"	subhs	%3,%3,#16\n" \
	"	cmp	%3,#16\n" \
	"	bhs	1b\n" \
	"	tst	%3,#0x0c\n" \
	"2:	ldrne	r0,[%1,%2]\n" \
	"	strne	r0,[%0],#4\n" \
	"	subne	%3,%3,#4\n" \
	"	tst	%3,#0x0c\n" \
	"	bne	2b\n" \
	"	tst	%3,#2\n" \
	"	ldrneh	r0,[%1,%2]\n" \
	"	strneh	r0,[%0],#2\n" \
	"	tst	%3,#1\n" \
	"	ldrneb	r0,[%1,%2]\n" \
	"	strneb	r0,[%0],#1\n" \
	"3:\n" \
	 : "+r"(pDst) : "r"(pRegs), "i"(DAVINCI_MMCDRR), \
	 "r"(cnt) : "r0", "r1", "r2", "r3");

#define DAVINCI_MMCSD_WRITE_FIFO(pDst, pRegs, cnt) asm( \
	"	cmp	%3,#16\n" \
	"1:	ldmhsia	%0!,{r0,r1,r2,r3}\n" \
	"	strhs	r0,[%1,%2]\n" \
	"	strhs	r1,[%1,%2]\n" \
	"	strhs	r2,[%1,%2]\n" \
	"	strhs	r3,[%1,%2]\n" \
	"	beq	3f\n" \
	"	subhs	%3,%3,#16\n" \
	"	cmp	%3,#16\n" \
	"	bhs	1b\n" \
	"	tst	%3,#0x0c\n" \
	"2:	ldrne	r0,[%0],#4\n" \
	"	strne	r0,[%1,%2]\n" \
	"	subne	%3,%3,#4\n" \
	"	tst	%3,#0x0c\n" \
	"	bne	2b\n" \
	"	tst	%3,#2\n" \
	"	ldrneh	r0,[%0],#2\n" \
	"	strneh	r0,[%1,%2]\n" \
	"	tst	%3,#1\n" \
	"	ldrneb	r0,[%0],#1\n" \
	"	strneb	r0,[%1,%2]\n" \
	"3:\n" \
	 : "+r"(pDst) : "r"(pRegs), "i"(DAVINCI_MMCDXR), \
	 "r"(cnt) : "r0", "r1", "r2", "r3");

/* PIO only */
static void mmc_davinci_sg_to_buf(struct mmc_davinci_host *host)
{
	struct scatterlist *sg;

	sg = host->data->sg + host->sg_idx;
	host->buffer_bytes_left = sg_dma_len(sg);
	host->buffer = sg_virt(sg);
	if (host->buffer_bytes_left > host->bytes_left)
		host->buffer_bytes_left = host->bytes_left;
}

static void davinci_fifo_data_trans(struct mmc_davinci_host *host, int n)
{
	u8 *p;

	if (host->buffer_bytes_left == 0) {
		host->sg_idx++;
		BUG_ON(host->sg_idx == host->sg_len);
		mmc_davinci_sg_to_buf(host);
	}

	p = host->buffer;
	if (n > host->buffer_bytes_left)
		n = host->buffer_bytes_left;
	host->buffer_bytes_left -= n;
	host->bytes_left -= n;

	/* NOTE:  we never transfer more than rw_threshold bytes
	 * to/from the fifo here; there's no I/O overlap.
	 */
	if (host->data_dir == DAVINCI_MMC_DATADIR_WRITE) {
		DAVINCI_MMCSD_WRITE_FIFO(p, host->base, n);
	} else {
		DAVINCI_MMCSD_READ_FIFO(p, host->base, n);
	}
	host->buffer = p;
}

static void mmc_davinci_start_command(struct mmc_davinci_host *host,
		struct mmc_command *cmd)
{
	u32 cmd_reg = 0;
	u32 im_val;

	dev_dbg(mmc_dev(host->mmc), "CMD%d, arg 0x%08x%s\n",
		cmd->opcode, cmd->arg,
		({ char *s;
		switch (mmc_resp_type(cmd)) {
		case MMC_RSP_R1:
			s = ", R1/R5/R6/R7 response";
			break;
		case MMC_RSP_R1B:
			s = ", R1b response";
			break;
		case MMC_RSP_R2:
			s = ", R2 response";
			break;
		case MMC_RSP_R3:
			s = ", R3/R4 response";
			break;
		default:
			s = ", (R? response)";
			break;
		}; s; }));
	host->cmd = cmd;

	switch (mmc_resp_type(cmd)) {
	case MMC_RSP_R1B:
		/* There's some spec confusion about when R1B is
		 * allowed, but if the card doesn't issue a BUSY
		 * then it's harmless for us to allow it.
		 */
		cmd_reg |= MMCCMD_BSYEXP;
		/* FALLTHROUGH */
	case MMC_RSP_R1:		/* 48 bits, CRC */
		cmd_reg |= MMCCMD_RSPFMT_R1456;
		break;
	case MMC_RSP_R2:		/* 136 bits, CRC */
		cmd_reg |= MMCCMD_RSPFMT_R2;
		break;
	case MMC_RSP_R3:		/* 48 bits, no CRC */
		cmd_reg |= MMCCMD_RSPFMT_R3;
		break;
	default:
		cmd_reg |= MMCCMD_RSPFMT_NONE;
		dev_dbg(mmc_dev(host->mmc), "unknown resp_type %04x\n",
			mmc_resp_type(cmd));
		break;
	}

	/* Set command index */
	cmd_reg |= cmd->opcode;

	/* Setting initialize clock */
	if (cmd->opcode == 0)
		cmd_reg |= MMCCMD_INITCK;

	/* Enable EDMA transfer triggers */
	if (host->do_dma)
		cmd_reg |= MMCCMD_DMATRIG;

	/* Setting whether command involves data transfer or not */
	if (cmd->data)
		cmd_reg |= MMCCMD_WDATX;

	/* Setting whether stream or block transfer */
	if (cmd->flags & MMC_DATA_STREAM)
		cmd_reg |= MMCCMD_STRMTP;

	/* Setting whether data read or write */
	if (host->data_dir == DAVINCI_MMC_DATADIR_WRITE)
		cmd_reg |= MMCCMD_DTRW;

	if (host->bus_mode == MMC_BUSMODE_PUSHPULL)
		cmd_reg |= MMCCMD_PPLEN;

	/* set Command timeout */
	writel(0xFFFF, host->base + DAVINCI_MMCTOR);

	/* Enable interrupt (calculate here, defer until FIFO is stuffed). */
	im_val =  MMCST0_RSPDNE | MMCST0_CRCRS | MMCST0_TOUTRS;
	if (host->data_dir == DAVINCI_MMC_DATADIR_WRITE) {
		im_val |= MMCST0_DATDNE | MMCST0_CRCWR;

		if (!host->do_dma)
			im_val |= MMCST0_DXRDY;
	} else if (host->data_dir == DAVINCI_MMC_DATADIR_READ) {
		im_val |= MMCST0_DATDNE | MMCST0_CRCRD | MMCST0_TOUTRD;

		if (!host->do_dma)
			im_val |= MMCST0_DRRDY;
	}

	/*
	 * Before non-DMA WRITE commands the controller needs priming:
	 * FIFO should be populated with 32 bytes
	 */
	if (!host->do_dma && (host->data_dir == DAVINCI_MMC_DATADIR_WRITE))
		davinci_fifo_data_trans(host, 32);

	writel(cmd->arg, host->base + DAVINCI_MMCARGHL);
	writel(cmd_reg,  host->base + DAVINCI_MMCCMD);
	writel(im_val, host->base + DAVINCI_MMCIM);
}

static void davinci_abort_dma(struct mmc_davinci_host *host)
{
	int sync_dev = 0;

	if (host->data_dir == DAVINCI_MMC_DATADIR_READ)
		sync_dev = host->rxdma;
	else
		sync_dev = host->txdma;

	davinci_stop_dma(sync_dev);
	davinci_clean_channel(sync_dev);
}

static void mmc_davinci_dma_cb(int lch, u16 ch_status, void *data)
{
	if (DMA_COMPLETE != ch_status) {
		struct mmc_davinci_host *host = data;

		/* Currently means:  DMA Event Missed, or "null" transfer
		 * request was seen.  In the future, TC errors (like bad
		 * addresses) might be presented too.
		 */
		dev_warn(mmc_dev(host->mmc), "DMA %s error\n",
			(host->data->flags & MMC_DATA_WRITE)
				? "write" : "read");
		davinci_abort_dma(host);
		host->data->error = -EIO;
	}
}

/* Set up tx or rx template, to be modified and updated later */
static void __init mmc_davinci_dma_setup(struct mmc_davinci_host *host,
		bool tx, struct edmacc_param *template)
{
	int		sync_dev;
	const u16	acnt = 4;
	const u16	bcnt = rw_threshold >> 2;
	const u16	ccnt = 0;
	u32		src_port = 0;
	u32		dst_port = 0;
	s16		src_bidx, dst_bidx;
	s16		src_cidx, dst_cidx;

	/*
	 * A-B Sync transfer:  each DMA request is for one "frame" of
	 * rw_threshold bytes, broken into "acnt"-size chunks repeated
	 * "bcnt" times.  Each segment needs "ccnt" such frames; since
	 * we tell the block layer our mmc->max_seg_size limit, we can
	 * trust (later) that it's within bounds.
	 *
	 * The FIFOs are read/written in 4-byte chunks (acnt == 4) and
	 * EDMA will optimize memory operations to use larger bursts.
	 */
	if (tx) {
		sync_dev = host->txdma;

		/* src_prt, ccnt, and link to be set up later */
		src_bidx = acnt;
		src_cidx = acnt * bcnt;

		dst_port = host->mem_res->start + DAVINCI_MMCDXR;
		dst_bidx = 0;
		dst_cidx = 0;
	} else {
		sync_dev = host->rxdma;

		src_port = host->mem_res->start + DAVINCI_MMCDRR;
		src_bidx = 0;
		src_cidx = 0;

		/* dst_prt, ccnt, and link to be set up later */
		dst_bidx = acnt;
		dst_cidx = acnt * bcnt;
	}

	/*
	 * We can't use FIFO mode for the FIFOs because MMC FIFO addresses
	 * are not 256-bit (32-byte) aligned.  So we use INCR, and the W8BIT
	 * parameter is ignored.
	 */
	davinci_set_dma_src_params(sync_dev, src_port, INCR, W8BIT);
	davinci_set_dma_dest_params(sync_dev, dst_port, INCR, W8BIT);

	davinci_set_dma_src_index(sync_dev, src_bidx, src_cidx);
	davinci_set_dma_dest_index(sync_dev, dst_bidx, dst_cidx);

	davinci_set_dma_transfer_params(sync_dev, acnt, bcnt, ccnt, 8, ABSYNC);

	davinci_get_dma_params(sync_dev, template);

	/* don't bother with irqs or chaining */
	template->opt &= ~(ITCCHEN | TCCHEN | ITCINTEN | TCINTEN);
}

static int mmc_davinci_send_dma_request(struct mmc_davinci_host *host,
		struct mmc_request *req)
{
	struct edmacc_param	regs;
	struct mmc_data		*data = host->data;
	struct scatterlist	*sg = &data->sg[0];
	unsigned		count = sg_dma_len(sg);
	int			lch;

	/* If this scatterlist segment (e.g. one page) is bigger than
	 * the transfer (e.g. a block) don't use the whole segment.
	 */
	if (count > host->bytes_left)
		count = host->bytes_left;

	/* Update the fields in "regs" that change, and write
	 * them to EDMA parameter RAM
	 */
	if (host->data_dir == DAVINCI_MMC_DATADIR_WRITE) {
		lch = host->txdma;
		regs = host->tx_template;
		regs.src = sg_dma_address(sg);
	} else {
		lch = host->rxdma;
		regs = host->rx_template;
		regs.dst = sg_dma_address(sg);
	}
	regs.ccnt = count >> ((rw_threshold == 32) ? 5 : 4);
	davinci_set_dma_params(lch, &regs);

	davinci_start_dma(lch);
	return 0;
}

static int mmc_davinci_start_dma_transfer(struct mmc_davinci_host *host,
		struct mmc_request *req)
{
	int i;
	struct mmc_data *data = host->data;
	int mask = rw_threshold - 1;

	host->sg_len = dma_map_sg(mmc_dev(host->mmc), data->sg, data->sg_len,
				((data->flags & MMC_DATA_WRITE)
				? DMA_TO_DEVICE
				: DMA_FROM_DEVICE));

	/* no individual DMA segment should need a partial FIFO */
	for (i = 0; i < host->sg_len; i++) {
		if (sg_dma_len(data->sg + i) & mask) {
			dma_unmap_sg(mmc_dev(host->mmc),
					data->sg, data->sg_len,
					(data->flags & MMC_DATA_WRITE)
					? DMA_TO_DEVICE
					: DMA_FROM_DEVICE);
			return -1;
		}
	}

	host->do_dma = 1;
	mmc_davinci_send_dma_request(host, req);

	return 0;
}

static void __init_or_module
davinci_release_dma_channels(struct mmc_davinci_host *host)
{
	if (!host->use_dma)
		return;

	davinci_free_dma(host->txdma);
	davinci_free_dma(host->rxdma);
}

static int __init davinci_acquire_dma_channels(struct mmc_davinci_host *host)
{
	const char		*hostname = mmc_hostname(host->mmc);
	int			edma_chan_num, tcc = 0, r;
	enum dma_event_q	queue_no = EVENTQ_0;

	/* Acquire master DMA write channel */
	r = davinci_request_dma(host->txdma, hostname,
		mmc_davinci_dma_cb, host, &edma_chan_num, &tcc, queue_no);
	if (r != 0) {
		dev_warn(mmc_dev(host->mmc),
				"MMC: davinci_request_dma() failed with %d\n",
				r);
		return r;
	}
	mmc_davinci_dma_setup(host, true, &host->tx_template);

	/* Acquire master DMA read channel */
	r = davinci_request_dma(host->rxdma, hostname,
		mmc_davinci_dma_cb, host, &edma_chan_num, &tcc, queue_no);
	if (r != 0) {
		dev_warn(mmc_dev(host->mmc),
				"MMC: davinci_request_dma() failed with %d\n",
				r);
		goto free_master_write;
	}
	mmc_davinci_dma_setup(host, false, &host->rx_template);

	return 0;

free_master_write:
	davinci_free_dma(host->txdma);

	return r;
}

static void
mmc_davinci_prepare_data(struct mmc_davinci_host *host, struct mmc_request *req)
{
	int fifo_lev = (rw_threshold == 32) ? MMCFIFOCTL_FIFOLEV : 0;
	int timeout;

	host->data = req->data;
	if (req->data == NULL) {
		host->data_dir = DAVINCI_MMC_DATADIR_NONE;
		writel(0, host->base + DAVINCI_MMCBLEN);
		writel(0, host->base + DAVINCI_MMCNBLK);
		return;
	}

	/* Init idx */
	host->sg_idx = 0;

	dev_dbg(mmc_dev(host->mmc),
		"MMCSD : Data xfer (%s %s), "
		"DTO %d cycles + %d ns, %d blocks of %d bytes\n",
		(req->data->flags & MMC_DATA_STREAM) ? "stream" : "block",
		(req->data->flags & MMC_DATA_WRITE) ? "write" : "read",
		req->data->timeout_clks, req->data->timeout_ns,
		req->data->blocks, req->data->blksz);

	/* Convert ns to clock cycles by assuming 20MHz frequency
	 * 1 cycle at 20MHz = 500 ns
	 */
	timeout = req->data->timeout_clks + req->data->timeout_ns / 500;
	if (timeout > 0xffff)
		timeout = 0xffff;

	writel(timeout, host->base + DAVINCI_MMCTOD);
	writel(req->data->blocks, host->base + DAVINCI_MMCNBLK);
	writel(req->data->blksz, host->base + DAVINCI_MMCBLEN);
	host->data_dir = (req->data->flags & MMC_DATA_WRITE)
			? DAVINCI_MMC_DATADIR_WRITE
			: DAVINCI_MMC_DATADIR_READ;

	/* Configure the FIFO */
	switch (host->data_dir) {
	case DAVINCI_MMC_DATADIR_WRITE:
		writel(fifo_lev | MMCFIFOCTL_FIFODIR_WR | MMCFIFOCTL_FIFORST,
			host->base + DAVINCI_MMCFIFOCTL);
		writel(fifo_lev | MMCFIFOCTL_FIFODIR_WR,
			host->base + DAVINCI_MMCFIFOCTL);
		break;

	case DAVINCI_MMC_DATADIR_READ:
		writel(fifo_lev | MMCFIFOCTL_FIFODIR_RD | MMCFIFOCTL_FIFORST,
			host->base + DAVINCI_MMCFIFOCTL);
		writel(fifo_lev | MMCFIFOCTL_FIFODIR_RD,
			host->base + DAVINCI_MMCFIFOCTL);
		break;
	default:
		break;
	}

	host->sg_len = req->data->sg_len;
	host->bytes_left = req->data->blocks * req->data->blksz;

	/* For now we try to use DMA whenever we won't need partial FIFO
	 * reads or writes, either for the whole transfer (as tested here)
	 * or for any individual scatterlist segment (tested when we call
	 * start_dma_transfer).
	 *
	 * While we *could* change that, unusual block sizes are rarely
	 * used.  The occasional fallback to PIO should't hurt.
	 */
	if (host->use_dma && (host->bytes_left & (rw_threshold - 1)) == 0
			&& mmc_davinci_start_dma_transfer(host, req) == 0) {
		host->buffer = NULL;
		host->bytes_left = 0;
	} else {
		/* Revert to CPU Copy */
		mmc_davinci_sg_to_buf(host);
	}
}

static void mmc_davinci_request(struct mmc_host *mmc, struct mmc_request *req)
{
	struct mmc_davinci_host *host = mmc_priv(mmc);
	unsigned long timeout = jiffies + msecs_to_jiffies(900);
	u32 mmcst1 = 0;

	/* Card may still be sending BUSY after a previous operation,
	 * typically some kind of write.  If so, we can't proceed yet.
	 */
	while (time_before(jiffies, timeout)) {
		mmcst1  = readl(host->base + DAVINCI_MMCST1);
		if (!(mmcst1 & MMCST1_BUSY))
			break;
		cpu_relax();
	}
	if (mmcst1 & MMCST1_BUSY) {
		dev_err(mmc_dev(host->mmc), "still BUSY? bad ... \n");
		req->cmd->error = -ETIMEDOUT;
		mmc_request_done(mmc, req);
		return;
	}

	host->do_dma = 0;
	mmc_davinci_prepare_data(host, req);
	mmc_davinci_start_command(host, req->cmd);
}

static unsigned int calculate_freq_for_card(struct mmc_davinci_host *host,
	unsigned int mmc_req_freq)
{
	unsigned int mmc_freq = 0, cpu_arm_clk = 0, mmc_push_pull = 0;

	cpu_arm_clk = host->mmc_input_clk;
	if (cpu_arm_clk > (2 * mmc_req_freq))
		mmc_push_pull = ((unsigned int)cpu_arm_clk
				/ (2 * mmc_req_freq)) - 1;
	else
		mmc_push_pull = 0;

	mmc_freq = (unsigned int)cpu_arm_clk / (2 * (mmc_push_pull + 1));

	if (mmc_freq > mmc_req_freq)
		mmc_push_pull = mmc_push_pull + 1;

	return mmc_push_pull;
}

static void mmc_davinci_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	unsigned int open_drain_freq = 0, cpu_arm_clk = 0;
	unsigned int mmc_push_pull_freq = 0;
	struct mmc_davinci_host *host = mmc_priv(mmc);

	cpu_arm_clk = host->mmc_input_clk;
	dev_dbg(mmc_dev(host->mmc),
		"clock %dHz busmode %d powermode %d Vdd %04x\n",
		ios->clock, ios->bus_mode, ios->power_mode,
		ios->vdd);
	if (ios->bus_width == MMC_BUS_WIDTH_4) {
		dev_dbg(mmc_dev(host->mmc), "Enabling 4 bit mode\n");
		writel(readl(host->base + DAVINCI_MMCCTL) | MMCCTL_WIDTH_4_BIT,
			host->base + DAVINCI_MMCCTL);
	} else {
		dev_dbg(mmc_dev(host->mmc), "Disabling 4 bit mode\n");
		writel(readl(host->base + DAVINCI_MMCCTL) & ~MMCCTL_WIDTH_4_BIT,
			host->base + DAVINCI_MMCCTL);
	}

	if (ios->bus_mode == MMC_BUSMODE_OPENDRAIN) {
		u32 temp;
		open_drain_freq = ((unsigned int)cpu_arm_clk
				/ (2 * MMCSD_INIT_CLOCK)) - 1;
		temp = readl(host->base + DAVINCI_MMCCLK) & ~0xFF;
		temp |= open_drain_freq;
		writel(temp, host->base + DAVINCI_MMCCLK);
	} else {
		u32 temp;
		mmc_push_pull_freq = calculate_freq_for_card(host, ios->clock);

		temp = readl(host->base + DAVINCI_MMCCLK) & ~MMCCLK_CLKEN;
		writel(temp, host->base + DAVINCI_MMCCLK);

		udelay(10);

		temp = readl(host->base + DAVINCI_MMCCLK) & ~MMCCLK_CLKRT_MASK;
		temp |= mmc_push_pull_freq;
		writel(temp, host->base + DAVINCI_MMCCLK);

		writel(temp | MMCCLK_CLKEN, host->base + DAVINCI_MMCCLK);

		udelay(10);
	}

	host->bus_mode = ios->bus_mode;
	if (ios->power_mode == MMC_POWER_UP) {
		/* Send clock cycles, poll completion */
		writel(0, host->base + DAVINCI_MMCARGHL);
		writel(MMCCMD_INITCK, host->base + DAVINCI_MMCCMD);
		while (!(readl(host->base + DAVINCI_MMCST0) & MMCST0_RSPDNE))
			cpu_relax();
	}

	/* FIXME on power OFF, reset things ... */
}

static void
mmc_davinci_xfer_done(struct mmc_davinci_host *host, struct mmc_data *data)
{
	host->data = NULL;
	host->data_dir = DAVINCI_MMC_DATADIR_NONE;

	if (host->do_dma) {
		davinci_abort_dma(host);

		dma_unmap_sg(mmc_dev(host->mmc), data->sg, host->sg_len,
			     (data->flags & MMC_DATA_WRITE)
			     ? DMA_TO_DEVICE
			     : DMA_FROM_DEVICE);
		host->do_dma = false;
	}

	if (!data->stop || (host->cmd && host->cmd->error)) {
		mmc_request_done(host->mmc, data->mrq);
		writel(0, host->base + DAVINCI_MMCIM);
	} else
		mmc_davinci_start_command(host, data->stop);
}

static void mmc_davinci_cmd_done(struct mmc_davinci_host *host,
				 struct mmc_command *cmd)
{
	host->cmd = NULL;

	if (!cmd) {
		dev_warn(mmc_dev(host->mmc),
			"%s(): No cmd ptr\n", __func__);
		return;
	}

	if (cmd->flags & MMC_RSP_PRESENT) {
		if (cmd->flags & MMC_RSP_136) {
			/* response type 2 */
			cmd->resp[3] = readl(host->base + DAVINCI_MMCRSP01);
			cmd->resp[2] = readl(host->base + DAVINCI_MMCRSP23);
			cmd->resp[1] = readl(host->base + DAVINCI_MMCRSP45);
			cmd->resp[0] = readl(host->base + DAVINCI_MMCRSP67);
		} else {
			/* response types 1, 1b, 3, 4, 5, 6 */
			cmd->resp[0] = readl(host->base + DAVINCI_MMCRSP67);
		}
	}

	if (host->data == NULL || cmd->error) {
		if (cmd->error == -ETIMEDOUT)
			cmd->mrq->cmd->retries = 0;
		mmc_request_done(host->mmc, cmd->mrq);
		writel(0, host->base + DAVINCI_MMCIM);
	}
}

static inline int handle_core_command(
		struct mmc_davinci_host *host, unsigned int status)
{
	int end_command = 0;
	int end_transfer = 0;
	unsigned int qstatus = status;
	struct mmc_data *data = host->data;

	/* handle FIFO first when using PIO for data */
	while (host->bytes_left && (status & (MMCST0_DXRDY | MMCST0_DRRDY))) {
		davinci_fifo_data_trans(host, rw_threshold);
		status = readl(host->base + DAVINCI_MMCST0);
		if (!status)
			break;
		qstatus |= status;
	}

	if (qstatus & MMCST0_DATDNE) {
		/* All blocks sent/received, and CRC checks passed */
		if (data != NULL) {
			if ((host->do_dma == 0) && (host->bytes_left > 0)) {
				/* if datasize < rw_threshold
				 * no RX ints are generated
				 */
				davinci_fifo_data_trans(host, host->bytes_left);
			}
			end_transfer = 1;
			data->bytes_xfered += data->blocks * data->blksz;
		} else {
			dev_warn(mmc_dev(host->mmc), "TC:host->data is NULL\n");
		}
	}

	if (qstatus & MMCST0_TOUTRD) {
		/* Read data timeout */
		data->error = -ETIMEDOUT;
		end_transfer = 1;

		/* REVISIT report *actual* bytecount on errors */

		dev_dbg(mmc_dev(host->mmc),
			"read data timeout, status %x\n",
			qstatus);
	}

	if (qstatus & (MMCST0_CRCWR | MMCST0_CRCRD)) {
		u32 temp;

		/* DAT line portion is disabled and in reset state */
		temp = readl(host->base + DAVINCI_MMCCTL);
		writel(temp | MMCCTL_CMDRST,
			host->base + DAVINCI_MMCCTL);
		udelay(10);
		writel(temp & ~MMCCTL_CMDRST,
			host->base + DAVINCI_MMCCTL);

		/* Data CRC error */
		data->error = -EILSEQ;
		end_transfer = 1;

		/* REVISIT report *actual* bytecount on errors */

		/* NOTE:  this controller uses CRCWR to report both CRC
		 * errors and timeouts (on writes).  MMCDRSP values are
		 * only weakly documented, but 0x9f was clearly a timeout
		 * case and the two three-bit patterns in various SD specs
		 * (101, 010) aren't part of it ...
		 */
		if (qstatus & MMCST0_CRCWR) {
			temp = readb(host->base + DAVINCI_MMCDRSP);
			if (temp == 0x9f)
				data->error = -ETIMEDOUT;
		}
		dev_dbg(mmc_dev(host->mmc), "data %s %s error\n",
			(qstatus & MMCST0_CRCWR) ? "write" : "read",
			(data->error == -ETIMEDOUT) ? "timeout" : "CRC");
	}

	if (qstatus & MMCST0_TOUTRS) {
		/* Command timeout */
		if (host->cmd) {
			dev_dbg(mmc_dev(host->mmc), "MMCSD: CMD%d "
				"timeout, status %x\n",
				host->cmd->opcode, qstatus);
			host->cmd->error = -ETIMEDOUT;
			if (data)
				end_transfer = 1;
			else
				end_command = 1;
		}
	}

	if (qstatus & MMCST0_CRCRS) {
		/* Command CRC error */
		dev_dbg(mmc_dev(host->mmc), "Command CRC error\n");
		if (host->cmd) {
			/* Ignore CMD CRC errors during high speed operation */
			if (host->mmc->ios.clock <= 25000000)
				host->cmd->error = -EILSEQ;
			end_command = 1;
		}
	}

	if (qstatus & MMCST0_RSPDNE) {
		/* End of command phase */
		end_command = 1;
	}

	if (end_command)
		mmc_davinci_cmd_done(host, host->cmd);
	if (end_transfer)
		mmc_davinci_xfer_done(host, data);
	return 0;
}

static irqreturn_t mmc_davinci_irq(int irq, void *dev_id)
{
	struct mmc_davinci_host *host = (struct mmc_davinci_host *)dev_id;
	unsigned int status;

	if (host->cmd == NULL && host->data == NULL) {
		status = readl(host->base + DAVINCI_MMCST0);
		dev_dbg(mmc_dev(host->mmc),
			"Spurious interrupt 0x%04x\n", status);
		/* Disable the interrupt from mmcsd */
		writel(0, host->base + DAVINCI_MMCIM);
		return IRQ_HANDLED;
	}

	do {
		status = readl(host->base + DAVINCI_MMCST0);
		if (status == 0)
			break;
		if (handle_core_command(host, status))
			break;
	} while (1);
	return IRQ_HANDLED;
}

static int mmc_davinci_get_cd(struct mmc_host *mmc)
{
	struct platform_device *pdev = to_platform_device(mmc->parent);
	struct davinci_mmc_config *config = pdev->dev.platform_data;

	if (!config || !config->get_cd)
		return -ENOSYS;
	return config->get_cd(pdev->id);
}

static int mmc_davinci_get_ro(struct mmc_host *mmc)
{
	struct platform_device *pdev = to_platform_device(mmc->parent);
	struct davinci_mmc_config *config = pdev->dev.platform_data;

	if (!config || !config->get_ro)
		return -ENOSYS;
	return config->get_ro(pdev->id);
}

static struct mmc_host_ops mmc_davinci_ops = {
	.request	= mmc_davinci_request,
	.set_ios	= mmc_davinci_set_ios,
	.get_cd		= mmc_davinci_get_cd,
	.get_ro		= mmc_davinci_get_ro,
};

static void __init init_mmcsd_host(struct mmc_davinci_host *host)
{
	/* DAT line portion is diabled and in reset state */
	writel(readl(host->base + DAVINCI_MMCCTL) | MMCCTL_DATRST,
		host->base + DAVINCI_MMCCTL);

	/* CMD line portion is diabled and in reset state */
	writel(readl(host->base + DAVINCI_MMCCTL) | MMCCTL_CMDRST,
		host->base + DAVINCI_MMCCTL);

	udelay(10);

	writel(0, host->base + DAVINCI_MMCCLK);
	writel(MMCCLK_CLKEN, host->base + DAVINCI_MMCCLK);

	writel(0xFFFF, host->base + DAVINCI_MMCTOR);
	writel(0xFFFF, host->base + DAVINCI_MMCTOD);

	writel(readl(host->base + DAVINCI_MMCCTL) & ~MMCCTL_DATRST,
		host->base + DAVINCI_MMCCTL);
	writel(readl(host->base + DAVINCI_MMCCTL) & ~MMCCTL_CMDRST,
		host->base + DAVINCI_MMCCTL);

	udelay(10);
}

static int __init davinci_mmcsd_probe(struct platform_device *pdev)
{
	struct davinci_mmc_config *pdata = pdev->dev.platform_data;
	struct mmc_davinci_host *host = NULL;
	struct mmc_host *mmc = NULL;
	struct resource *r, *mem = NULL;
	int ret = 0, irq = 0;
	size_t mem_size;

	/* REVISIT:  when we're fully converted, fail if pdata is NULL */

	ret = -ENODEV;
	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	irq = platform_get_irq(pdev, 0);
	if (!r || irq == NO_IRQ)
		goto out;

	ret = -EBUSY;
	mem_size = r->end - r->start + 1;
	mem = request_mem_region(r->start, mem_size, DRIVER_NAME);
	if (!mem)
		goto out;

	ret = -ENOMEM;
	mmc = mmc_alloc_host(sizeof(struct mmc_davinci_host), &pdev->dev);
	if (!mmc)
		goto out;

	host = mmc_priv(mmc);
	host->mmc = mmc;	/* Important */

	r = platform_get_resource(pdev, IORESOURCE_DMA, 0);
	if (!r)
		goto out;
	host->rxdma = r->start;

	r = platform_get_resource(pdev, IORESOURCE_DMA, 1);
	if (!r)
		goto out;
	host->txdma = r->start;

	host->mem_res = mem;
	host->base = ioremap(mem->start, mem_size);
	if (!host->base)
		goto out;

	ret = -ENXIO;
	host->clk = clk_get(&pdev->dev, "mmc");
	if (IS_ERR(host->clk)) {
		ret = PTR_ERR(host->clk);
		goto out;
	}
	clk_enable(host->clk);
	host->mmc_input_clk = clk_get_rate(host->clk);

	init_mmcsd_host(host);

	host->use_dma = use_dma;
	host->irq = irq;

	if (host->use_dma && davinci_acquire_dma_channels(host) != 0)
		host->use_dma = 0;

	/* REVISIT:  someday, support IRQ-driven card detection.  */
	mmc->caps |= MMC_CAP_NEEDS_POLL;

	if (!pdata || pdata->wires == 4 || pdata->wires == 0)
		mmc->caps |= MMC_CAP_4_BIT_DATA;

	mmc->ops = &mmc_davinci_ops;
	mmc->f_min = 312500;
	mmc->f_max = 25000000;
	if (cpu_is_davinci_dm355()) {
		mmc->f_max = 50000000;
		mmc->caps |= MMC_CAP_MMC_HIGHSPEED | MMC_CAP_SD_HIGHSPEED;
	}
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;

	/* with no iommu coalescing pages, each phys_seg is a hw_seg */
	mmc->max_hw_segs	= NR_SG;
	mmc->max_phys_segs	= mmc->max_hw_segs;

	/* EDMA limit per hw segment (one or two MBytes) */
	mmc->max_seg_size	= MAX_CCNT * rw_threshold;

	/* MMC/SD controller limits for multiblock requests */
	mmc->max_blk_size	= 4095;  /* BLEN is 11 bits */
	mmc->max_blk_count	= 65535; /* NBLK is 16 bits */
	mmc->max_req_size	= mmc->max_blk_size * mmc->max_blk_count;

	dev_dbg(mmc_dev(host->mmc), "max_phys_segs=%d\n", mmc->max_phys_segs);
	dev_dbg(mmc_dev(host->mmc), "max_hw_segs=%d\n", mmc->max_hw_segs);
	dev_dbg(mmc_dev(host->mmc), "max_blk_size=%d\n", mmc->max_blk_size);
	dev_dbg(mmc_dev(host->mmc), "max_req_size=%d\n", mmc->max_req_size);
	dev_dbg(mmc_dev(host->mmc), "max_seg_size=%d\n", mmc->max_seg_size);

	platform_set_drvdata(pdev, host);

	ret = mmc_add_host(mmc);
	if (ret < 0)
		goto out;

	ret = request_irq(irq, mmc_davinci_irq, 0, mmc_hostname(mmc), host);
	if (ret)
		goto out;

	rename_region(mem, mmc_hostname(mmc));

	dev_info(mmc_dev(host->mmc), "Using %s, %d-bit mode\n",
		host->use_dma ? "DMA" : "PIO",
		(mmc->caps & MMC_CAP_4_BIT_DATA) ? 4 : 1);

	return 0;

out:
	if (host) {
		davinci_release_dma_channels(host);

		if (host->clk) {
			clk_disable(host->clk);
			clk_put(host->clk);
		}

		if (host->base)
			iounmap(host->base);
	}

	if (mmc)
		mmc_free_host(mmc);

	if (mem)
		release_resource(mem);

	dev_dbg(&pdev->dev, "probe err %d\n", ret);

	return ret;
}

static int __exit davinci_mmcsd_remove(struct platform_device *pdev)
{
	struct mmc_davinci_host *host = platform_get_drvdata(pdev);

	platform_set_drvdata(pdev, NULL);
	if (host) {
		mmc_remove_host(host->mmc);
		free_irq(host->irq, host);

		davinci_release_dma_channels(host);

		clk_disable(host->clk);
		clk_put(host->clk);

		iounmap(host->base);

		release_resource(host->mem_res);

		mmc_free_host(host->mmc);
	}

	return 0;
}

#ifdef CONFIG_PM
static int davinci_mmcsd_suspend(struct platform_device *pdev, pm_message_t msg)
{
	struct mmc_davinci_host *host = platform_get_drvdata(pdev);

	return mmc_suspend_host(host->mmc, msg);
}

static int davinci_mmcsd_resume(struct platform_device *pdev)
{
	struct mmc_davinci_host *host = platform_get_drvdata(pdev);

	return mmc_resume_host(host->mmc);
}
#else
#define davinci_mmcsd_suspend	NULL
#define davinci_mmcsd_resume	NULL
#endif

static struct platform_driver davinci_mmcsd_driver = {
	.driver		= {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
	},
	.remove		= __exit_p(davinci_mmcsd_remove),
	.suspend	= davinci_mmcsd_suspend,
	.resume		= davinci_mmcsd_resume,
};

static int __init davinci_mmcsd_init(void)
{
	return platform_driver_probe(&davinci_mmcsd_driver,
				     davinci_mmcsd_probe);
}
module_init(davinci_mmcsd_init);

static void __exit davinci_mmcsd_exit(void)
{
	platform_driver_unregister(&davinci_mmcsd_driver);
}
module_exit(davinci_mmcsd_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MMC/SD driver for Davinci MMC controller");
