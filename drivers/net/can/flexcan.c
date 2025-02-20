/*
 * flexcan.c - FLEXCAN CAN controller driver
 *
 * Copyright (c) 2005-2006 Varma Electronics Oy
 * Copyright (c) 2009 Sascha Hauer, Pengutronix
 * Copyright (c) 2010 Marc Kleine-Budde, Pengutronix
 *
 * Based on code originally by Andrey Volkov <avolkov@varma-el.com>
 *
 * LICENCE:
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/netdevice.h>
#include <linux/can.h>
#include <linux/can/dev.h>
#include <linux/can/error.h>
#include <linux/can/led.h>
#include <linux/can/platform/flexcan.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/if_arp.h>
#include <linux/if_ether.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mfd/syscon.h>
#include <linux/mfd/syscon/imx6q-iomuxc-gpr.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/regmap.h>

#define DRV_NAME			"flexcan"

#define IRQ_CAN_RX           1

/* 8 for RX fifo and 2 error handling */
#define FLEXCAN_NAPI_WEIGHT		(8 + 2)

/* FLEXCAN module configuration register (CANMCR) bits */
#define FLEXCAN_MCR_MDIS		BIT(31)
#define FLEXCAN_MCR_FRZ			BIT(30)
#define FLEXCAN_MCR_FEN			BIT(29)
#define FLEXCAN_MCR_HALT		BIT(28)
#define FLEXCAN_MCR_NOT_RDY		BIT(27)
#define FLEXCAN_MCR_WAK_MSK		BIT(26)
#define FLEXCAN_MCR_SOFTRST		BIT(25)
#define FLEXCAN_MCR_FRZ_ACK		BIT(24)
#define FLEXCAN_MCR_SUPV		BIT(23)
#define FLEXCAN_MCR_SLF_WAK		BIT(22)
#define FLEXCAN_MCR_WRN_EN		BIT(21)
#define FLEXCAN_MCR_LPM_ACK		BIT(20)
#define FLEXCAN_MCR_WAK_SRC		BIT(19)
#define FLEXCAN_MCR_DOZE		BIT(18)
#define FLEXCAN_MCR_SRX_DIS		BIT(17)
#define FLEXCAN_MCR_BCC			BIT(16)
#define FLEXCAN_MCR_LPRIO_EN		BIT(13)
#define FLEXCAN_MCR_AEN			BIT(12)
#define FLEXCAN_MCR_MAXMB(x)		((x) & 0x1f)
#define FLEXCAN_MCR_IDAM_A		(0 << 8)
#define FLEXCAN_MCR_IDAM_B		(1 << 8)
#define FLEXCAN_MCR_IDAM_C		(2 << 8)
#define FLEXCAN_MCR_IDAM_D		(3 << 8)

/* FLEXCAN control register (CANCTRL) bits */
#define FLEXCAN_CTRL_PRESDIV(x)		(((x) & 0xff) << 24)
#define FLEXCAN_CTRL_RJW(x)		(((x) & 0x03) << 22)
#define FLEXCAN_CTRL_PSEG1(x)		(((x) & 0x07) << 19)
#define FLEXCAN_CTRL_PSEG2(x)		(((x) & 0x07) << 16)
#define FLEXCAN_CTRL_BOFF_MSK		BIT(15)
#define FLEXCAN_CTRL_ERR_MSK		BIT(14)
#define FLEXCAN_CTRL_CLK_SRC		BIT(13)
#define FLEXCAN_CTRL_LPB		BIT(12)
#define FLEXCAN_CTRL_TWRN_MSK		BIT(11)
#define FLEXCAN_CTRL_RWRN_MSK		BIT(10)
#define FLEXCAN_CTRL_SMP		BIT(7)
#define FLEXCAN_CTRL_BOFF_REC		BIT(6)
#define FLEXCAN_CTRL_TSYN		BIT(5)
#define FLEXCAN_CTRL_LBUF		BIT(4)
#define FLEXCAN_CTRL_LOM		BIT(3)
#define FLEXCAN_CTRL_PROPSEG(x)		((x) & 0x07)
#define FLEXCAN_CTRL_ERR_BUS		(FLEXCAN_CTRL_ERR_MSK)
#define FLEXCAN_CTRL_ERR_STATE \
	(FLEXCAN_CTRL_TWRN_MSK | FLEXCAN_CTRL_RWRN_MSK | \
	 FLEXCAN_CTRL_BOFF_MSK)
#define FLEXCAN_CTRL_ERR_ALL \
	(FLEXCAN_CTRL_ERR_BUS | FLEXCAN_CTRL_ERR_STATE)

/* FLEXCAN error and status register (ESR) bits */
#define FLEXCAN_ESR_TWRN_INT		BIT(17)
#define FLEXCAN_ESR_RWRN_INT		BIT(16)
#define FLEXCAN_ESR_BIT1_ERR		BIT(15)
#define FLEXCAN_ESR_BIT0_ERR		BIT(14)
#define FLEXCAN_ESR_ACK_ERR		BIT(13)
#define FLEXCAN_ESR_CRC_ERR		BIT(12)
#define FLEXCAN_ESR_FRM_ERR		BIT(11)
#define FLEXCAN_ESR_STF_ERR		BIT(10)
#define FLEXCAN_ESR_TX_WRN		BIT(9)
#define FLEXCAN_ESR_RX_WRN		BIT(8)
#define FLEXCAN_ESR_IDLE		BIT(7)
#define FLEXCAN_ESR_TXRX		BIT(6)
#define FLEXCAN_EST_FLT_CONF_SHIFT	(4)
#define FLEXCAN_ESR_FLT_CONF_MASK	(0x3 << FLEXCAN_EST_FLT_CONF_SHIFT)
#define FLEXCAN_ESR_FLT_CONF_ACTIVE	(0x0 << FLEXCAN_EST_FLT_CONF_SHIFT)
#define FLEXCAN_ESR_FLT_CONF_PASSIVE	(0x1 << FLEXCAN_EST_FLT_CONF_SHIFT)
#define FLEXCAN_ESR_BOFF_INT		BIT(2)
#define FLEXCAN_ESR_ERR_INT		BIT(1)
#define FLEXCAN_ESR_WAK_INT		BIT(0)
#define FLEXCAN_ESR_ERR_BUS \
	(FLEXCAN_ESR_BIT1_ERR | FLEXCAN_ESR_BIT0_ERR | \
	 FLEXCAN_ESR_ACK_ERR | FLEXCAN_ESR_CRC_ERR | \
	 FLEXCAN_ESR_FRM_ERR | FLEXCAN_ESR_STF_ERR)
#define FLEXCAN_ESR_ERR_STATE \
	(FLEXCAN_ESR_TWRN_INT | FLEXCAN_ESR_RWRN_INT | FLEXCAN_ESR_BOFF_INT)
#define FLEXCAN_ESR_ERR_ALL \
	(FLEXCAN_ESR_ERR_BUS | FLEXCAN_ESR_ERR_STATE)
#define FLEXCAN_ESR_ALL_INT \
	(FLEXCAN_ESR_TWRN_INT | FLEXCAN_ESR_RWRN_INT | \
	 FLEXCAN_ESR_BOFF_INT | FLEXCAN_ESR_ERR_INT | \
	 FLEXCAN_ESR_WAK_INT)

/* FLEXCAN interrupt flag register (IFLAG) bits */
#define FLEXCAN_RESERVED_BUF_ID		8
#define FLEXCAN_TX_BUF_ID		13
#define FLEXCAN_IFLAG_BUF(x)		BIT(x)
#define FLEXCAN_IFLAG_RX_FIFO_OVERFLOW	BIT(7)
#define FLEXCAN_IFLAG_RX_FIFO_WARN	BIT(6)
#define FLEXCAN_IFLAG_RX_FIFO_AVAILABLE	BIT(5)
#define FLEXCAN_IFLAG_DEFAULT \
	(FLEXCAN_IFLAG_RX_FIFO_OVERFLOW | FLEXCAN_IFLAG_RX_FIFO_AVAILABLE | \
	 FLEXCAN_IFLAG_BUF(FLEXCAN_TX_BUF_ID))

