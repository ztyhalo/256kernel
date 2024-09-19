/*
 * Copyright 2004-2007, 2015 Freescale Semiconductor, Inc. All Rights Reserved.
 * Copyright (C) 2008 Juergen Beisert
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation
 * 51 Franklin Street, Fifth Floor
 * Boston, MA  02110-1301, USA.
 */
#ifndef __SPI_SPI_IMX_H__
#define __SPI_SPI_IMX_H__

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi_bitbang.h>
#include <linux/types.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>

#include <linux/platform_data/dma-imx.h>
#include <linux/platform_data/spi-imx.h>

#define DRIVER_NAME "spi_imx"

#define MXC_CSPIRXDATA		0x00
#define MXC_CSPITXDATA		0x04
#define MXC_CSPICTRL		0x08
#define MXC_CSPIINT		0x0c
#define MXC_RESET		0x1c

/* generic defines to abstract from the different register layouts */
#define MXC_INT_RR	(1 << 0) /* Receive data ready interrupt */
#define MXC_INT_TE	(1 << 1) /* Transmit FIFO empty interrupt */

/* The maximum  bytes that a sdma BD can transfer.*/
#define MAX_SDMA_BD_BYTES  (1 << 15)
/* 3 Sec for 1MB or less than 1MB, else change with the transfer length */
#define IMX_DEFAULT_DMA_TIMEOUT (msecs_to_jiffies(3000))
#define IMX_DMA_TIMEOUT(len) ((len < 0x100000) ? IMX_DEFAULT_DMA_TIMEOUT : \
				len * IMX_DEFAULT_DMA_TIMEOUT / 0x100000)
struct spi_imx_config {
	unsigned int speed_hz;
	unsigned int bpw;
	unsigned int mode;
	u8 cs;
};

enum spi_imx_devtype {
	IMX1_CSPI,
	IMX21_CSPI,
	IMX27_CSPI,
	IMX31_CSPI,
	IMX35_CSPI,	/* CSPI on all i.mx except above */
	IMX51_ECSPI,	/* ECSPI on i.mx51 and later */
};

struct spi_imx_data;

struct spi_imx_devtype_data {
	void (*intctrl)(struct spi_imx_data *, int);
	int (*config)(struct spi_imx_data *, struct spi_imx_config *);
	void (*trigger)(struct spi_imx_data *);
	int (*rx_available)(struct spi_imx_data *);
	void (*reset)(struct spi_imx_data *);
	enum spi_imx_devtype devtype;
};

struct spi_imx_data {
	struct spi_bitbang bitbang;

	struct completion xfer_done;
	void __iomem *base;
	int irq;
	struct clk *clk_per;
	struct clk *clk_ipg;
	unsigned long spi_clk;

	unsigned int count;
	void (*tx)(struct spi_imx_data *);
	void (*rx)(struct spi_imx_data *);
	void *rx_buf;
	const void *tx_buf;
	unsigned int txfifo; /* number of words pushed in tx FIFO */

	/* DMA */
	unsigned int dma_is_inited;
	unsigned int dma_finished;
	bool usedma;
	u32 rx_wml;
	u32 tx_wml;
	u32 rxt_wml;
	struct completion dma_rx_completion;
	struct completion dma_tx_completion;

	const struct spi_imx_devtype_data *devtype_data;
	int chipselect[0];
};

#endif /*__SPI_SPI_IMX_H__*/