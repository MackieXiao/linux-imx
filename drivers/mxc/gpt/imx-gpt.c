/*
 * Copyright (C) 2023 Freescale Semiconductor, Inc. All Rights Reserved.
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/clockchips.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/sched_clock.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <soc/imx/timer.h>

#define DRIVER_NAME "imx-gpt"

/* V2 register offset */
#define V2_REG_CR       0x00
#define V2_REG_PR       0x04
#define V2_REG_SR       0x08
#define V2_REG_IR       0x0c
#define V2_REG_OCR1     0x10
#define V2_REG_OCR2     0x14
#define V2_REG_OCR3     0x18
#define V2_REG_ICR1     0x1c
#define V2_REG_ICR2     0x20
#define V2_REG_CNT      0x24

/* V2 register bit mask */
#define V2_CR_EN                        (1 << 0)
#define V2_CR_ENMOD                     (1 << 1)
#define V2_CR_DBGEN                     (1 << 2)
#define V2_CR_WAITEN                    (1 << 3)
#define V2_CR_DOZEEN                    (1 << 4)
#define V2_CR_STOPEN                    (1 << 5)
#define V2_CR_CLKSRC_NO_CLOCK           (0 << 6)
#define V2_CR_CLKSRC_IPG_CLK            (1 << 6)
#define V2_CR_CLKSRC_IPG_CLK_HIGHFREQ   (2 << 6)
#define V2_CR_CLKSRC_EXT_CLK            (3 << 6)
#define V2_CR_CLKSRC_IPG_CLK_32K        (4 << 6)
#define V2_CR_CLKSRC_IPG_CLK_24M        (5 << 6)
#define V2_CR_FRR                       (1 << 9)
#define V2_CR_EN_24M                    (1 << 10)
#define V2_CR_SWR                       (1 << 15)
#define V2_IM_MODE_DISABLED             (0)
#define V2_IM_MODE_RISING_EDGE          (1)
#define V2_IM_MODE_FALLING_EDGE         (2)
#define V2_IM_MODE_BOTH                 (3)
#define V2_IM1_MODE_OFFSET              (16)
#define V2_IM2_MODE_OFFSET              (18)
#define V2_OM_MODE_DISCONNECTED         (0)
#define V2_OM_MODE_TOGGLE               (1)
#define V2_OM_MODE_CLEAR                (2)
#define V2_OM_MODE_SET                  (3)
#define V2_OM_MODE_LOW_PULSE            (4)
#define V2_OM1_MODE_OFFSET              (20)
#define V2_OM2_MODE_OFFSET              (23)
#define V2_OM3_MODE_OFFSET              (26)
#define V2_CR_FO1                       (1 << 29)
#define V2_CR_FO2                       (1 << 30)
#define V2_CR_FO3                       (1 << 31)

#define V2_PR_PRE24M_DIV_1      (0 << 12)
#define V2_PR_PRE24M_DIV_2      (1 << 12)
#define V2_PR_PRE24M_DIV_3      (2 << 12)
#define V2_PR_PRE24M_DIV_4      (3 << 12)
#define V2_PR_PRE24M_DIV_5      (4 << 12)
#define V2_PR_PRE24M_DIV_6      (5 << 12)
#define V2_PR_PRE24M_DIV_7      (6 << 12)
#define V2_PR_PRE24M_DIV_8      (7 << 12)
#define V2_PR_PRE24M_DIV_9      (8 << 12)
#define V2_PR_PRE24M_DIV_10     (9 << 12)
#define V2_PR_PRE24M_DIV_11     (10 << 12)
#define V2_PR_PRE24M_DIV_12     (11 << 12)
#define V2_PR_PRE24M_DIV_13     (12 << 12)
#define V2_PR_PRE24M_DIV_14     (13 << 12)
#define V2_PR_PRE24M_DIV_15     (14 << 12)
#define V2_PR_PRE24M_DIV_16     (15 << 12)

#define V2_SR_OF1       (1 << 0)
#define V2_SR_OF2       (1 << 1)
#define V2_SR_OF3       (1 << 2)
#define V2_SR_IF1       (1 << 3)
#define V2_SR_IF2       (1 << 4)
#define V2_SR_ROV       (1 << 5)

#define V2_IR_OF1IE             (1 << 0)
#define V2_IR_OF2IE             (1 << 1)
#define V2_IR_OF3IE             (1 << 2)
#define V2_IR_IF1IE             (1 << 3)
#define V2_IR_IF2IE             (1 << 4)
#define V2_IR_ROVIE             (1 << 5)

#define V2_OM_NUM       3
#define V2_IM_NUM       2

#define V2_TIMER_RATE_OSC_DIV8  3000000