/* FLEXCAN message buffers */
#define FLEXCAN_MB_CNT_CODE(x)		(((x) & 0xf) << 24)
#define FLEXCAN_MB_CNT_SRR		BIT(22)
#define FLEXCAN_MB_CNT_IDE		BIT(21)
#define FLEXCAN_MB_CNT_RTR		BIT(20)
#define FLEXCAN_MB_CNT_LENGTH(x)	(((x) & 0xf) << 16)
#define FLEXCAN_MB_CNT_TIMESTAMP(x)	((x) & 0xffff)

#define FLEXCAN_MB_CODE_MASK		(0xf0ffffff)

#define FLEXCAN_TIMEOUT_US             (50)

/*
 * FLEXCAN hardware feature flags
 *
 * Below is some version info we got:
 *    SOC   Version   IP-Version  Glitch-  [TR]WRN_INT
 *                                Filter?   connected?
 *   MX25  FlexCAN2  03.00.00.00     no         no
 *   MX28  FlexCAN2  03.00.04.00    yes        yes
 *   MX35  FlexCAN2  03.00.00.00     no         no
 *   MX53  FlexCAN2  03.00.00.00    yes         no
 *   MX6s  FlexCAN3  10.00.12.00    yes        yes
 *
 * Some SOCs do not have the RX_WARN & TX_WARN interrupt line connected.
 */
#define FLEXCAN_HAS_V10_FEATURES	BIT(1) /* For core version >= 10 */
#define FLEXCAN_HAS_BROKEN_ERR_STATE	BIT(2) /* [TR]WRN_INT not connected */
#define FLEXCAN_HAS_ERR005829		BIT(3) /* have errata ERR005829 */

/* Structure of the message buffer */
struct flexcan_mb {
	u32 can_ctrl;
	u32 can_id;
	u32 data[2];
};

/* Structure of the hardware registers */
struct flexcan_regs {
	u32 mcr;		/* 0x00 */
	u32 ctrl;		/* 0x04 */
	u32 timer;		/* 0x08 */
	u32 _reserved1;		/* 0x0c */
	u32 rxgmask;		/* 0x10 */
	u32 rx14mask;		/* 0x14 */
	u32 rx15mask;		/* 0x18 */
	u32 ecr;		/* 0x1c */
	u32 esr;		/* 0x20 */
	u32 imask2;		/* 0x24 */
	u32 imask1;		/* 0x28 */
	u32 iflag2;		/* 0x2c */
	u32 iflag1;		/* 0x30 */
	u32 crl2;		/* 0x34 */
	u32 esr2;		/* 0x38 */
	u32 imeur;		/* 0x3c */
	u32 lrfr;		/* 0x40 */
	u32 crcr;		/* 0x44 */
	u32 rxfgmask;		/* 0x48 */
	u32 rxfir;		/* 0x4c */
	u32 _reserved3[12];
	struct flexcan_mb cantxfg[64];
};

struct flexcan_devtype_data {
	u32 features;	/* hardware controller features */
};

struct flexcan_stop_mode {
	struct regmap *gpr;
	u8 req_gpr;
	u8 req_bit;
	u8 ack_gpr;
	u8 ack_bit;
};

struct can_rx_offload{
	struct net_device *dev;
	unsigned int (*mailbox_read)(struct can_rx_offload * offload, bool drop, 
				struct sk_buff **skb, u32 * timestamp, unsigned int mb);
	struct sk_buff_head skb_queue;
	u32 skb_queue_len_max;
	unsigned int mb_first;
	unsigned int mb_last;
	struct napi_struct napi;

	bool inc;
};

struct flexcan_priv {
	struct can_priv can;
	struct net_device *dev;
	struct napi_struct napi;
	struct can_rx_offload offload;

	void __iomem *base;
	u32 reg_esr;
	u32 reg_ctrl_default;

	struct clk *clk_ipg;
	struct clk *clk_per;
	struct flexcan_platform_data *pdata;
	const struct flexcan_devtype_data *devtype_data;
	struct regulator *reg_xceiver;
	struct flexcan_stop_mode stm;
	int id;
};

static struct flexcan_devtype_data fsl_p1010_devtype_data = {
	.features = FLEXCAN_HAS_BROKEN_ERR_STATE,
};
static struct flexcan_devtype_data fsl_imx28_devtype_data;
static struct flexcan_devtype_data fsl_imx6q_devtype_data = {
	.features = FLEXCAN_HAS_V10_FEATURES | FLEXCAN_HAS_ERR005829,
};

static const struct can_bittiming_const flexcan_bittiming_const = {
	.name = DRV_NAME,
	.tseg1_min = 4,
	.tseg1_max = 16,
	.tseg2_min = 2,
	.tseg2_max = 8,
	.sjw_max = 4,
	.brp_min = 1,
	.brp_max = 256,
	.brp_inc = 1,
};

/*
 * Abstract off the read/write for arm versus ppc. This
 * assumes that PPC uses big-endian registers and everything
 * else uses little-endian registers, independent of CPU
 * endianess.
 */
#if defined(CONFIG_PPC)
static inline u32 flexcan_read(void __iomem *addr)
{
	return in_be32(addr);
}

static inline void flexcan_write(u32 val, void __iomem *addr)
{
	out_be32(addr, val);
}
#else
static inline u32 flexcan_read(void __iomem *addr)
{
	return readl(addr);
}

static inline void flexcan_write(u32 val, void __iomem *addr)
{
	writel(val, addr);
}
#endif

static inline void flexcan_enter_stop_mode(struct flexcan_priv *priv)
{
	/* enable stop request */
	if (priv->devtype_data->features & FLEXCAN_HAS_V10_FEATURES)
		regmap_update_bits(priv->stm.gpr, priv->stm.req_gpr,
			1 << priv->stm.req_bit, 1 << priv->stm.req_bit);
}

static inline void flexcan_exit_stop_mode(struct flexcan_priv *priv)
{
	/* remove stop request */
	if (priv->devtype_data->features & FLEXCAN_HAS_V10_FEATURES)
		regmap_update_bits(priv->stm.gpr, priv->stm.req_gpr,
			1 << priv->stm.req_bit, 0);
}

static inline int flexcan_transceiver_enable(const struct flexcan_priv *priv)
{
	if (priv->pdata && priv->pdata->transceiver_switch) {
		priv->pdata->transceiver_switch(1);
		return 0;
	}

	if (!priv->reg_xceiver)
		return 0;

	return regulator_enable(priv->reg_xceiver);
}

static inline int flexcan_transceiver_disable(const struct flexcan_priv *priv)
{
	if (priv->pdata && priv->pdata->transceiver_switch) {
		priv->pdata->transceiver_switch(0);
		return 0;
	}

	if (!priv->reg_xceiver)
		return 0;

	return regulator_disable(priv->reg_xceiver);
}

static inline int flexcan_has_and_handle_berr(const struct flexcan_priv *priv,
					      u32 reg_esr)
{
	return (priv->can.ctrlmode & CAN_CTRLMODE_BERR_REPORTING) &&
		(reg_esr & FLEXCAN_ESR_ERR_BUS);
}

static int flexcan_chip_enable(struct flexcan_priv *priv)
{
	struct flexcan_regs __iomem *regs = priv->base;
	unsigned int timeout = FLEXCAN_TIMEOUT_US / 10;
	u32 reg;

	reg = flexcan_read(&regs->mcr);
	reg &= ~FLEXCAN_MCR_MDIS;
	flexcan_write(reg, &regs->mcr);

	while (timeout-- && (flexcan_read(&regs->mcr) & FLEXCAN_MCR_LPM_ACK))
		udelay(20);

	if (flexcan_read(&regs->mcr) & FLEXCAN_MCR_LPM_ACK)
		return -ETIMEDOUT;

	return 0;
}

static int flexcan_chip_disable(struct flexcan_priv *priv)
{
	struct flexcan_regs __iomem *regs = priv->base;
	unsigned int timeout = FLEXCAN_TIMEOUT_US / 10;
	u32 reg;

	reg = flexcan_read(&regs->mcr);
	reg |= FLEXCAN_MCR_MDIS;
	flexcan_write(reg, &regs->mcr);

	while (timeout-- && !(flexcan_read(&regs->mcr) & FLEXCAN_MCR_LPM_ACK))
		udelay(20);

	if (!(flexcan_read(&regs->mcr) & FLEXCAN_MCR_LPM_ACK))
		return -ETIMEDOUT;

	return 0;
}

