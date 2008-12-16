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
#include <mach/edma.h>
#include <mach/hardware.h>
#include <mach/irqs.h>

#include "davinci_mmc.h"

/* MMCSD Init clock in Hz in opendain mode */
#define MMCSD_INIT_CLOCK		200000
#define DRIVER_NAME			"davinci_mmc"

/* This macro could not be defined to 0 (ZERO) or -ve value.
 * This value is multiplied to "HZ"
 * while requesting for timer interrupt every time for probing card.
 */
#define MULTIPILER_TO_HZ 1

static struct mmcsd_config_def mmcsd_cfg = {
/* read write thresholds (in bytes) can be 16/32 */
	.rw_threshold	= 32,
/* To use the DMA or not-- 1- Use DMA, 0-Interrupt mode */
	.use_dma	= 1,
};

#define RSP_TYPE(x)	((x) & ~(MMC_RSP_BUSY|MMC_RSP_OPCODE))

static void davinci_mmc_read_fifo(struct mmc_davinci_host *host,
		u16 len, u8 *dest)
{
	void __iomem *fifo = host->base + DAVINCI_MMCDRR;
	u16 index = 0;

	dev_dbg(mmc_dev(host->mmc), "RX fifo %p count %d buf %p\n",
			fifo, len, dest);

	if (likely((0x03 & (unsigned long) dest) == 0)) {
		if (len >= 4) {
			ioread32_rep(fifo, dest, len >> 2);
			index = len & ~0x03;
		}
		if (len & 0x02) {
			*(u16 *)&dest[index] = ioread16(fifo);
			index += 2;
		}
		if (len & 0x01) {
			dest[index] = ioread8(fifo);
			index += 1;
		}
	} else if ((0x01 & (unsigned long) dest) == 0) {
		if (len >= 2) {
			ioread16_rep(fifo, dest, len >> 1);
			index = len & ~0x01;
		}
		if (len & 0x01)
			dest[index] = ioread8(fifo);
	} else {
		ioread8_rep(fifo, dest, len);
	}
}

static void davinci_mmc_write_fifo(struct mmc_davinci_host *host,
		u16 len, const u8 *src)
{
	void __iomem *fifo = host->base + DAVINCI_MMCDXR;
	u16 index = 0;

	dev_dbg(mmc_dev(host->mmc), "TX fifo %p count %d buf %p\n",
			fifo, len, src);

	if (likely((0x03 & (unsigned long) src) == 0)) {
		if (len >= 4) {
			iowrite32_rep(fifo, src + index, len >> 2);
			index = len & ~0x03;
		}
		if (len & 0x02) {
			iowrite16(*(u16 *)&src[index], fifo);
			index += 2;
		}
		if (len & 0x01) {
			iowrite8(src[index], fifo);
			index += 1;
		}
	} else if ((0x01 & (unsigned long) src) == 0) {
		if (len >= 2) {
			iowrite16_rep(fifo, src + index, len >> 1);
			index = len & ~0x01;
		}
		if (len & 0x01)
			iowrite8(src[index], fifo);
	} else {
		iowrite8_rep(fifo, src, len);
	}
}

/* PIO only */
static void mmc_davinci_sg_to_buf(struct mmc_davinci_host *host)
{
	struct scatterlist *sg;

	sg = host->data->sg + host->sg_idx;
	host->buffer_bytes_left = sg->length;
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

	if (host->data_dir == DAVINCI_MMC_DATADIR_WRITE)
		davinci_mmc_write_fifo(host, n, p);
	else
		davinci_mmc_read_fifo(host, n, p);

	host->buffer = p;
}

static void mmc_davinci_start_command(struct mmc_davinci_host *host,
		struct mmc_command *cmd)
{
	u32 cmd_reg = 0;
	u32 resp_type = 0;
	u32 cmd_type = 0;
	u32 im_val;

