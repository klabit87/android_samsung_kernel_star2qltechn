/*
 * Copyright (c) 2012-2015, 2017 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/io.h>

#include "dbm.h"

/**
*  USB DBM Hardware registers.
*
*/
enum dbm_reg {
	DBM_EP_CFG,
	DBM_DATA_FIFO,
	DBM_DATA_FIFO_SIZE,
	DBM_DATA_FIFO_EN,
	DBM_GEVNTADR,
	DBM_GEVNTSIZ,
	DBM_DBG_CNFG,
	DBM_HW_TRB0_EP,
	DBM_HW_TRB1_EP,
	DBM_HW_TRB2_EP,
	DBM_HW_TRB3_EP,
	DBM_PIPE_CFG,
	DBM_SOFT_RESET,
	DBM_GEN_CFG,
	DBM_GEVNTADR_LSB,
	DBM_GEVNTADR_MSB,
	DBM_DATA_FIFO_LSB,
	DBM_DATA_FIFO_MSB,
	DBM_DATA_FIFO_ADDR_EN,
	DBM_DATA_FIFO_SIZE_EN,
};

struct dbm_reg_data {
	u32 offset;
	unsigned int ep_mult;
};

#define DBM_1_4_NUM_EP		4
#define DBM_1_5_NUM_EP		8

struct dbm {
	void __iomem *base;
	const struct dbm_reg_data *reg_table;

	struct device		*dev;
	struct list_head	head;

	int dbm_num_eps;
	u8 ep_num_mapping[DBM_1_5_NUM_EP];
	bool dbm_reset_ep_after_lpm;

	bool is_1p4;
};

static const struct dbm_reg_data dbm_1_4_regtable[] = {
	[DBM_EP_CFG]		= { 0x0000, 0x4 },
	[DBM_DATA_FIFO]		= { 0x0010, 0x4 },
	[DBM_DATA_FIFO_SIZE]	= { 0x0020, 0x4 },
	[DBM_DATA_FIFO_EN]	= { 0x0030, 0x0 },
	[DBM_GEVNTADR]		= { 0x0034, 0x0 },
	[DBM_GEVNTSIZ]		= { 0x0038, 0x0 },
	[DBM_DBG_CNFG]		= { 0x003C, 0x0 },
	[DBM_HW_TRB0_EP]	= { 0x0040, 0x4 },
	[DBM_HW_TRB1_EP]	= { 0x0050, 0x4 },
	[DBM_HW_TRB2_EP]	= { 0x0060, 0x4 },
	[DBM_HW_TRB3_EP]	= { 0x0070, 0x4 },
	[DBM_PIPE_CFG]		= { 0x0080, 0x0 },
	[DBM_SOFT_RESET]	= { 0x0084, 0x0 },
	[DBM_GEN_CFG]		= { 0x0088, 0x0 },
	[DBM_GEVNTADR_LSB]	= { 0x0098, 0x0 },
	[DBM_GEVNTADR_MSB]	= { 0x009C, 0x0 },
	[DBM_DATA_FIFO_LSB]	= { 0x00A0, 0x8 },
	[DBM_DATA_FIFO_MSB]	= { 0x00A4, 0x8 },
};

static const struct dbm_reg_data dbm_1_5_regtable[] = {
	[DBM_EP_CFG]		= { 0x0000, 0x4 },
	[DBM_DATA_FIFO]		= { 0x0280, 0x4 },
	[DBM_DATA_FIFO_SIZE]	= { 0x0080, 0x4 },
	[DBM_DATA_FIFO_EN]	= { 0x026C, 0x0 },
	[DBM_GEVNTADR]		= { 0x0270, 0x0 },
	[DBM_GEVNTSIZ]		= { 0x0268, 0x0 },
	[DBM_DBG_CNFG]		= { 0x0208, 0x0 },
	[DBM_HW_TRB0_EP]	= { 0x0220, 0x4 },
	[DBM_HW_TRB1_EP]	= { 0x0230, 0x4 },
	[DBM_HW_TRB2_EP]	= { 0x0240, 0x4 },
	[DBM_HW_TRB3_EP]	= { 0x0250, 0x4 },
	[DBM_PIPE_CFG]		= { 0x0274, 0x0 },
	[DBM_SOFT_RESET]	= { 0x020C, 0x0 },
	[DBM_GEN_CFG]		= { 0x0210, 0x0 },
	[DBM_GEVNTADR_LSB]	= { 0x0260, 0x0 },
	[DBM_GEVNTADR_MSB]	= { 0x0264, 0x0 },
	[DBM_DATA_FIFO_LSB]	= { 0x0100, 0x8 },
	[DBM_DATA_FIFO_MSB]	= { 0x0104, 0x8 },
	[DBM_DATA_FIFO_ADDR_EN]	= { 0x0200, 0x0 },
	[DBM_DATA_FIFO_SIZE_EN]	= { 0x0204, 0x0 },
};