static int flexcan_chip_freeze(struct flexcan_priv *priv)
{
	struct flexcan_regs __iomem *regs = priv->base;
	unsigned int timeout = 1000 * 1000 * 10 / priv->can.bittiming.bitrate;
	u32 reg;

	reg = flexcan_read(&regs->mcr);
	reg |= FLEXCAN_MCR_HALT;
	flexcan_write(reg, &regs->mcr);

	while (timeout-- && !(flexcan_read(&regs->mcr) & FLEXCAN_MCR_FRZ_ACK))
		usleep_range(100, 200);

	if (!(flexcan_read(&regs->mcr) & FLEXCAN_MCR_FRZ_ACK))
		return -ETIMEDOUT;

	return 0;
}

static int flexcan_chip_unfreeze(struct flexcan_priv *priv)
{
	struct flexcan_regs __iomem *regs = priv->base;
	unsigned int timeout = FLEXCAN_TIMEOUT_US / 10;
	u32 reg;

	reg = flexcan_read(&regs->mcr);
	reg &= ~FLEXCAN_MCR_HALT;
	flexcan_write(reg, &regs->mcr);

	while (timeout-- && (flexcan_read(&regs->mcr) & FLEXCAN_MCR_FRZ_ACK))
		udelay(20);

	if (flexcan_read(&regs->mcr) & FLEXCAN_MCR_FRZ_ACK)
		return -ETIMEDOUT;

	return 0;
}

static int flexcan_chip_softreset(struct flexcan_priv *priv)
{
	struct flexcan_regs __iomem *regs = priv->base;
	unsigned int timeout = FLEXCAN_TIMEOUT_US / 10;

	flexcan_write(FLEXCAN_MCR_SOFTRST, &regs->mcr);
	while (timeout-- && (flexcan_read(&regs->mcr) & FLEXCAN_MCR_SOFTRST))
		udelay(20);

	if (flexcan_read(&regs->mcr) & FLEXCAN_MCR_SOFTRST)
		return -ETIMEDOUT;

	return 0;
}

static int flexcan_get_berr_counter(const struct net_device *dev,
				    struct can_berr_counter *bec)
{
	const struct flexcan_priv *priv = netdev_priv(dev);
	struct flexcan_regs __iomem *regs = priv->base;
	u32 reg = flexcan_read(&regs->ecr);

	bec->txerr = (reg >> 0) & 0xff;
	bec->rxerr = (reg >> 8) & 0xff;

	return 0;
}


static int can_rx_offload_napi_poll(struct napi_struct *napi, int quota)
{
	struct can_rx_offload *offload = container_of(napi, struct can_rx_offload, napi);
	struct net_device *dev = offload->dev;
	struct net_device_stats *stats = &dev->stats;
	struct sk_buff *skb;
	int work_done = 0;

	while ((work_done < quota) &&
	       (skb = skb_dequeue(&offload->skb_queue))) {
		struct canfd_frame *cf = (struct canfd_frame *)skb->data;

		work_done++;
		stats->rx_packets++;
		stats->rx_bytes += cf->len;
		netif_receive_skb(skb);
	}

	if (work_done < quota) {
		// napi_complete_done(napi, work_done);
		napi_complete(napi);

		/* Check if there was another interrupt */
		if (!skb_queue_empty(&offload->skb_queue))
			napi_reschedule(&offload->napi);
	}

	can_led_event(offload->dev, CAN_LED_EVENT_RX);

	return work_done;
}

static void can_rx_offload_reset(struct can_rx_offload *offload)
{
}
static int can_rx_offload_init_queue(struct net_device *dev, struct can_rx_offload *offload, unsigned int weight)
{
	offload->dev = dev;

	/* Limit queue len to 4x the weight (rounted to next power of two) */
	offload->skb_queue_len_max = 2 << fls(weight);
	offload->skb_queue_len_max *= 4;
	skb_queue_head_init(&offload->skb_queue);

	can_rx_offload_reset(offload);
	netif_napi_add(dev, &offload->napi, can_rx_offload_napi_poll, weight);

	dev_dbg(dev->dev.parent, "%s: skb_queue_len_max=%d\n",
		__func__, offload->skb_queue_len_max);

	return 0;
}


int can_rx_offload_add_fifo(struct net_device *dev, struct can_rx_offload *offload, unsigned int weight)
{
	// if (!offload->mailbox_read)
	// 	return -EINVAL;

	return can_rx_offload_init_queue(dev, offload, weight);
}

static int flexcan_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	const struct flexcan_priv *priv = netdev_priv(dev);
	struct flexcan_regs __iomem *regs = priv->base;
	struct can_frame *cf = (struct can_frame *)skb->data;
	u32 can_id;
	u32 ctrl = FLEXCAN_MB_CNT_CODE(0xc) | (cf->can_dlc << 16);

	if (can_dropped_invalid_skb(dev, skb))
		return NETDEV_TX_OK;

	netif_stop_queue(dev);

	if (cf->can_id & CAN_EFF_FLAG) {
		can_id = cf->can_id & CAN_EFF_MASK;
		ctrl |= FLEXCAN_MB_CNT_IDE | FLEXCAN_MB_CNT_SRR;
	} else {
		can_id = (cf->can_id & CAN_SFF_MASK) << 18;
	}

	if (cf->can_id & CAN_RTR_FLAG)
		ctrl |= FLEXCAN_MB_CNT_RTR;

	if (cf->can_dlc > 0) {
		u32 data = be32_to_cpup((__be32 *)&cf->data[0]);
		flexcan_write(data, &regs->cantxfg[FLEXCAN_TX_BUF_ID].data[0]);
	}
	if (cf->can_dlc > 3) {
		u32 data = be32_to_cpup((__be32 *)&cf->data[4]);
		flexcan_write(data, &regs->cantxfg[FLEXCAN_TX_BUF_ID].data[1]);
	}

	can_put_echo_skb(skb, dev, 0);

	flexcan_write(can_id, &regs->cantxfg[FLEXCAN_TX_BUF_ID].can_id);
	flexcan_write(ctrl, &regs->cantxfg[FLEXCAN_TX_BUF_ID].can_ctrl);

	if (priv->devtype_data->features & FLEXCAN_HAS_ERR005829) {
		writel(0x0, &regs->cantxfg[FLEXCAN_RESERVED_BUF_ID].can_ctrl);
		writel(0x0, &regs->cantxfg[FLEXCAN_RESERVED_BUF_ID].can_ctrl);
	}

	return NETDEV_TX_OK;
}

static void do_bus_err(struct net_device *dev,
		       struct can_frame *cf, u32 reg_esr)
{
	struct flexcan_priv *priv = netdev_priv(dev);
	int rx_errors = 0, tx_errors = 0;

	cf->can_id |= CAN_ERR_PROT | CAN_ERR_BUSERROR;

	if (reg_esr & FLEXCAN_ESR_BIT1_ERR) {
		netdev_dbg(dev, "BIT1_ERR irq\n");
		printk("bit1 err irq!\n");
		cf->data[2] |= CAN_ERR_PROT_BIT1;
		tx_errors = 1;
	}
	if (reg_esr & FLEXCAN_ESR_BIT0_ERR) {
		netdev_dbg(dev, "BIT0_ERR irq\n");
		printk("BIT0_ERR irq!\n");
		cf->data[2] |= CAN_ERR_PROT_BIT0;
		tx_errors = 1;
	}
	if (reg_esr & FLEXCAN_ESR_ACK_ERR) {
		netdev_dbg(dev, "ACK_ERR irq\n");
		printk("ACK_ERR irq!\n");
		cf->can_id |= CAN_ERR_ACK;
		cf->data[3] |= CAN_ERR_PROT_LOC_ACK;
		tx_errors = 1;
	}
	if (reg_esr & FLEXCAN_ESR_CRC_ERR) {
		netdev_dbg(dev, "CRC_ERR irq\n");
		printk("CRC_ERR irq\n");
		cf->data[2] |= CAN_ERR_PROT_BIT;
		cf->data[3] |= CAN_ERR_PROT_LOC_CRC_SEQ;
		rx_errors = 1;
	}
	if (reg_esr & FLEXCAN_ESR_FRM_ERR) {
		netdev_dbg(dev, "FRM_ERR irq\n");
		printk("FRM_ERR irq\n");
		cf->data[2] |= CAN_ERR_PROT_FORM;
		rx_errors = 1;
	}
	if (reg_esr & FLEXCAN_ESR_STF_ERR) {
		netdev_dbg(dev, "STF_ERR irq\n");
		printk("STF_ERR irq\n");
		cf->data[2] |= CAN_ERR_PROT_STUFF;
		rx_errors = 1;
	}