	dev_dbg(mmc_dev(host->mmc), "CMD%d, arg 0x%08x%s\n",
		cmd->opcode, cmd->arg,
		({ char *s;
		switch (RSP_TYPE(mmc_resp_type(cmd))) {
		case RSP_TYPE(MMC_RSP_R1):
			s = ", R1/R1b response";
			break;
		case RSP_TYPE(MMC_RSP_R2):
			s = ", R2 response";
			break;
		case RSP_TYPE(MMC_RSP_R3):
			s = ", R3 response";
			break;
		default:
			s = "";
			break;
		}; s; }));
	host->cmd = cmd;

	/* Protocol layer does not provide response type,
	 * but our hardware needs to know exact type, not just size!
	 */
	switch (RSP_TYPE(mmc_resp_type(cmd))) {
	case MMC_RSP_NONE:
		/* resp 0 */
		break;
	case RSP_TYPE(MMC_RSP_R1):
		resp_type = 1;
		break;
	case RSP_TYPE(MMC_RSP_R2):
		resp_type = 2;
		break;
	case RSP_TYPE(MMC_RSP_R3):
		resp_type = 3;
		break;
	default:
		break;
	}

	/* Protocol layer does not provide command type, but our hardware
	 * needs it!
	 * any data transfer means adtc type (but that information is not
	 * in command structure, so we flagged it into host struct.)
	 * However, telling bc, bcr and ac apart based on response is
	 * not foolproof:
	 * CMD0  = bc  = resp0  CMD15 = ac  = resp0
	 * CMD2  = bcr = resp2  CMD10 = ac  = resp2
	 *
	 * Resolve to best guess with some exception testing:
	 * resp0 -> bc, except CMD15 = ac
	 * rest are ac, except if opendrain
	 */

	if (mmc_cmd_type(cmd) == MMC_CMD_ADTC)
		cmd_type = DAVINCI_MMC_CMDTYPE_ADTC;
	else if (mmc_cmd_type(cmd) == MMC_CMD_BC)
		cmd_type = DAVINCI_MMC_CMDTYPE_BC;
	else if (mmc_cmd_type(cmd) == MMC_CMD_BCR)
		cmd_type = DAVINCI_MMC_CMDTYPE_BCR;
	else
		cmd_type = DAVINCI_MMC_CMDTYPE_AC;

	/* Set command Busy or not */
	if (cmd->flags & MMC_RSP_BUSY) {
		/*
		 * Linux core sending BUSY which is not defined for cmd 24
		 * as per mmc standard
		 */
		if (cmd->opcode != 24)
			cmd_reg = cmd_reg | (1 << 8);
	}

	/* Set command index */
	cmd_reg |= cmd->opcode;

	/* Setting initialize clock */
	if (cmd->opcode == 0)
		cmd_reg = cmd_reg | (1 << 14);

	/* Set for generating DMA Xfer event */
	if ((host->do_dma == 1) && (host->data != NULL)
	    && ((cmd->opcode == 18) || (cmd->opcode == 25)
		|| (cmd->opcode == 24) || (cmd->opcode == 17)))
		cmd_reg = cmd_reg | (1 << 16);

	/* Setting whether command involves data transfer or not */
	if (cmd_type == DAVINCI_MMC_CMDTYPE_ADTC)
		cmd_reg = cmd_reg | (1 << 13);

	/* Setting whether stream or block transfer */
	if (cmd->flags & MMC_DATA_STREAM)
		cmd_reg = cmd_reg | (1 << 12);

	/* Setting whether data read or write */
	if (host->data_dir == DAVINCI_MMC_DATADIR_WRITE)
		cmd_reg = cmd_reg | (1 << 11);

	/* Setting response type */
	cmd_reg = cmd_reg | (resp_type << 9);

	if (host->bus_mode == MMC_BUSMODE_PUSHPULL)
		cmd_reg = cmd_reg | (1 << 7);

	/* set Command timeout */
	writel(0xFFFF, host->base + DAVINCI_MMCTOR);