static DEFINE_SPINLOCK(icap_lock);

struct imx_timer {
    int irq;
    struct clk *clk_per;
    struct clk *clk_ipg;
    struct device_node *np;
    void __iomem *base;
    const struct imx_gpt_data *gpt;
    enum imx_gpt_type type;
};

struct imx_gpt_data {
    void (*gpt_setup_tctl)(struct imx_timer *imxtm);
    void (*gpt_enable)(struct imx_timer *imxtm);
    void (*gpt_disable)(struct imx_timer *imxtm);
    void (*gpt_irq_acknowledge)(struct imx_timer *imxtm);
    bool (*gpt_is_rollover)(struct imx_timer *imxtm);

    void (*gpt_oc_setup)(struct imx_timer *imxtm, u32 chan, u32 oc_val);
    void (*gpt_oc_irq_enable)(struct imx_timer *imxtm, u32 chan);
    void (*gpt_oc_irq_disable)(struct imx_timer *imxtm, u32 chan);
    void (*gpt_oc_irq_acknowledge)(struct imx_timer *imxtm, u32 chan);
    bool (*gpt_is_oc_irq)(struct imx_timer *imxtm, u32 chan);

    void (*gpt_ic_setup)(struct imx_timer *imxtm, u32 chan, u32 mode);
    void (*gpt_ic_irq_enable)(struct imx_timer *imxtm, u32 chan);
    void (*gpt_ic_irq_disable)(struct imx_timer *imxtm, u32 chan);
    void (*gpt_ic_irq_acknowledge)(struct imx_timer *imxtm, u32 chan);
    bool (*gpt_is_ic_irq)(struct imx_timer *imxtm, u32 chan);
};

void v2_gpt_setup_tctl(struct imx_timer *imxtm)
{
    u32 cr_val, pr_val;

    cr_val = 0;
    pr_val = 0;

    /*
	 * Initialise to a known state (all timers off, and timing reset)
	 */

	writel_relaxed(cr_val, imxtm->base + V2_REG_CR);
	writel_relaxed(pr_val, imxtm->base + V2_REG_PR); /* see datasheet note */

    cr_val = V2_CR_WAITEN;
    if (clk_get_rate(imxtm->clk_per) == V2_TIMER_RATE_OSC_DIV8) {
        cr_val |= V2_CR_EN_24M;
        cr_val |= V2_CR_CLKSRC_IPG_CLK_24M;
		/* 24 / 8 = 3 MHz */
        pr_val = V2_PR_PRE24M_DIV_8;
    } else {
        cr_val |= V2_CR_EN_24M;
        cr_val |= V2_CR_CLKSRC_IPG_CLK_24M;
        /* 24 / 3 = 8 MHz */
        pr_val = V2_PR_PRE24M_DIV_3;
    }

    writel_relaxed(cr_val, imxtm->base + V2_REG_CR);
	writel_relaxed(pr_val, imxtm->base + V2_REG_PR);
}

void v2_gpt_enable(struct imx_timer *imxtm)
{
    u32 cr_val;

    cr_val = readl_relaxed(imxtm->base + V2_REG_CR);
    cr_val |= V2_CR_EN;
    writel_relaxed(cr_val, imxtm->base + V2_REG_CR);
}

void v2_gpt_disable(struct imx_timer *imxtm)
{
    u32 cr_val;

    cr_val = readl_relaxed(imxtm->base + V2_REG_CR);
    cr_val &= !(u32)V2_CR_EN;
    writel_relaxed(cr_val, imxtm->base + V2_REG_CR);
}

void v2_gpt_irq_acknowledge(struct imx_timer *imxtm)
{
    writel_relaxed(V2_SR_ROV, imxtm->base + V2_REG_SR);
}

bool v2_gpt_is_rollover(struct imx_timer *imxtm)
{
    u32 sr_val;

    sr_val = readl_relaxed(imxtm->base + V2_REG_SR);
    return sr_val & V2_SR_ROV ? true : false;
}

void v2_gpt_oc_setup(struct imx_timer *imxtm, u32 chan, u32 oc_val)
{
    u32 ocr[V2_OM_NUM] = {V2_REG_OCR1, V2_REG_OCR2, V2_REG_OCR3};

    if (chan > V2_OM_NUM || chan == 0)
        return;

    writel_relaxed(oc_val, imxtm->base + ocr[chan - 1]);
}