	priv->can.can_stats.bus_error++;
	if (rx_errors)
		dev->stats.rx_errors++;
	if (tx_errors)
		dev->stats.tx_errors++;
}

static int flexcan_poll_bus_err(struct net_device *dev, u32 reg_esr)
{
	struct sk_buff *skb;
	struct can_frame *cf;

	skb = alloc_can_err_skb(dev, &cf);
	if (unlikely(!skb))
		return 0;

	do_bus_err(dev, cf, reg_esr);
	netif_receive_skb(skb);

	dev->stats.rx_packets++;
	dev->stats.rx_bytes += cf->can_dlc;

	return 1;
}

static void do_state(struct net_device *dev,
		     struct can_frame *cf, enum can_state new_state)
{
	struct flexcan_priv *priv = netdev_priv(dev);
	struct can_berr_counter bec;

	flexcan_get_berr_counter(dev, &bec);

	switch (priv->can.state) {
	case CAN_STATE_ERROR_ACTIVE:
		/*
		 * from: ERROR_ACTIVE
		 * to  : ERROR_WARNING, ERROR_PASSIVE, BUS_OFF
		 * =>  : there was a warning int
		 */
		if (new_state >= CAN_STATE_ERROR_WARNING &&
		    new_state <= CAN_STATE_BUS_OFF) {
			netdev_dbg(dev, "Error Warning IRQ\n");
			priv->can.can_stats.error_warning++;

			cf->can_id |= CAN_ERR_CRTL;
			cf->data[1] = (bec.txerr > bec.rxerr) ?
				CAN_ERR_CRTL_TX_WARNING :
				CAN_ERR_CRTL_RX_WARNING;
		}
	case CAN_STATE_ERROR_WARNING:	/* fallthrough */
		/*
		 * from: ERROR_ACTIVE, ERROR_WARNING
		 * to  : ERROR_PASSIVE, BUS_OFF
		 * =>  : error passive int
		 */
		if (new_state >= CAN_STATE_ERROR_PASSIVE &&
		    new_state <= CAN_STATE_BUS_OFF) {
			netdev_dbg(dev, "Error Passive IRQ\n");
			priv->can.can_stats.error_passive++;

			cf->can_id |= CAN_ERR_CRTL;
			cf->data[1] = (bec.txerr > bec.rxerr) ?
				CAN_ERR_CRTL_TX_PASSIVE :
				CAN_ERR_CRTL_RX_PASSIVE;
		}
		break;
	case CAN_STATE_BUS_OFF:
		netdev_err(dev, "BUG! "
			   "hardware recovered automatically from BUS_OFF\n");
		break;
	default:
		break;
	}

	/* process state changes depending on the new state */
	switch (new_state) {
	case CAN_STATE_ERROR_ACTIVE:
		netdev_dbg(dev, "Error Active\n");
		cf->can_id |= CAN_ERR_PROT;
		cf->data[2] = CAN_ERR_PROT_ACTIVE;
		break;
	case CAN_STATE_BUS_OFF:
		cf->can_id |= CAN_ERR_BUSOFF;
		can_bus_off(dev);
		break;
	default:
		break;
	}
}

static int flexcan_poll_state(struct net_device *dev, u32 reg_esr)
{
	struct flexcan_priv *priv = netdev_priv(dev);
	struct sk_buff *skb;
	struct can_frame *cf;
	enum can_state new_state;
	int flt;

	flt = reg_esr & FLEXCAN_ESR_FLT_CONF_MASK;
	if (likely(flt == FLEXCAN_ESR_FLT_CONF_ACTIVE)) {
		if (likely(!(reg_esr & (FLEXCAN_ESR_TX_WRN |
					FLEXCAN_ESR_RX_WRN))))
			new_state = CAN_STATE_ERROR_ACTIVE;
		else
			new_state = CAN_STATE_ERROR_WARNING;
	} else if (unlikely(flt == FLEXCAN_ESR_FLT_CONF_PASSIVE))
		new_state = CAN_STATE_ERROR_PASSIVE;
	else
		new_state = CAN_STATE_BUS_OFF;

	/* state hasn't changed */
	if (likely(new_state == priv->can.state))
		return 0;

	skb = alloc_can_err_skb(dev, &cf);
	if (unlikely(!skb))
		return 0;

	do_state(dev, cf, new_state);
	priv->can.state = new_state;
	netif_receive_skb(skb);

	dev->stats.rx_packets++;
	dev->stats.rx_bytes += cf->can_dlc;

	return 1;
}

static void flexcan_read_fifo(const struct net_device *dev,
			      struct can_frame *cf)
{
	const struct flexcan_priv *priv = netdev_priv(dev);
	struct flexcan_regs __iomem *regs = priv->base;
	struct flexcan_mb __iomem *mb = &regs->cantxfg[0];
	u32 reg_ctrl, reg_id;

	reg_ctrl = flexcan_read(&mb->can_ctrl);
	reg_id = flexcan_read(&mb->can_id);
	if (reg_ctrl & FLEXCAN_MB_CNT_IDE)
		cf->can_id = ((reg_id >> 0) & CAN_EFF_MASK) | CAN_EFF_FLAG;
	else
		cf->can_id = (reg_id >> 18) & CAN_SFF_MASK;

	if (reg_ctrl & FLEXCAN_MB_CNT_RTR)
		cf->can_id |= CAN_RTR_FLAG;
	cf->can_dlc = get_can_dlc((reg_ctrl >> 16) & 0xf);

	*(__be32 *)(cf->data + 0) = cpu_to_be32(flexcan_read(&mb->data[0]));
	*(__be32 *)(cf->data + 4) = cpu_to_be32(flexcan_read(&mb->data[1]));

	/* mark as read */
	flexcan_write(FLEXCAN_IFLAG_RX_FIFO_AVAILABLE, &regs->iflag1);
	flexcan_read(&regs->timer);
}

static int flexcan_read_frame(struct net_device *dev)
{
	struct net_device_stats *stats = &dev->stats;
	struct can_frame *cf;
	struct sk_buff *skb;

	skb = alloc_can_skb(dev, &cf);
	if (unlikely(!skb)) {
		stats->rx_dropped++;
		return 0;
	}

	flexcan_read_fifo(dev, cf);
	netif_receive_skb(skb);

	stats->rx_packets++;
	stats->rx_bytes += cf->can_dlc;

	can_led_event(dev, CAN_LED_EVENT_RX);

	return 1;
}
#ifndef IRQ_CAN_RX
static int flexcan_poll(struct napi_struct *napi, int quota)
{
	struct net_device *dev = napi->dev;
	const struct flexcan_priv *priv = netdev_priv(dev);
	struct flexcan_regs __iomem *regs = priv->base;
	u32 reg_iflag1, reg_esr;
	int work_done = 0;

	/*
	 * The error bits are cleared on read,
	 * use saved value from irq handler.
	 */
	reg_esr = flexcan_read(&regs->esr) | priv->reg_esr;

	/* handle state changes */
	work_done += flexcan_poll_state(dev, reg_esr);

	/* handle RX-FIFO */
	reg_iflag1 = flexcan_read(&regs->iflag1);
	while (reg_iflag1 & FLEXCAN_IFLAG_RX_FIFO_AVAILABLE &&
	       work_done < quota) {
		work_done += flexcan_read_frame(dev);
		reg_iflag1 = flexcan_read(&regs->iflag1);
	}

	/* report bus errors */
	if (flexcan_has_and_handle_berr(priv, reg_esr) && work_done < quota)
		work_done += flexcan_poll_bus_err(dev, reg_esr);

	if (work_done < quota) {
		napi_complete(napi);
		/* enable IRQs */
		flexcan_write(FLEXCAN_IFLAG_DEFAULT, &regs->imask1);
		flexcan_write(priv->reg_ctrl_default, &regs->ctrl);
	}

	return work_done;
}
#endif 