static LIST_HEAD(dbm_list);

/**
 * Write register masked field with debug info.
 *
 * @dbm - DBM specific data
 * @reg - DBM register, used to look up the offset value
 * @ep - endpoint number
 * @mask - register bitmask.
 * @val - value to write.
 *
 */
static inline void msm_dbm_write_ep_reg_field(struct dbm *dbm,
					      enum dbm_reg reg, int ep,
					      const u32 mask, u32 val)
{
	u32 shift = __ffs(mask);
	u32 offset = dbm->reg_table[reg].offset +
			(dbm->reg_table[reg].ep_mult * ep);
	u32 tmp = ioread32(dbm->base + offset);

	tmp &= ~mask;		/* clear written bits */
	val = tmp | (val << shift);
	iowrite32(val, dbm->base + offset);
}

#define msm_dbm_write_reg_field(d, r, m, v) \
	msm_dbm_write_ep_reg_field(d, r, 0, m, v)

/**
 *
 * Read register with debug info.
 *
 * @dbm - DBM specific data
 * @reg - DBM register, used to look up the offset value
 * @ep - endpoint number
 *
 * @return u32
 */
static inline u32 msm_dbm_read_ep_reg(struct dbm *dbm, enum dbm_reg reg, int ep)
{
	u32 offset = dbm->reg_table[reg].offset +
			(dbm->reg_table[reg].ep_mult * ep);
	return ioread32(dbm->base + offset);
}

#define msm_dbm_read_reg(d, r) msm_dbm_read_ep_reg(d, r, 0)

/**
 *
 * Write register with debug info.
 *
 * @dbm - DBM specific data
 * @reg - DBM register, used to look up the offset value
 * @ep - endpoint number
 *
 */
static inline void msm_dbm_write_ep_reg(struct dbm *dbm, enum dbm_reg reg,
					int ep, u32 val)
{
	u32 offset = dbm->reg_table[reg].offset +
			(dbm->reg_table[reg].ep_mult * ep);
	iowrite32(val, dbm->base + offset);
}

#define msm_dbm_write_reg(d, r, v) msm_dbm_write_ep_reg(d, r, 0, v)

/**
 * Return DBM EP number according to usb endpoint number.
 *
 */
static int find_matching_dbm_ep(struct dbm *dbm, u8 usb_ep)
{
	int i;

	for (i = 0; i < dbm->dbm_num_eps; i++)
		if (dbm->ep_num_mapping[i] == usb_ep)
			return i;

	pr_err("%s: No DBM EP matches USB EP %d", __func__, usb_ep);
	return -ENODEV; /* Not found */
}


/**
 * Reset the DBM registers upon initialization.
 *
 */
int dbm_soft_reset(struct dbm *dbm, bool reset)
{
	if (!dbm) {
		pr_err("%s: dbm pointer is NULL!\n", __func__);
		return -EPERM;
	}

	pr_debug("%s DBM reset\n", (reset ? "Enter" : "Exit"));

	msm_dbm_write_reg_field(dbm, DBM_SOFT_RESET, DBM_SFT_RST_MASK, reset);

	return 0;
}

/**
 * Soft reset specific DBM ep.
 * This function is called by the function driver upon events
 * such as transfer aborting, USB re-enumeration and USB
 * disconnection.
 *
 * @dbm_ep - DBM ep number.
 * @enter_reset - should we enter a reset state or get out of it.
 *
 */