	/* Enable interrupt (calculate here, defer until FIFO is stuffed). */
	im_val =  MMCSD_EVENT_EOFCMD
		| MMCSD_EVENT_ERROR_CMDCRC
		| MMCSD_EVENT_ERROR_DATACRC
		| MMCSD_EVENT_ERROR_CMDTIMEOUT
		| MMCSD_EVENT_ERROR_DATATIMEOUT;
	if (host->data_dir == DAVINCI_MMC_DATADIR_WRITE) {
		im_val |= MMCSD_EVENT_BLOCK_XFERRED;

		if (!host->do_dma)
			im_val |= MMCSD_EVENT_WRITE;
	} else if (host->data_dir == DAVINCI_MMC_DATADIR_READ) {
		im_val |= MMCSD_EVENT_BLOCK_XFERRED;

		if (!host->do_dma)
			im_val |= MMCSD_EVENT_READ;
	}

	/*
	 * It is required by controoler b4 WRITE command that
	 * FIFO should be populated with 32 bytes
	 */
	if ((host->data_dir == DAVINCI_MMC_DATADIR_WRITE)
			&& (cmd_type == DAVINCI_MMC_CMDTYPE_ADTC)
			&& (host->do_dma != 1))
		/* Fill the FIFO for Tx */
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
		struct mmc_davinci_host *host = (struct mmc_davinci_host *)data;
		dev_warn(mmc_dev(host->mmc), "[DMA FAILED]");
		davinci_abort_dma(host);
	}
}

static int mmc_davinci_send_dma_request(struct mmc_davinci_host *host,
					struct mmc_request *req)
{
	int sync_dev;
	unsigned char i, j;
	unsigned short acnt, bcnt, ccnt;
	unsigned int src_port, dst_port, temp_ccnt;
	enum address_mode mode_src, mode_dst;
	enum fifo_width fifo_width_src, fifo_width_dst;
	unsigned short src_bidx, dst_bidx;
	unsigned short src_cidx, dst_cidx;
	unsigned short bcntrld;
	enum sync_dimension sync_mode;
	edmacc_paramentry_regs temp;
	int edma_chan_num;
	struct mmc_data *data = host->data;
	struct scatterlist *sg = &data->sg[0];
	unsigned int count;
	int num_frames, frame;

#define MAX_C_CNT		64000

	frame = data->blksz;
	count = sg_dma_len(sg);

	if ((data->blocks == 1) && (count > data->blksz))
		count = frame;

	if ((count & (mmcsd_cfg.rw_threshold-1)) == 0) {
		/* This should always be true due to an earlier check */
		acnt = 4;
		bcnt = mmcsd_cfg.rw_threshold>>2;
		num_frames = count >> ((mmcsd_cfg.rw_threshold == 32) ? 5 : 4);
	} else if (count < mmcsd_cfg.rw_threshold) {
		if ((count&3) == 0) {
			acnt = 4;
			bcnt = count>>2;
		} else if ((count&1) == 0) {
			acnt = 2;
			bcnt = count>>1;
		} else {
			acnt = 1;
			bcnt = count;
		}
		num_frames = 1;
	} else {
		acnt = 4;
		bcnt = mmcsd_cfg.rw_threshold>>2;
		num_frames = count >> ((mmcsd_cfg.rw_threshold == 32) ? 5 : 4);
		dev_warn(mmc_dev(host->mmc),
			"MMC: count of 0x%x unsupported, truncating transfer\n",
			count);
	}

	if (num_frames > MAX_C_CNT) {
		temp_ccnt = MAX_C_CNT;
		ccnt = temp_ccnt;
	} else {
		ccnt = num_frames;
		temp_ccnt = ccnt;
	}

	if (host->data_dir == DAVINCI_MMC_DATADIR_WRITE) {
		/*AB Sync Transfer */
		sync_dev = host->txdma;

		src_port = (unsigned int)sg_dma_address(sg);
		mode_src = INCR;
		fifo_width_src = W8BIT;	/* It's not cared as modeDsr is INCR */
		src_bidx = acnt;
		src_cidx = acnt * bcnt;
		dst_port = (unsigned int)(host->mem_res->start +
				DAVINCI_MMCDXR);
		/* cannot be FIFO, address not aligned on 32 byte boundary */
		mode_dst = INCR;
		fifo_width_dst = W8BIT;	/* It's not cared as modeDsr is INCR */
		dst_bidx = 0;
		dst_cidx = 0;
		bcntrld = 8;
		sync_mode = ABSYNC;

	} else {
		sync_dev = host->rxdma;

		src_port = (unsigned int)(host->mem_res->start +
				DAVINCI_MMCDRR);
		/* cannot be FIFO, address not aligned on 32 byte boundary */
		mode_src = INCR;
		fifo_width_src = W8BIT;
		src_bidx = 0;
		src_cidx = 0;
		dst_port = (unsigned int)sg_dma_address(sg);
		mode_dst = INCR;
		fifo_width_dst = W8BIT;	/* It's not cared as modeDsr is INCR */
		dst_bidx = acnt;
		dst_cidx = acnt * bcnt;
		bcntrld = 8;
		sync_mode = ABSYNC;
	}