static inline struct flexcan_priv *rx_offload_to_priv(struct can_rx_offload *offload)
{
	return container_of(offload, struct flexcan_priv, offload);
}
static unsigned int flexcan_mailbox_read(struct can_rx_offload *offload, bool drop,
					 struct sk_buff **skb, u32 *timestamp,
					 unsigned int n)
{
	// struct flexcan_priv *priv = rx_offload_to_priv(offload);
	// struct net_device *dev = offload->dev;

	// u32 reg_ctrl, reg_id, reg_iflag1;

	// reg_iflag1 = flexcan_read(&regs->iflag1); 
	// if (!(reg_iflag1 & FLEXCAN_IFLAG_RX_FIFO_AVAILABLE))
	// 		return 0;
	// struct net_device_stats *stats = &dev->stats;
	// struct can_frame *cf = NULL;

	// *skb = alloc_can_skb(dev, &cf);
	// if (unlikely(!skb)) {
	// 	printk("hndz can alloc fail!\n");
	// 	stats->rx_dropped++;
	// 	return 0;
	// }

	// return new_flexcan_read_fifo(dev, cf);

	const struct flexcan_priv *priv = rx_offload_to_priv(offload);
	struct flexcan_regs __iomem *regs = priv->base;
	struct flexcan_mb __iomem *mb = &regs->cantxfg[0];
	u32 reg_ctrl, reg_id, reg_iflag1;
	// struct canfd_frame *cf = NULL;
	struct can_frame  *cf = NULL;
	reg_iflag1 = flexcan_read(&regs->iflag1); 
	if (!(reg_iflag1 & FLEXCAN_IFLAG_RX_FIFO_AVAILABLE))
			return 0;

	if (!drop) {
			*skb = alloc_can_skb(offload->dev,
					     &cf);
	}
	if(*skb && cf)
	{
		reg_ctrl = flexcan_read(&mb->can_ctrl);
		reg_id = flexcan_read(&mb->can_id);
		if (reg_ctrl & FLEXCAN_MB_CNT_IDE)
			cf->can_id = ((reg_id >> 0) & CAN_EFF_MASK) | CAN_EFF_FLAG;
		else
			cf->can_id = (reg_id >> 18) & CAN_SFF_MASK;

		if (reg_ctrl & FLEXCAN_MB_CNT_RTR)
			cf->can_id |= CAN_RTR_FLAG;
		cf->can_dlc = get_can_dlc((reg_ctrl >> 16) & 0xf);

		*(__be32 *)(cf->data + 0) = cpu_to_be32(flexcan_read(&mb->data[0]));
		*(__be32 *)(cf->data + 4) = cpu_to_be32(flexcan_read(&mb->data[1]));
	}
	/* mark as read */
	flexcan_write(FLEXCAN_IFLAG_RX_FIFO_AVAILABLE, &regs->iflag1);
	flexcan_read(&regs->timer);
	return 1;

	//return 1;
}

static struct sk_buff *can_rx_offload_offload_one(struct can_rx_offload *offload, unsigned int n)
{
	struct sk_buff *skb = NULL;
	u32 timestamp;

	/* If queue is full or skb not available, read to discard mailbox */
	bool drop = unlikely(skb_queue_len(&offload->skb_queue) >
					   offload->skb_queue_len_max);

	// if (offload->mailbox_read(offload, drop, &skb, &timestamp, n) && !skb)
	// 	offload->dev->stats.rx_dropped++;
	if (flexcan_mailbox_read(offload, drop, &skb, &timestamp, n) && !skb)
		offload->dev->stats.rx_dropped++;

	// if (skb) {
	// 	struct can_rx_offload_cb *cb = can_rx_offload_get_cb(skb);

	// 	cb->timestamp = timestamp;
	// }

	return skb;
}
static inline void can_rx_offload_schedule(struct can_rx_offload *offload)
{
	napi_schedule(&offload->napi);
}

// static inline void can_rx_offload_disable(struct can_rx_offload *offload)
// {
// 	napi_disable(&offload->napi);
// }
int can_rx_offload_irq_offload_fifo(struct can_rx_offload *offload)
{
	struct sk_buff *skb;
	int received = 0;
	// printk("zty canrx offload!\n");
	while ((skb = can_rx_offload_offload_one(offload, 0))) {
		// printk("zty add!\n");
		skb_queue_tail(&offload->skb_queue, skb);
		received++;
	}

	if (received)
		can_rx_offload_schedule(offload);

	return received;
}
#ifndef IRQ_CAN_RX
static irqreturn_t flexcan_irq(int irq, void *dev_id)
{
	struct net_device *dev = dev_id;
	struct net_device_stats *stats = &dev->stats;
	struct flexcan_priv *priv = netdev_priv(dev);
	struct flexcan_regs __iomem *regs = priv->base;
	u32 reg_iflag1, reg_esr;

	reg_iflag1 = flexcan_read(&regs->iflag1);
	reg_esr = flexcan_read(&regs->esr);
	/* ACK all bus error and state change IRQ sources */
	if (reg_esr & FLEXCAN_ESR_ALL_INT)
		flexcan_write(reg_esr & FLEXCAN_ESR_ALL_INT, &regs->esr);

	if (reg_esr & FLEXCAN_ESR_WAK_INT)
		flexcan_exit_stop_mode(priv);

	/*
	 * schedule NAPI in case of:
	 * - rx IRQ
	 * - state change IRQ
	 * - bus error IRQ and bus error reporting is activated
	 */
	if ((reg_iflag1 & FLEXCAN_IFLAG_RX_FIFO_AVAILABLE) ||
	    (reg_esr & FLEXCAN_ESR_ERR_STATE) ||
	    flexcan_has_and_handle_berr(priv, reg_esr)) {
		/*
		 * The error bits are cleared on read,
		 * save them for later use.
		 */
		priv->reg_esr = reg_esr & FLEXCAN_ESR_ERR_BUS;
		flexcan_write(FLEXCAN_IFLAG_DEFAULT &
			~FLEXCAN_IFLAG_RX_FIFO_AVAILABLE, &regs->imask1);
		flexcan_write(priv->reg_ctrl_default & ~FLEXCAN_CTRL_ERR_ALL,
		       &regs->ctrl);
		napi_schedule(&priv->napi);
	}

	/* FIFO overflow */
	if (reg_iflag1 & FLEXCAN_IFLAG_RX_FIFO_OVERFLOW) {
		flexcan_write(FLEXCAN_IFLAG_RX_FIFO_OVERFLOW, &regs->iflag1);
		dev->stats.rx_over_errors++;
		dev->stats.rx_errors++;
	}

	/* transmission complete interrupt */
	if (reg_iflag1 & (1 << FLEXCAN_TX_BUF_ID)) {
		stats->tx_bytes += can_get_echo_skb(dev, 0);
		stats->tx_packets++;
		can_led_event(dev, CAN_LED_EVENT_TX);
		flexcan_write((1 << FLEXCAN_TX_BUF_ID), &regs->iflag1);
		netif_wake_queue(dev);
	}

	return IRQ_HANDLED;
}
#endif
// extern unsigned long hndz_read_current_timer(void);
static irqreturn_t flexcan_napi_irq(int irq, void *dev_id)
{
	struct net_device *dev = dev_id;
	struct net_device_stats *stats = &dev->stats;
	struct flexcan_priv *priv = netdev_priv(dev);
	struct flexcan_regs __iomem *regs = priv->base;
	u32 reg_iflag1, reg_esr;
	irqreturn_t handled = IRQ_NONE;
	// unsigned long time1, time2;
	u32 rvnum = 0;
	// time1 = hndz_read_current_timer();
	reg_iflag1 = flexcan_read(&regs->iflag1);
	reg_esr = flexcan_read(&regs->esr);
	/* ACK all bus error and state change IRQ sources */
	// if (reg_esr & FLEXCAN_ESR_ALL_INT)
	// 	flexcan_write(reg_esr & FLEXCAN_ESR_ALL_INT, &regs->esr);

	// if (reg_esr & FLEXCAN_ESR_WAK_INT)
	// 	flexcan_exit_stop_mode(priv);

	/*
	 * schedule NAPI in case of:
	 * - rx IRQ
	 * - state change IRQ
	 * - bus error IRQ and bus error reporting is activated
	 */

	if(reg_iflag1 & FLEXCAN_IFLAG_RX_FIFO_AVAILABLE){
		handled = IRQ_HANDLED;
		rvnum = can_rx_offload_irq_offload_fifo(&priv->offload);
	}

		/* FIFO overflow */
	if (reg_iflag1 & FLEXCAN_IFLAG_RX_FIFO_OVERFLOW) {
		handled = IRQ_HANDLED;
		flexcan_write(FLEXCAN_IFLAG_RX_FIFO_OVERFLOW, &regs->iflag1);
		dev->stats.rx_over_errors++;
		dev->stats.rx_errors++;
	}

	// if ((reg_iflag1 & FLEXCAN_IFLAG_RX_FIFO_AVAILABLE) ||
	//     (reg_esr & FLEXCAN_ESR_ERR_STATE) ||
	//     flexcan_has_and_handle_berr(priv, reg_esr)) {
	// 	/*
	// 	 * The error bits are cleared on read,
	// 	 * save them for later use.
	// 	 */
	// 	priv->reg_esr = reg_esr & FLEXCAN_ESR_ERR_BUS;
	// 	flexcan_write(FLEXCAN_IFLAG_DEFAULT &
	// 		~FLEXCAN_IFLAG_RX_FIFO_AVAILABLE, &regs->imask1);
	// 	flexcan_write(priv->reg_ctrl_default & ~FLEXCAN_CTRL_ERR_ALL,
	// 	       &regs->ctrl);
	// 	napi_schedule(&priv->napi);
	// }

	// /* FIFO overflow */
	// if (reg_iflag1 & FLEXCAN_IFLAG_RX_FIFO_OVERFLOW) {
	// 	flexcan_write(FLEXCAN_IFLAG_RX_FIFO_OVERFLOW, &regs->iflag1);
	// 	dev->stats.rx_over_errors++;
	// 	dev->stats.rx_errors++;
	// }

	/* transmission complete interrupt */
	if (reg_iflag1 & (1 << FLEXCAN_TX_BUF_ID)) {
		handled = IRQ_HANDLED;
		stats->tx_bytes += can_get_echo_skb(dev, 0);
		stats->tx_packets++;
		can_led_event(dev, CAN_LED_EVENT_TX);
		flexcan_write((1 << FLEXCAN_TX_BUF_ID), &regs->iflag1);
		netif_wake_queue(dev);
	}

	if (reg_esr & FLEXCAN_ESR_ALL_INT) {
		handled = IRQ_HANDLED;
		flexcan_write(reg_esr & FLEXCAN_ESR_ALL_INT, &regs->esr);
	}

	if(reg_esr & FLEXCAN_ESR_ERR_STATE)
	{
		printk("hndz flexcan FLEXCAN_ESR_ERR_STATE error 0x%x!\n", reg_esr);
		//error process
	}

	if(reg_esr & FLEXCAN_ESR_ERR_BUS)
	{
		printk("hndz flexcan FLEXCAN_ESR_ERR_BUS error 0x%x!\n", reg_esr);
		//error process
	}
	// time2 = hndz_read_current_timer();
	// if(rvnum > 0)
	// 	printk("hndz can time %lu num %d!\n", time2-time1, rvnum);

	return IRQ_HANDLED;
}