static int ep_soft_reset(struct dbm *dbm, u8 dbm_ep, bool enter_reset)
{
	pr_debug("Setting DBM ep %d reset to %d\n", dbm_ep, enter_reset);

	if (dbm_ep >= dbm->dbm_num_eps) {
		pr_err("Invalid DBM ep index %d\n", dbm_ep);
		return -ENODEV;
	}

	if (enter_reset) {
		msm_dbm_write_reg_field(dbm, DBM_SOFT_RESET,
			DBM_SFT_RST_EPS_MASK & 1 << dbm_ep, 1);
	} else {
		msm_dbm_write_reg_field(dbm, DBM_SOFT_RESET,
			DBM_SFT_RST_EPS_MASK & 1 << dbm_ep, 0);
	}

	return 0;
}


/**
 * Soft reset specific DBM ep (by USB EP number).
 * This function is called by the function driver upon events
 * such as transfer aborting, USB re-enumeration and USB
 * disconnection.
 *
 * The function relies on ep_soft_reset() for checking
 * the legality of the resulting DBM ep number.
 *
 * @usb_ep - USB ep number.
 * @enter_reset - should we enter a reset state or get out of it.
 *
 */
int dbm_ep_soft_reset(struct dbm *dbm, u8 usb_ep, bool enter_reset)
{
	int dbm_ep;

	if (!dbm) {
		pr_err("%s: dbm pointer is NULL!\n", __func__);
		return -EPERM;
	}

	dbm_ep = find_matching_dbm_ep(dbm, usb_ep);

	pr_debug("Setting USB ep %d reset to %d\n", usb_ep, enter_reset);
	return ep_soft_reset(dbm, dbm_ep, enter_reset);
}

/**
 * Configure a USB DBM ep to work in BAM mode.
 *
 *
 * @usb_ep - USB physical EP number.
 * @producer - producer/consumer.
 * @disable_wb - disable write back to system memory.
 * @internal_mem - use internal USB memory for data fifo.
 * @ioc - enable interrupt on completion.
 *
 * @return int - DBM ep number.
 */
int dbm_ep_config(struct dbm *dbm, u8 usb_ep, u8 bam_pipe, bool producer,
		  bool disable_wb, bool internal_mem, bool ioc)
{
	int dbm_ep;
	u32 ep_cfg;

	if (!dbm) {
		pr_err("%s: dbm pointer is NULL!\n", __func__);
		return -EPERM;
	}

	pr_debug("Configuring DBM ep\n");

	dbm_ep = find_matching_dbm_ep(dbm, usb_ep);

	if (dbm_ep < 0) {
		pr_err("usb ep index %d has no corresponding dbm ep\n", usb_ep);
		return -ENODEV;
	}

	/* Due to HW issue, EP 7 can be set as IN EP only */
	if (!dbm->is_1p4 && dbm_ep == 7 && producer) {
		pr_err("last DBM EP can't be OUT EP\n");
		return -ENODEV;
	}

	/* First, reset the dbm endpoint */
	ep_soft_reset(dbm, dbm_ep, 0);

	/* Set ioc bit for dbm_ep if needed */
	msm_dbm_write_reg_field(dbm, DBM_DBG_CNFG,
		DBM_ENABLE_IOC_MASK & 1 << dbm_ep, ioc ? 1 : 0);

	ep_cfg = (producer ? DBM_PRODUCER : 0) |
		(disable_wb ? DBM_DISABLE_WB : 0) |
		(internal_mem ? DBM_INT_RAM_ACC : 0);

	msm_dbm_write_ep_reg_field(dbm, DBM_EP_CFG, dbm_ep,
		DBM_PRODUCER | DBM_DISABLE_WB | DBM_INT_RAM_ACC, ep_cfg >> 8);

	msm_dbm_write_ep_reg_field(dbm, DBM_EP_CFG, dbm_ep, USB3_EPNUM,
		usb_ep);

	if (dbm->is_1p4) {
		msm_dbm_write_ep_reg_field(dbm, DBM_EP_CFG, dbm_ep,
				DBM_BAM_PIPE_NUM, bam_pipe);
		msm_dbm_write_reg_field(dbm, DBM_PIPE_CFG, 0x000000ff, 0xe4);
	}

	msm_dbm_write_ep_reg_field(dbm, DBM_EP_CFG, dbm_ep, DBM_EN_EP, 1);

	return dbm_ep;
}

/**
 * Return number of configured DBM endpoints.
 */