	davinci_set_dma_src_params(sync_dev, src_port, mode_src,
			fifo_width_src);
	davinci_set_dma_dest_params(sync_dev, dst_port, mode_dst,
			fifo_width_dst);
	davinci_set_dma_src_index(sync_dev, src_bidx, src_cidx);
	davinci_set_dma_dest_index(sync_dev, dst_bidx, dst_cidx);
	davinci_set_dma_transfer_params(sync_dev, acnt, bcnt, ccnt, bcntrld,
					sync_mode);

	davinci_get_dma_params(sync_dev, &temp);
	if (sync_dev == host->txdma) {
		if (host->option_write == 0) {
			host->option_write = temp.opt;
		} else {
			temp.opt = host->option_write;
			davinci_set_dma_params(sync_dev, &temp);
		}
	}
	if (sync_dev == host->rxdma) {
		if (host->option_read == 0) {
			host->option_read = temp.opt;
		} else {
			temp.opt = host->option_read;
			davinci_set_dma_params(sync_dev, &temp);
		}
	}

	if (host->sg_len > 1) {
		davinci_get_dma_params(sync_dev, &temp);
		temp.opt &= ~TCINTEN;
		davinci_set_dma_params(sync_dev, &temp);

		for (i = 0; i < host->sg_len - 1; i++) {
			sg = &data->sg[i + 1];

			if (i != 0) {
				j = i - 1;
				davinci_get_dma_params(
					host->edma_ch_details.chanel_num[j],
					&temp);
				temp.opt &= ~TCINTEN;
				davinci_set_dma_params(
					host->edma_ch_details.chanel_num[j],
					&temp);
			}

			edma_chan_num = host->edma_ch_details.chanel_num[0];

			frame = data->blksz;
			count = sg_dma_len(sg);

			if ((data->blocks == 1) && (count > data->blksz))
				count = frame;

			ccnt = count >> ((mmcsd_cfg.rw_threshold == 32) ? 5 : 4);

			if (sync_dev == host->txdma)
				temp.src = (unsigned int)sg_dma_address(sg);
			else
				temp.dst = (unsigned int)sg_dma_address(sg);
			temp.opt |= TCINTEN;

			temp.ccnt = (temp.ccnt & 0xFFFF0000) | (ccnt);

			davinci_set_dma_params(edma_chan_num, &temp);
			if (i != 0) {
				j = i - 1;
				davinci_dma_link_lch(host->edma_ch_details.
						chanel_num[j],
						edma_chan_num);
			}
		}
		davinci_dma_link_lch(sync_dev,
				host->edma_ch_details.chanel_num[0]);
	}

	davinci_start_dma(sync_dev);
	return 0;
}

static int mmc_davinci_start_dma_transfer(struct mmc_davinci_host *host,
		struct mmc_request *req)
{
	int use_dma = 1, i;
	struct mmc_data *data = host->data;
	int mask = mmcsd_cfg.rw_threshold-1;

	host->sg_len = dma_map_sg(mmc_dev(host->mmc), data->sg, host->sg_len,
				((data->flags & MMC_DATA_WRITE)
				? DMA_TO_DEVICE
				: DMA_FROM_DEVICE));

	/* Decide if we can use DMA */
	for (i = 0; i < host->sg_len; i++) {
		if (data->sg[i].length & mask) {
			use_dma = 0;
			break;
		}
	}

	if (!use_dma) {
		dma_unmap_sg(mmc_dev(host->mmc), data->sg, host->sg_len,
				  (data->flags & MMC_DATA_WRITE)
				? DMA_TO_DEVICE
				: DMA_FROM_DEVICE);
		return -1;
	}