void can_rx_offload_enable(struct can_rx_offload *offload)
{
//	can_rx_offload_reset(offload);
	napi_enable(&offload->napi);
}

static void flexcan_set_bittiming(struct net_device *dev)
{
	const struct flexcan_priv *priv = netdev_priv(dev);
	const struct can_bittiming *bt = &priv->can.bittiming;
	struct flexcan_regs __iomem *regs = priv->base;
	u32 reg;

	reg = flexcan_read(&regs->ctrl);
	reg &= ~(FLEXCAN_CTRL_PRESDIV(0xff) |
		 FLEXCAN_CTRL_RJW(0x3) |
		 FLEXCAN_CTRL_PSEG1(0x7) |
		 FLEXCAN_CTRL_PSEG2(0x7) |
		 FLEXCAN_CTRL_PROPSEG(0x7) |
		 FLEXCAN_CTRL_LPB |
		 FLEXCAN_CTRL_SMP |
		 FLEXCAN_CTRL_LOM);

	reg |= FLEXCAN_CTRL_PRESDIV(bt->brp - 1) |
		FLEXCAN_CTRL_PSEG1(bt->phase_seg1 - 1) |
		FLEXCAN_CTRL_PSEG2(bt->phase_seg2 - 1) |
		FLEXCAN_CTRL_RJW(bt->sjw - 1) |
		FLEXCAN_CTRL_PROPSEG(bt->prop_seg - 1);

	if (priv->can.ctrlmode & CAN_CTRLMODE_LOOPBACK)
		reg |= FLEXCAN_CTRL_LPB;
	if (priv->can.ctrlmode & CAN_CTRLMODE_LISTENONLY)
		reg |= FLEXCAN_CTRL_LOM;
	if (priv->can.ctrlmode & CAN_CTRLMODE_3_SAMPLES)
		reg |= FLEXCAN_CTRL_SMP;

	netdev_info(dev, "writing ctrl=0x%08x\n", reg);
	flexcan_write(reg, &regs->ctrl);

	/* print chip status */
	netdev_dbg(dev, "%s: mcr=0x%08x ctrl=0x%08x\n", __func__,
		   flexcan_read(&regs->mcr), flexcan_read(&regs->ctrl));
}

/*
 * flexcan_chip_start
 *
 * this functions is entered with clocks enabled
 *
 */