void v2_gpt_oc_irq_enable(struct imx_timer *imxtm, u32 chan)
{
    unsigned long flags;
    u32 mask[V2_OM_NUM] = {V2_IR_OF1IE, V2_IR_OF2IE, V2_IR_OF3IE};
    u32 ir_val;

    if (chan > V2_OM_NUM || chan == 0)
        return;

    spin_lock_irqsave(&icap_lock, flags);
    ir_val = readl_relaxed(imxtm->base + V2_REG_IR);
    ir_val |= mask[chan - 1];
    writel_relaxed(ir_val, imxtm->base + V2_REG_IR);
    spin_unlock_irqrestore(&icap_lock, flags);
}

void v2_gpt_oc_irq_disable(struct imx_timer *imxtm, u32 chan)
{
    unsigned long flags;
    u32 mask[V2_OM_NUM] = { !(u32)V2_IR_OF1IE, !(u32)V2_IR_OF2IE,
                            !(u32)V2_IR_OF3IE};
    u32 ir_val;

    if (chan > 3 || chan == 0)
        return;

    spin_lock_irqsave(&icap_lock, flags);
    ir_val = readl_relaxed(imxtm->base + V2_REG_IR);
    ir_val &= mask[chan - 1];
    writel_relaxed(ir_val, imxtm->base + V2_REG_IR);
    spin_unlock_irqrestore(&icap_lock, flags);
}

void v2_gpt_oc_irq_acknowledge(struct imx_timer *imxtm, u32 chan)
{
    u32 mask[V2_OM_NUM] = {V2_SR_OF1, V2_SR_OF2, V2_SR_OF3};

    if (chan > V2_IM_NUM || chan == 0)
        return;

    writel_relaxed(mask[chan - 1], imxtm->base + V2_REG_SR);
}

bool v2_gpt_is_oc_irq(struct imx_timer *imxtm, u32 chan)
{
    u32 mask[V2_OM_NUM] = {V2_SR_OF1, V2_SR_OF2, V2_SR_OF3};
    u32 sr_val;

    if (chan > V2_OM_NUM || chan == 0)
        return false;

    sr_val = readl_relaxed(imxtm->base + V2_REG_SR);
    return sr_val & mask[chan - 1] ? true : false;
}

void v2_gpt_ic_setup(struct imx_timer *imxtm, u32 chan, u32 mode)
{
    u32 offset[V2_IM_NUM] = {V2_IM1_MODE_OFFSET, V2_IM2_MODE_OFFSET};
    u32 cr_val;

    if (chan > V2_IM_NUM || chan == 0 || mode > V2_IM_MODE_BOTH)
        return;

    cr_val = readl_relaxed(imxtm->base + V2_REG_CR);
    cr_val |= mode << offset[chan - 1];
    writel_relaxed(cr_val, imxtm->base + V2_REG_CR);
}

void v2_gpt_ic_irq_enable(struct imx_timer *imxtm, u32 chan)
{
    unsigned long flags;
    u32 ir_mask[V2_IM_NUM] = {V2_IR_IF1IE, V2_IR_IF2IE};
    u32 ir_val;

    if (chan > V2_IM_NUM || chan == 0)
        return;

    spin_lock_irqsave(&icap_lock, flags);
    ir_val = readl_relaxed(imxtm->base + V2_REG_IR);
    ir_val |= ir_mask[chan - 1];
    writel_relaxed(ir_val, imxtm->base + V2_REG_IR);
    spin_unlock_irqrestore(&icap_lock, flags);
}

void v2_gpt_ic_irq_disable(struct imx_timer *imxtm, u32 chan)
{
    unsigned long flags;
    u32 ir_mask[V2_IM_NUM] = {!(u32)V2_IR_IF1IE, !(u32)V2_IR_IF2IE};
    u32 ir_val;

    if (chan > V2_IM_NUM|| chan == 0)
        return;

    spin_lock_irqsave(&icap_lock, flags);
    ir_val = readl_relaxed(imxtm->base + V2_REG_IR);
    ir_val &= ir_mask[chan - 1];
    writel_relaxed(ir_val, imxtm->base + V2_REG_IR);
    spin_unlock_irqrestore(&icap_lock, flags);
}

void v2_gpt_ic_irq_acknowledge(struct imx_timer *imxtm, u32 chan)
{
    u32 ir_mask[V2_IM_NUM] = {V2_IR_IF1IE, V2_IR_IF2IE};

    if (chan > V2_IM_NUM || chan == 0)
        return;

    writel_relaxed(ir_mask[chan - 1], imxtm->base + V2_REG_SR);
}

bool v2_gpt_is_ic_irq(struct imx_timer *imxtm, u32 chan)
{
    u32 mask[V2_IM_NUM] = {V2_IR_IF1IE, V2_IR_IF2IE};
    u32 sr_val;

    if (chan > V2_IM_NUM || chan == 0)
        return false;

    sr_val = readl_relaxed(imxtm->base + V2_REG_SR);
    return sr_val & mask[chan - 1] ? true : false;
}