	host->do_dma = 1;

	mmc_davinci_send_dma_request(host, req);

	return 0;
}

static int davinci_release_dma_channels(struct mmc_davinci_host *host)
{
	davinci_free_dma(host->txdma);
	davinci_free_dma(host->rxdma);

	if (host->edma_ch_details.cnt_chanel) {
		davinci_free_dma(host->edma_ch_details.chanel_num[0]);
		host->edma_ch_details.cnt_chanel = 0;
	}

	return 0;
}

static int davinci_acquire_dma_channels(struct mmc_davinci_host *host)
{
	int edma_chan_num, tcc = 0, r, sync_dev;
	enum dma_event_q queue_no = EVENTQ_0;

	/* Acquire master DMA write channel */
	r = davinci_request_dma(host->txdma, "MMC_WRITE",
		mmc_davinci_dma_cb, host, &edma_chan_num, &tcc, queue_no);
	if (r != 0) {
		dev_warn(mmc_dev(host->mmc),
				"MMC: davinci_request_dma() failed with %d\n",
				r);
		return r;
	}

	/* Acquire master DMA read channel */
	r = davinci_request_dma(host->rxdma, "MMC_READ",
		mmc_davinci_dma_cb, host, &edma_chan_num, &tcc, queue_no);
	if (r != 0) {
		dev_warn(mmc_dev(host->mmc),
				"MMC: davinci_request_dma() failed with %d\n",
				r);
		goto free_master_write;
	}

	host->edma_ch_details.cnt_chanel = 0;

	/* currently data Writes are done using single block mode,
	 * so no DMA slave write channel is required for now */

	/* Create a DMA slave read channel
	 * (assuming max segments handled is 2) */
	sync_dev = host->rxdma;
	r = davinci_request_dma(DAVINCI_EDMA_PARAM_ANY, "LINK", NULL, NULL,
		&edma_chan_num, &sync_dev, queue_no);
	if (r != 0) {
		dev_warn(mmc_dev(host->mmc),
			"MMC: davinci_request_dma() failed with %d\n", r);
		goto free_master_read;
	}

	host->edma_ch_details.cnt_chanel++;
	host->edma_ch_details.chanel_num[0] = edma_chan_num;

	return 0;

free_master_read:
	davinci_free_dma(host->rxdma);
free_master_write:
	davinci_free_dma(host->txdma);

	return r;
}

static void
mmc_davinci_prepare_data(struct mmc_davinci_host *host, struct mmc_request *req)
{
	int fifo_lev = (mmcsd_cfg.rw_threshold == 32) ? MMCFIFOCTL_FIFOLEV : 0;
	int timeout, sg_len;

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
		"DTO %d cycles + %d ns, %d blocks of %d bytes\r\n",
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

	sg_len = (req->data->blocks == 1) ? 1 : req->data->sg_len;
	host->sg_len = sg_len;

	host->bytes_left = req->data->blocks * req->data->blksz;

	if ((host->use_dma == 1) &&
		  ((host->bytes_left & (mmcsd_cfg.rw_threshold-1)) == 0) &&
	      (mmc_davinci_start_dma_transfer(host, req) == 0)) {
		host->buffer = NULL;
		host->bytes_left = 0;
	} else {
		/* Revert to CPU Copy */

		host->do_dma = 0;
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
		"clock %dHz busmode %d powermode %d Vdd %04x\r\n",
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
		while (!(readl(host->base + DAVINCI_MMCST0) &
				MMCSD_EVENT_EOFCMD))
			cpu_relax();
	}

	/* FIXME on power OFF, reset things ... */
}

static void
mmc_davinci_xfer_done(struct mmc_davinci_host *host, struct mmc_data *data)
{
	host->data = NULL;
	host->data_dir = DAVINCI_MMC_DATADIR_NONE;
	if (data->error == 0)
		data->bytes_xfered += data->blocks * data->blksz;

	if (host->do_dma) {
		davinci_abort_dma(host);

		dma_unmap_sg(mmc_dev(host->mmc), data->sg, host->sg_len,
			     (data->flags & MMC_DATA_WRITE)
			     ? DMA_TO_DEVICE
			     : DMA_FROM_DEVICE);
	}

	if (data->error == -ETIMEDOUT) {
		mmc_request_done(host->mmc, data->mrq);
		return;
	}

	if (!data->stop) {
		mmc_request_done(host->mmc, data->mrq);
		return;
	}
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
	}
}