static int flexcan_chip_start(struct net_device *dev)
{
	struct flexcan_priv *priv = netdev_priv(dev);
	struct flexcan_regs __iomem *regs = priv->base;
	int err;
	u32 reg_mcr, reg_ctrl;
	int i;

	/* enable module */
	err = flexcan_chip_enable(priv);
	if (err)
		return err;

	/* soft reset */
	err = flexcan_chip_softreset(priv);
	if (err)
		goto out_chip_disable;

	flexcan_set_bittiming(dev);

	/*
	 * MCR
	 *
	 * enable freeze
	 * enable fifo
	 * halt now
	 * only supervisor access
	 * enable warning int
	 * choose format C
	 * disable local echo
	 * enable self wakeup
	 */
	reg_mcr = flexcan_read(&regs->mcr);
	reg_mcr &= ~FLEXCAN_MCR_MAXMB(0xff);
	reg_mcr |= FLEXCAN_MCR_FRZ | FLEXCAN_MCR_FEN | FLEXCAN_MCR_HALT |
		FLEXCAN_MCR_SUPV | FLEXCAN_MCR_WRN_EN |
		FLEXCAN_MCR_IDAM_C | FLEXCAN_MCR_SRX_DIS |
		FLEXCAN_MCR_WAK_MSK | FLEXCAN_MCR_SLF_WAK |
		FLEXCAN_MCR_MAXMB(FLEXCAN_TX_BUF_ID);
	netdev_dbg(dev, "%s: writing mcr=0x%08x", __func__, reg_mcr);
	flexcan_write(reg_mcr, &regs->mcr);

	/*
	 * CTRL
	 *
	 * disable timer sync feature
	 *
	 * disable auto busoff recovery
	 * transmit lowest buffer first
	 *
	 * enable tx and rx warning interrupt
	 * enable bus off interrupt
	 * (== FLEXCAN_CTRL_ERR_STATE)
	 */
	reg_ctrl = flexcan_read(&regs->ctrl);
	reg_ctrl &= ~FLEXCAN_CTRL_TSYN;
	reg_ctrl |= FLEXCAN_CTRL_BOFF_REC | FLEXCAN_CTRL_LBUF |
		FLEXCAN_CTRL_ERR_STATE;
	/*
	 * enable the "error interrupt" (FLEXCAN_CTRL_ERR_MSK),
	 * on most Flexcan cores, too. Otherwise we don't get
	 * any error warning or passive interrupts.
	 */
	if (priv->devtype_data->features & FLEXCAN_HAS_BROKEN_ERR_STATE ||
	    priv->can.ctrlmode & CAN_CTRLMODE_BERR_REPORTING)
		reg_ctrl |= FLEXCAN_CTRL_ERR_MSK;

	/* save for later use */
	priv->reg_ctrl_default = reg_ctrl;
	netdev_dbg(dev, "%s: writing ctrl=0x%08x", __func__, reg_ctrl);
	flexcan_write(reg_ctrl, &regs->ctrl);

	// printk("zty pri can ctrlmode is 0x%x!\n", priv->can.ctrlmode);
	/* Abort any pending TX, mark Mailbox as INACTIVE */
	// flexcan_write(FLEXCAN_MB_CNT_CODE(0x4),
	// 	      &regs->cantxfg[FLEXCAN_TX_BUF_ID].can_ctrl);

	for (i = 0; i < ARRAY_SIZE(regs->cantxfg); i++) {
		writel(0, &regs->cantxfg[i].can_ctrl);
		writel(0, &regs->cantxfg[i].can_id);
		writel(0, &regs->cantxfg[i].data[0]);
		writel(0, &regs->cantxfg[i].data[1]);

		/* put MB into rx queue */
		writel(FLEXCAN_MB_CNT_CODE(0x4), &regs->cantxfg[i].can_ctrl);
	}

	/* acceptance mask/acceptance code (accept everything) */
	flexcan_write(0x0, &regs->rxgmask);
	flexcan_write(0x0, &regs->rx14mask);
	flexcan_write(0x0, &regs->rx15mask);

	if (priv->devtype_data->features & FLEXCAN_HAS_V10_FEATURES)
		flexcan_write(0x0, &regs->rxfgmask);

	err = flexcan_transceiver_enable(priv);
	if (err)
		goto out_chip_disable;

	/* synchronize with the can bus */
	err = flexcan_chip_unfreeze(priv);
	if (err)
		goto out_transceiver_disable;

	priv->can.state = CAN_STATE_ERROR_ACTIVE;

	/* enable FIFO interrupts */
	flexcan_write(FLEXCAN_IFLAG_DEFAULT, &regs->imask1);

	/* print chip status */
	netdev_dbg(dev, "%s: reading mcr=0x%08x ctrl=0x%08x\n", __func__,
		   flexcan_read(&regs->mcr), flexcan_read(&regs->ctrl));

	return 0;

 out_transceiver_disable:
	flexcan_transceiver_disable(priv);
 out_chip_disable:
	flexcan_chip_disable(priv);
	return err;
}

/*
 * flexcan_chip_stop
 *
 * this functions is entered with clocks enabled
 *
 */
static void flexcan_chip_stop(struct net_device *dev)
{
	struct flexcan_priv *priv = netdev_priv(dev);
	struct flexcan_regs __iomem *regs = priv->base;

	/* freeze + disable module */
	flexcan_chip_freeze(priv);
	flexcan_chip_disable(priv);

	/* Disable all interrupts */
	flexcan_write(0, &regs->imask1);
	flexcan_write(priv->reg_ctrl_default & ~FLEXCAN_CTRL_ERR_ALL,
		      &regs->ctrl);

	flexcan_transceiver_disable(priv);
	priv->can.state = CAN_STATE_STOPPED;

	return;
}

static int flexcan_open(struct net_device *dev)
{
	struct flexcan_priv *priv = netdev_priv(dev);
	int err;

	err = clk_prepare_enable(priv->clk_ipg);
	if (err)
		return err;

	err = clk_prepare_enable(priv->clk_per);
	if (err)
		goto out_disable_ipg;

	err = open_candev(dev);
	if (err)
		goto out_disable_per;

#ifndef IRQ_CAN_RX
	err = request_irq(dev->irq, flexcan_irq, IRQF_SHARED, dev->name, dev);
	if (err)
		goto out_close;
#else
	err = request_irq(dev->irq, flexcan_napi_irq, IRQF_SHARED, dev->name, dev);
	if (err)
		goto out_close;
 	err = can_rx_offload_add_fifo(dev, &priv->offload, FLEXCAN_NAPI_WEIGHT);
	if(err)
		goto out_free_irq;
#endif

	/* start chip and queuing */
	err = flexcan_chip_start(dev);
	if (err)
		goto out_free_irq;

	can_led_event(dev, CAN_LED_EVENT_OPEN);
#ifndef IRQ_CAN_RX
	napi_enable(&priv->napi);
#else
	can_rx_offload_enable(&priv->offload);
#endif
	netif_start_queue(dev);

	return 0;

 out_free_irq:
	free_irq(dev->irq, dev);
 out_close:
	close_candev(dev);
 out_disable_per:
	clk_disable_unprepare(priv->clk_per);
 out_disable_ipg:
	clk_disable_unprepare(priv->clk_ipg);

	return err;
}

static int flexcan_close(struct net_device *dev)
{
	struct flexcan_priv *priv = netdev_priv(dev);

	netif_stop_queue(dev);
	napi_disable(&priv->napi);
	flexcan_chip_stop(dev);

	free_irq(dev->irq, dev);
	clk_disable_unprepare(priv->clk_per);
	clk_disable_unprepare(priv->clk_ipg);

	close_candev(dev);

	can_led_event(dev, CAN_LED_EVENT_STOP);

	return 0;
}