int dbm_get_num_of_eps_configured(struct dbm *dbm)
{
	int i;
	int count = 0;

	if (!dbm) {
		pr_err("%s: dbm pointer is NULL!\n", __func__);
		return -EPERM;
	}

	for (i = 0; i < dbm->dbm_num_eps; i++)
		if (dbm->ep_num_mapping[i])
			count++;

	return count;
}

/**
 * Configure a USB DBM ep to work in normal mode.
 *
 * @usb_ep - USB ep number.
 *
 */
int dbm_ep_unconfig(struct dbm *dbm, u8 usb_ep)
{
	int dbm_ep;
	u32 data;

	if (!dbm) {
		pr_err("%s: dbm pointer is NULL!\n", __func__);
		return -EPERM;
	}

	pr_debug("Unconfiguring DB ep\n");

	dbm_ep = find_matching_dbm_ep(dbm, usb_ep);

	if (dbm_ep < 0) {
		pr_err("usb ep index %d has no corresponding dbm ep\n", usb_ep);
		return -ENODEV;
	}

	dbm->ep_num_mapping[dbm_ep] = 0;

	data = msm_dbm_read_ep_reg(dbm, DBM_EP_CFG, dbm_ep);
	data &= (~0x1);
	msm_dbm_write_ep_reg(dbm, DBM_EP_CFG, dbm_ep, data);

	/* Reset the dbm endpoint */
	ep_soft_reset(dbm, dbm_ep, true);
	/*
	 * The necessary delay between asserting and deasserting the dbm ep
	 * reset is based on the number of active endpoints. If there is more
	 * than one endpoint, a 1 msec delay is required. Otherwise, a shorter
	 * delay will suffice.
	 *
	 * As this function can be called in atomic context, sleeping variants
	 * for delay are not possible - albeit a 1ms delay.
	 */
	if (dbm_get_num_of_eps_configured(dbm) > 1)
		udelay(1000);
	else
		udelay(10);
	ep_soft_reset(dbm, dbm_ep, false);

	return 0;
}

/**
 * Configure the DBM with the USB3 core event buffer.
 * This function is called by the SNPS UDC upon initialization.
 *
 * @addr - address of the event buffer.
 * @size - size of the event buffer.
 *
 */
int dbm_event_buffer_config(struct dbm *dbm, u32 addr_lo, u32 addr_hi, int size)
{
	if (!dbm) {
		pr_err("%s: dbm pointer is NULL!\n", __func__);
		return -EPERM;
	}

	pr_debug("Configuring event buffer\n");

	if (size < 0) {
		pr_err("Invalid size. size = %d", size);
		return -EINVAL;
	}

	/* In case event buffer is already configured, Do nothing. */
	if (msm_dbm_read_reg(dbm, DBM_GEVNTSIZ))
		return 0;

	if (!dbm->is_1p4 || sizeof(phys_addr_t) > sizeof(u32)) {
		msm_dbm_write_reg(dbm, DBM_GEVNTADR_LSB, addr_lo);
		msm_dbm_write_reg(dbm, DBM_GEVNTADR_MSB, addr_hi);
	} else {
		msm_dbm_write_reg(dbm, DBM_GEVNTADR, addr_lo);
	}

	msm_dbm_write_reg_field(dbm, DBM_GEVNTSIZ, DBM_GEVNTSIZ_MASK, size);

	return 0;
}


int dbm_data_fifo_config(struct dbm *dbm, u8 dep_num, phys_addr_t addr,
				u32 size, u8 dst_pipe_idx)
{
	u8 dbm_ep = dst_pipe_idx;
	u32 lo = lower_32_bits(addr);
	u32 hi = upper_32_bits(addr);

	if (!dbm) {
		pr_err("%s: dbm pointer is NULL!\n", __func__);
		return -EPERM;
	}

	dbm->ep_num_mapping[dbm_ep] = dep_num;

	if (!dbm->is_1p4 || sizeof(addr) > sizeof(u32)) {
		msm_dbm_write_ep_reg(dbm, DBM_DATA_FIFO_LSB, dbm_ep, lo);
		msm_dbm_write_ep_reg(dbm, DBM_DATA_FIFO_MSB, dbm_ep, hi);
	} else {
		msm_dbm_write_ep_reg(dbm, DBM_DATA_FIFO, dbm_ep, addr);
	}

	msm_dbm_write_ep_reg_field(dbm, DBM_DATA_FIFO_SIZE, dbm_ep,
		DBM_DATA_FIFO_SIZE_MASK, size);

	return 0;
}