static inline int handle_core_command(
		struct mmc_davinci_host *host, unsigned int status)
{
	int end_command = 0;
	int end_transfer = 0;
	unsigned int qstatus;

	qstatus = status;
	while (1) {
		if ((status & MMCSD_EVENT_WRITE) &&
				(host->data_dir == DAVINCI_MMC_DATADIR_WRITE)
				&& (host->bytes_left > 0)) {
			/* Buffer almost empty */
			davinci_fifo_data_trans(host, mmcsd_cfg.rw_threshold);
		}

		if ((status & MMCSD_EVENT_READ) &&
				(host->data_dir == DAVINCI_MMC_DATADIR_READ)
				&& (host->bytes_left > 0)) {
			/* Buffer almost empty */
			davinci_fifo_data_trans(host, mmcsd_cfg.rw_threshold);
		}
		status = readl(host->base + DAVINCI_MMCST0);
		if (!status)
			break;
		qstatus |= status;
		if (host->data == NULL) {
			dev_dbg(mmc_dev(host->mmc), "Status is %x at end of "
				"ISR when host->data is NULL", status);
			break;
		}
	}

	if (qstatus & MMCSD_EVENT_BLOCK_XFERRED) {
		/* Block sent/received */
		if (host->data != NULL) {
			if ((host->do_dma == 0) && (host->bytes_left > 0)) {
				/* if datasize<mmcsd_cfg.rw_threshold
				 * no RX ints are generated
				 */
				davinci_fifo_data_trans(host,
						mmcsd_cfg.rw_threshold);
			}
			end_transfer = 1;
		} else {
			dev_warn(mmc_dev(host->mmc), "TC:host->data is NULL\n");
		}
	}

	if (qstatus & MMCSD_EVENT_ERROR_DATATIMEOUT) {
		/* Data timeout */
		if (host->data) {
			host->data->error = -ETIMEDOUT;
			dev_dbg(mmc_dev(host->mmc), "MMCSD: Data timeout, "
				"CMD%d and status is %x\n",
				host->cmd->opcode, status);

			if (host->cmd)
				host->cmd->error = -ETIMEDOUT;
			end_transfer = 1;
		}
	}

	if (qstatus & MMCSD_EVENT_ERROR_DATACRC) {
		u32 temp;
		/* DAT line portion is disabled and in reset state */
		temp = readl(host->base + DAVINCI_MMCCTL);

		writel(temp | MMCCTL_CMDRST,
			host->base + DAVINCI_MMCCTL);

		udelay(10);

		writel(temp & ~MMCCTL_CMDRST,
			host->base + DAVINCI_MMCCTL);

		/* Data CRC error */
		if (host->data) {
			host->data->error = -EILSEQ;
			dev_dbg(mmc_dev(host->mmc), "MMCSD: Data CRC error, "
				"bytes left %d\n", host->bytes_left);
			end_transfer = 1;
		} else {
			dev_dbg(mmc_dev(host->mmc), "MMCSD: Data CRC error\n");
		}
	}

	if (qstatus & MMCSD_EVENT_ERROR_CMDTIMEOUT) {
		if (host->do_dma)
			davinci_abort_dma(host);

		/* Command timeout */
		if (host->cmd) {
			dev_dbg(mmc_dev(host->mmc), "MMCSD: CMD%d "
				"timeout, status %x\n",
				host->cmd->opcode, status);
			host->cmd->error = -ETIMEDOUT;
			end_command = 1;
		}
	}

	if (qstatus & MMCSD_EVENT_ERROR_CMDCRC) {
		/* Command CRC error */
		dev_dbg(mmc_dev(host->mmc), "Command CRC error\n");
		if (host->cmd) {
			/* Ignore CMD CRC errors during high speed operation */
			if (host->mmc->ios.clock <= 25000000)
				host->cmd->error = -EILSEQ;
			end_command = 1;
		}
	}