static const struct imx_gpt_data v2_gpt_data = {
    .gpt_setup_tctl = v2_gpt_setup_tctl,
    .gpt_enable = v2_gpt_enable,
    .gpt_disable = v2_gpt_disable,
    .gpt_irq_acknowledge = v2_gpt_irq_acknowledge,
    .gpt_is_rollover = v2_gpt_is_rollover,
    .gpt_oc_setup = v2_gpt_oc_setup,
    .gpt_oc_irq_enable = v2_gpt_oc_irq_enable,
    .gpt_oc_irq_disable = v2_gpt_oc_irq_disable,
    .gpt_oc_irq_acknowledge = v2_gpt_oc_irq_acknowledge,
    .gpt_ic_setup = v2_gpt_ic_setup,
    .gpt_is_ic_irq = v2_gpt_is_oc_irq,
    .gpt_ic_irq_enable = v2_gpt_ic_irq_enable,
    .gpt_ic_irq_disable = v2_gpt_ic_irq_disable,
    .gpt_ic_irq_acknowledge = v2_gpt_ic_irq_acknowledge,
    .gpt_is_ic_irq = v2_gpt_is_ic_irq,
};

int gpt_setup(struct imx_timer *imxtm, struct platform_device *pdev)
{
    const struct imx_gpt_data *gpt_data = of_device_get_match_data(&pdev->dev);

    gpt_data->gpt_setup_tctl(imxtm);
    gpt_data->gpt_oc_setup(imxtm, 1, 0xFFFFFFFF);
    gpt_data->gpt_ic_setup(imxtm, 1, V2_IM_MODE_BOTH);
    gpt_data->gpt_ic_irq_enable(imxtm, 1);
    gpt_data->gpt_enable(imxtm);

    return 0;
}

static int gpt_probe(struct platform_device *pdev)
{
    static int initialized = 0;
    struct imx_timer *imxtm;
    int ret;

    if (initialized)
        return 0;

    imxtm = kzalloc(sizeof(*imxtm), GFP_KERNEL);
    if (!imxtm)
        return -ENOMEM;

    imxtm->np = pdev->dev.of_node;
    imxtm->base = of_iomap(imxtm->np, 0);
    if (!imxtm->base) {
        kfree(imxtm);
        return -ENXIO;
    }

    imxtm->irq = irq_of_parse_and_map(imxtm->np, 0);
    if (imxtm->irq <= 0) {
        kfree(imxtm);
        return -EINVAL;
    }

    imxtm->clk_ipg = of_clk_get_by_name(imxtm->np, "ipg");

    /* Try osc_per first, and fall back to per otherwise */
    imxtm->clk_per = of_clk_get_by_name(imxtm->np, "osc_per");
    if (IS_ERR(imxtm->clk_per))
        imxtm->clk_per = of_clk_get_by_name(imxtm->np, "per");

    if (IS_ERR(imxtm->clk_per) || IS_ERR(imxtm->clk_ipg)) {
        pr_err("i.MX timer: unable to get clk\n");
        return PTR_ERR(imxtm->clk_per);
    }

    clk_prepare_enable(imxtm->clk_ipg);
    clk_prepare_enable(imxtm->clk_per);

    ret = gpt_setup(imxtm, pdev);
    if (ret)
        return ret;

    initialized = 1;

    return 0;
}

static int gpt_remove(struct platform_device *dev)
{
    return 0;
}


static const struct of_device_id gpt_of_match[] = {
    {.compatible = "fsl,imx8mn-gpt", .data = &v2_gpt_data},
    {.compatible = "fsl,imx8mm-gpt", .data = &v2_gpt_data},
    {.compatible = "fsl,imx8mp-gpt", .data = &v2_gpt_data},
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, gpt_of_match);

static struct platform_driver gpt_driver = {
    .driver = {
        .name   = DRIVER_NAME,
        .of_match_table = gpt_of_match,
    },
    .probe = gpt_probe,
    .remove = gpt_remove,
};
module_platform_driver(gpt_driver);

#if 0
static int __init gpt_driver_init(void)
{
    printk("Mackie: %s\n", __PRETTY_FUNCTION__);
    return platform_driver_register(&gpt_driver);
}

static void __exit gpt_driver_exit(void)
{
    printk("Mackie: %s\n", __PRETTY_FUNCTION__);
    platform_driver_unregister(&gpt_driver);
}

module_init(gpt_driver_init);
module_exit(gpt_driver_exit);
#endif

MODULE_DESCRIPTION("i.MX GPT driver");
MODULE_AUTHOR("Mackie Xiao");
MODULE_LICENSE("GPL");