static int flexcan_set_mode(struct net_device *dev, enum can_mode mode)
{
	int err;

	switch (mode) {
	case CAN_MODE_START:
		err = flexcan_chip_start(dev);
		if (err)
			return err;

		netif_wake_queue(dev);
		break;

	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static const struct net_device_ops flexcan_netdev_ops = {
	.ndo_open	= flexcan_open,
	.ndo_stop	= flexcan_close,
	.ndo_start_xmit	= flexcan_start_xmit,
};

static int register_flexcandev(struct net_device *dev)
{
	struct flexcan_priv *priv = netdev_priv(dev);
	struct flexcan_regs __iomem *regs = priv->base;
	u32 reg, err;

	err = clk_prepare_enable(priv->clk_ipg);
	if (err)
		return err;

	err = clk_prepare_enable(priv->clk_per);
	if (err)
		goto out_disable_ipg;

	/* select "bus clock", chip must be disabled */
	err = flexcan_chip_disable(priv);
	if (err)
		goto out_disable_per;
	reg = flexcan_read(&regs->ctrl);
	reg |= FLEXCAN_CTRL_CLK_SRC;
	flexcan_write(reg, &regs->ctrl);

	err = flexcan_chip_enable(priv);
	if (err)
		goto out_chip_disable;

	/* set freeze, halt and activate FIFO, restrict register access */
	reg = flexcan_read(&regs->mcr);
	reg |= FLEXCAN_MCR_FRZ | FLEXCAN_MCR_HALT |
		FLEXCAN_MCR_FEN | FLEXCAN_MCR_SUPV;
	flexcan_write(reg, &regs->mcr);

	/*
	 * Currently we only support newer versions of this core
	 * featuring a RX FIFO. Older cores found on some Coldfire
	 * derivates are not yet supported.
	 */
	reg = flexcan_read(&regs->mcr);
	if (!(reg & FLEXCAN_MCR_FEN)) {
		netdev_err(dev, "Could not enable RX FIFO, unsupported core\n");
		err = -ENODEV;
		goto out_chip_disable;
	}

	err = register_candev(dev);

	/* disable core and turn off clocks */
 out_chip_disable:
	flexcan_chip_disable(priv);
 out_disable_per:
	clk_disable_unprepare(priv->clk_per);
 out_disable_ipg:
	clk_disable_unprepare(priv->clk_ipg);

	return err;
}

static void unregister_flexcandev(struct net_device *dev)
{
	unregister_candev(dev);
}

static int flexcan_of_parse_stop_mode(struct platform_device *pdev)
{
	struct net_device *dev = platform_get_drvdata(pdev);
	struct device_node *np = pdev->dev.of_node;
	struct device_node *node;
	struct flexcan_priv *priv;
	phandle phandle;
	u32 out_val[5];
	int ret;

	if (!np)
		return -EINVAL;
	/*
	 * stop mode property format is:
	 * <&gpr req_gpr req_bit ack_gpr ack_bit>.
	 */
	ret = of_property_read_u32_array(np, "stop-mode", out_val, 5);
	if (ret) {
		dev_dbg(&pdev->dev, "no stop-mode property\n");
		return ret;
	}
	phandle = *out_val;

	node = of_find_node_by_phandle(phandle);
	if (!node) {
		dev_dbg(&pdev->dev, "could not find gpr node by phandle\n");
		return PTR_ERR(node);
	}

	priv = netdev_priv(dev);
	priv->stm.gpr = syscon_node_to_regmap(node);
	if (IS_ERR(priv->stm.gpr)) {
		dev_dbg(&pdev->dev, "could not find gpr regmap\n");
		return PTR_ERR(priv->stm.gpr);
	}

	of_node_put(node);

	priv->stm.req_gpr = out_val[1];
	priv->stm.req_bit = out_val[2];
	priv->stm.ack_gpr = out_val[3];
	priv->stm.ack_bit = out_val[4];

	dev_dbg(&pdev->dev, "gpr %s req_gpr 0x%x req_bit %u ack_gpr 0x%x ack_bit %u\n",
			node->full_name, priv->stm.req_gpr,
			priv->stm.req_bit, priv->stm.ack_gpr,
			priv->stm.ack_bit);
	return 0;
}

static const struct of_device_id flexcan_of_match[] = {
	{ .compatible = "fsl,imx6q-flexcan", .data = &fsl_imx6q_devtype_data, },
	{ .compatible = "fsl,imx28-flexcan", .data = &fsl_imx28_devtype_data, },
	{ .compatible = "fsl,p1010-flexcan", .data = &fsl_p1010_devtype_data, },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, flexcan_of_match);

static const struct platform_device_id flexcan_id_table[] = {
	{ .name = "flexcan", .driver_data = (kernel_ulong_t)&fsl_p1010_devtype_data, },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(platform, flexcan_id_table);

static int flexcan_probe(struct platform_device *pdev)
{
	const struct of_device_id *of_id;
	const struct flexcan_devtype_data *devtype_data;
	struct net_device *dev;
	struct flexcan_priv *priv;
	struct resource *mem;
	struct clk *clk_ipg = NULL, *clk_per = NULL;
	void __iomem *base;
	int err, irq;
	u32 clock_freq = 0;
	int wakeup = 1;

	if (pdev->dev.of_node)
		of_property_read_u32(pdev->dev.of_node,
						"clock-frequency", &clock_freq);

	if (!clock_freq) {
		clk_ipg = devm_clk_get(&pdev->dev, "ipg");
		if (IS_ERR(clk_ipg)) {
			dev_err(&pdev->dev, "no ipg clock defined\n");
			return PTR_ERR(clk_ipg);
		}

		clk_per = devm_clk_get(&pdev->dev, "per");
		if (IS_ERR(clk_per)) {
			dev_err(&pdev->dev, "no per clock defined\n");
			return PTR_ERR(clk_per);
		}
		clock_freq = clk_get_rate(clk_per);
	}

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	irq = platform_get_irq(pdev, 0);
	if (irq <= 0)
		return -ENODEV;

	base = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(base))
		return PTR_ERR(base);

	of_id = of_match_device(flexcan_of_match, &pdev->dev);
	if (of_id) {
		devtype_data = of_id->data;
	} else if (pdev->id_entry->driver_data) {
		devtype_data = (struct flexcan_devtype_data *)
			pdev->id_entry->driver_data;
	} else {
		return -ENODEV;
	}

	dev = alloc_candev(sizeof(struct flexcan_priv), 1);
	if (!dev)
		return -ENOMEM;

	dev->netdev_ops = &flexcan_netdev_ops;
	dev->irq = irq;
	dev->flags |= IFF_ECHO;

	priv = netdev_priv(dev);
	priv->can.clock.freq = clock_freq;
	priv->can.bittiming_const = &flexcan_bittiming_const;
	priv->can.do_set_mode = flexcan_set_mode;
	priv->can.do_get_berr_counter = flexcan_get_berr_counter;
	priv->can.ctrlmode_supported = CAN_CTRLMODE_LOOPBACK |
		CAN_CTRLMODE_LISTENONLY	| CAN_CTRLMODE_3_SAMPLES |
		CAN_CTRLMODE_BERR_REPORTING;
	priv->base = base;
	priv->dev = dev;
	priv->clk_ipg = clk_ipg;
	priv->clk_per = clk_per;
	priv->pdata = dev_get_platdata(&pdev->dev);
	priv->devtype_data = devtype_data;

	priv->reg_xceiver = devm_regulator_get(&pdev->dev, "xceiver");
	if (IS_ERR(priv->reg_xceiver))
		priv->reg_xceiver = NULL;
#ifndef IRQ_CAN_RX
	netif_napi_add(dev, &priv->napi, flexcan_poll, FLEXCAN_NAPI_WEIGHT);
#endif
	platform_set_drvdata(pdev, dev);
	SET_NETDEV_DEV(dev, &pdev->dev);

	err = register_flexcandev(dev);
	if (err) {
		dev_err(&pdev->dev, "registering netdev failed\n");
		goto failed_register;
	}

	devm_can_led_init(dev);

	if (priv->devtype_data->features & FLEXCAN_HAS_V10_FEATURES) {
		err = flexcan_of_parse_stop_mode(pdev);
		if (err) {
			wakeup = 0;
			dev_dbg(&pdev->dev, "failed to parse stop-mode\n");
		}
	}

	device_set_wakeup_capable(&pdev->dev, wakeup);

	dev_info(&pdev->dev, "device registered (reg_base=%p, irq=%d)\n",
		 priv->base, dev->irq);

	return 0;

 failed_register:
	free_candev(dev);
	return err;
}

static int flexcan_remove(struct platform_device *pdev)
{
	struct net_device *dev = platform_get_drvdata(pdev);
	struct flexcan_priv *priv = netdev_priv(dev);

	unregister_flexcandev(dev);
	netif_napi_del(&priv->napi);
	free_candev(dev);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int flexcan_suspend(struct device *device)
{
	struct net_device *dev = dev_get_drvdata(device);
	struct flexcan_priv *priv = netdev_priv(dev);
	int err = 0;

	if (netif_running(dev)) {
		netif_stop_queue(dev);
		netif_device_detach(dev);
		/*
		* if wakeup is enabled, enter stop mode
		* else enter disabled mode.
		*/
		if (device_may_wakeup(device)) {
			enable_irq_wake(dev->irq);
			flexcan_enter_stop_mode(priv);
		} else {
			err = flexcan_chip_disable(priv);
		}
	}
	priv->can.state = CAN_STATE_SLEEPING;

	return err;
}

static int flexcan_resume(struct device *device)
{
	struct net_device *dev = dev_get_drvdata(device);
	struct flexcan_priv *priv = netdev_priv(dev);
	int err = 0;

	priv->can.state = CAN_STATE_ERROR_ACTIVE;
	if (netif_running(dev)) {
		netif_device_attach(dev);
		netif_start_queue(dev);

		if (device_may_wakeup(device)) {
			disable_irq_wake(dev->irq);
			flexcan_exit_stop_mode(priv);
		} else {
			err = flexcan_chip_enable(priv);
		}
	}

	return err;
}
#endif /* CONFIG_PM_SLEEP */

static SIMPLE_DEV_PM_OPS(flexcan_pm_ops, flexcan_suspend, flexcan_resume);

static struct platform_driver flexcan_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.pm = &flexcan_pm_ops,
		.of_match_table = flexcan_of_match,
	},
	.probe = flexcan_probe,
	.remove = flexcan_remove,
	.id_table = flexcan_id_table,
};

module_platform_driver(flexcan_driver);

MODULE_AUTHOR("Sascha Hauer <kernel@pengutronix.de>, "
	      "Marc Kleine-Budde <kernel@pengutronix.de>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("CAN port driver for flexcan based chip");