	if (qstatus & MMCSD_EVENT_EOFCMD) {
		/* End of command phase */
		end_command = 1;
	}

	if (end_command)
		mmc_davinci_cmd_done(host, host->cmd);
	if (end_transfer)
		mmc_davinci_xfer_done(host, host->data);
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
	.request = mmc_davinci_request,
	.set_ios = mmc_davinci_set_ios,
	.get_cd = mmc_davinci_get_cd,
	.get_ro = mmc_davinci_get_ro,
};

static void init_mmcsd_host(struct mmc_davinci_host *host)
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

static int davinci_mmcsd_probe(struct platform_device *pdev)
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

	/* REVISIT:  someday, support IRQ-driven card detection.  */
	mmc->caps |= MMC_CAP_NEEDS_POLL;

	if (!pdata || pdata->wires == 4 || pdata->wires == 0)
		mmc->caps |= MMC_CAP_4_BIT_DATA;

	mmc->ops = &mmc_davinci_ops;
	mmc->f_min = 312500;
#ifdef CONFIG_MMC_HIGHSPEED /* FIXME: no longer used */
	mmc->f_max = 50000000;
	mmc->caps |= MMC_CAP_MMC_HIGHSPEED;
#else
	mmc->f_max = 25000000;
#endif
	mmc->ocr_avail = MMC_VDD_32_33;

#ifdef CONFIG_MMC_BLOCK_BOUNCE
	mmc->max_phys_segs = 1;
	mmc->max_hw_segs   = 1;
#else
	mmc->max_phys_segs = 2;
	mmc->max_hw_segs   = 2;
#endif
	mmc->max_blk_size  = 4095;  /* BLEN is 11 bits */
	mmc->max_blk_count = 65535; /* NBLK is 16 bits */
	mmc->max_req_size  = mmc->max_blk_size * mmc->max_blk_count;
	mmc->max_seg_size  = mmc->max_req_size;

	dev_dbg(mmc_dev(host->mmc), "max_phys_segs=%d\n", mmc->max_phys_segs);
	dev_dbg(mmc_dev(host->mmc), "max_hw_segs=%d\n", mmc->max_hw_segs);
	dev_dbg(mmc_dev(host->mmc), "max_blk_size=%d\n", mmc->max_blk_size);
	dev_dbg(mmc_dev(host->mmc), "max_req_size=%d\n", mmc->max_req_size);
	dev_dbg(mmc_dev(host->mmc), "max_seg_size=%d\n", mmc->max_seg_size);

	if (mmcsd_cfg.use_dma)
		if (davinci_acquire_dma_channels(host) != 0)
			goto out;

	host->use_dma = mmcsd_cfg.use_dma;
	host->irq = irq;

	platform_set_drvdata(pdev, host);

	ret = mmc_add_host(mmc);
	if (ret < 0)
		goto out;

	ret = request_irq(irq, mmc_davinci_irq, 0, mmc_hostname(mmc), host);
	if (ret)
		goto out;

	dev_info(mmc_dev(host->mmc), "Using %s, %d-bit mode\n",
		mmcsd_cfg.use_dma ? "DMA" : "PIO",
		(mmc->caps & MMC_CAP_4_BIT_DATA) ? 4 : 1);

	return 0;

out:
	if (host) {
		if (host->edma_ch_details.cnt_chanel)
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

static int davinci_mmcsd_remove(struct platform_device *pdev)
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
	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
	},
	.probe = davinci_mmcsd_probe,
	.remove = davinci_mmcsd_remove,
	.suspend = davinci_mmcsd_suspend,
	.resume = davinci_mmcsd_resume,
};

static int davinci_mmcsd_init(void)
{
	return platform_driver_register(&davinci_mmcsd_driver);
}

static void __exit davinci_mmcsd_exit(void)
{
	platform_driver_unregister(&davinci_mmcsd_driver);
}

module_init(davinci_mmcsd_init);
module_exit(davinci_mmcsd_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MMCSD driver for Davinci MMC controller");