void dbm_set_speed(struct dbm *dbm, bool speed)
{
	if (!dbm) {
		pr_err("%s: dbm pointer is NULL!\n", __func__);
		return;
	}

	msm_dbm_write_reg(dbm, DBM_GEN_CFG, speed);
}

void dbm_enable(struct dbm *dbm)
{
	if (!dbm) {
		pr_err("%s: dbm pointer is NULL!\n", __func__);
		return;
	}

	if (dbm->is_1p4) /* no-op */
		return;

	msm_dbm_write_reg(dbm, DBM_DATA_FIFO_ADDR_EN, 0x000000FF);
	msm_dbm_write_reg(dbm, DBM_DATA_FIFO_SIZE_EN, 0x000000FF);
}

bool dbm_reset_ep_after_lpm(struct dbm *dbm)
{
	if (!dbm) {
		pr_err("%s: dbm pointer is NULL!\n", __func__);
		return false;
	}

	return dbm->dbm_reset_ep_after_lpm;
}

bool dbm_l1_lpm_interrupt(struct dbm *dbm)
{
	if (!dbm) {
		pr_err("%s: dbm pointer is NULL!\n", __func__);
		return false;
	}

	return !dbm->is_1p4;
}

static const struct of_device_id msm_dbm_id_table[] = {
	{ .compatible = "qcom,usb-dbm-1p4", .data = &dbm_1_4_regtable },
	{ .compatible = "qcom,usb-dbm-1p5", .data = &dbm_1_5_regtable },
	{ },
};
MODULE_DEVICE_TABLE(of, msm_dbm_id_table);

static int msm_dbm_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	const struct of_device_id *match;
	struct dbm *dbm;
	struct resource *res;

	dbm = devm_kzalloc(&pdev->dev, sizeof(*dbm), GFP_KERNEL);
	if (!dbm)
		return -ENOMEM;

	match = of_match_node(msm_dbm_id_table, node);
	if (!match) {
		dev_err(&pdev->dev, "Unsupported DBM module\n");
		return -ENODEV;
	}
	dbm->reg_table = match->data;

	if (!strcmp(match->compatible, "qcom,usb-dbm-1p4")) {
		dbm->dbm_num_eps = DBM_1_4_NUM_EP;
		dbm->is_1p4 = true;
	} else {
		dbm->dbm_num_eps = DBM_1_5_NUM_EP;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "missing memory base resource\n");
		return -ENODEV;
	}

	dbm->base = devm_ioremap_nocache(&pdev->dev, res->start,
		resource_size(res));
	if (!dbm->base) {
		dev_err(&pdev->dev, "ioremap failed\n");
		return -ENOMEM;
	}

	dbm->dbm_reset_ep_after_lpm = of_property_read_bool(node,
			"qcom,reset-ep-after-lpm-resume");

	dbm->dev = &pdev->dev;

	platform_set_drvdata(pdev, dbm);

	list_add_tail(&dbm->head, &dbm_list);

	return 0;
}

static struct platform_driver msm_dbm_driver = {
	.probe		= msm_dbm_probe,
	.driver = {
		.name	= "msm-usb-dbm",
		.of_match_table = of_match_ptr(msm_dbm_id_table),
	},
};

module_platform_driver(msm_dbm_driver);

static struct dbm *of_usb_find_dbm(struct device_node *node)
{
	struct dbm  *dbm;

	list_for_each_entry(dbm, &dbm_list, head) {
		if (node != dbm->dev->of_node)
			continue;
		return dbm;
	}
	return ERR_PTR(-ENODEV);
}

struct dbm *usb_get_dbm_by_phandle(struct device *dev, const char *phandle)
{
	struct device_node *node;

	if (!dev->of_node) {
		dev_dbg(dev, "device does not have a device node entry\n");
		return ERR_PTR(-EINVAL);
	}

	node = of_parse_phandle(dev->of_node, phandle, 0);
	if (!node) {
		dev_dbg(dev, "failed to get %s phandle in %s node\n", phandle,
			dev->of_node->full_name);
		return ERR_PTR(-ENODEV);
	}

	return of_usb_find_dbm(node);
}

MODULE_DESCRIPTION("MSM USB DBM driver");
MODULE_LICENSE("GPL v2");
