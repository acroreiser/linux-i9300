/* linux/arch/arm/mach-exynos/cpu.c
 *
 * Copyright (c) 2010-2011 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/sched.h>
#include <linux/sysdev.h>

#include <asm/mach/map.h>
#include <asm/mmu-legacy.h>
#include <asm/mach/irq.h>

#include <asm/proc-fns.h>
#include <asm/exception.h>
#include <asm/hardware/cache-l2x0.h>
#include <linux/irqchip/arm-gic.h>

#include <plat/cpu.h>
#include <plat/clock.h>
#include <plat/devs.h>
#include <plat/exynos4.h>
#include <plat/adc-core.h>
#include <plat/sdhci.h>
#include <plat/fb-core.h>
#include <plat/fimc-core.h>
#include <plat/iic-core.h>
#include <plat/reset.h>
#include <plat/tv-core.h>

#include <mach/regs-irq.h>
#include <mach/regs-pmu.h>

extern int combiner_init(unsigned int combiner_nr, void __iomem *base,
			 unsigned int irq_start);
extern void combiner_cascade_irq(unsigned int combiner_nr, unsigned int irq);

/* Initial IO mappings */
static struct map_desc exynos_iodesc[] __initdata = {
	{
		.virtual	= (unsigned long)S5P_VA_SYSTIMER,
		.pfn		= __phys_to_pfn(EXYNOS_PA_SYSTIMER),
		.length		= SZ_4K,
		.type		= MT_DEVICE,
	}, {
		.virtual	= (unsigned long)S5P_VA_PMU,
		.pfn		= __phys_to_pfn(EXYNOS_PA_PMU),
		.length		= SZ_64K,
		.type		= MT_DEVICE,
	}, {
		.virtual	= (unsigned long)S5P_VA_COMBINER_BASE,
		.pfn		= __phys_to_pfn(EXYNOS_PA_COMBINER),
		.length		= SZ_4K,
		.type		= MT_DEVICE,
	}, {
		.virtual	= (unsigned long)S5P_VA_GIC_CPU,
		.pfn		= __phys_to_pfn(EXYNOS_PA_GIC_CPU),
		.length		= SZ_64K,
		.type		= MT_DEVICE,
	}, {
		.virtual	= (unsigned long)S5P_VA_GIC_DIST,
		.pfn		= __phys_to_pfn(EXYNOS_PA_GIC_DIST),
		.length		= SZ_64K,
		.type		= MT_DEVICE,
	}, {
		.virtual	= (unsigned long)S3C_VA_UART,
		.pfn		= __phys_to_pfn(S3C_PA_UART),
		.length		= SZ_512K,
		.type		= MT_DEVICE,
	},
};

static struct map_desc exynos4_iodesc[] __initdata = {
	{
		.virtual	= (unsigned long)S5P_VA_CMU,
		.pfn		= __phys_to_pfn(EXYNOS4_PA_CMU),
		.length		= SZ_128K,
		.type		= MT_DEVICE,
	}, {
		.virtual	= (unsigned long)S5P_VA_COREPERI_BASE,
		.pfn		= __phys_to_pfn(EXYNOS4_PA_COREPERI),
		.length		= SZ_8K,
		.type		= MT_DEVICE,
	}, {
		.virtual	= (unsigned long)S5P_VA_L2CC,
		.pfn		= __phys_to_pfn(EXYNOS4_PA_L2CC),
		.length		= SZ_4K,
		.type		= MT_DEVICE,
	}, {
		.virtual	= (unsigned long)S5P_VA_GPIO1,
		.pfn		= __phys_to_pfn(EXYNOS4_PA_GPIO1),
		.length		= SZ_4K,
		.type		= MT_DEVICE,
	}, {
		.virtual	= (unsigned long)S5P_VA_GPIO2,
		.pfn		= __phys_to_pfn(EXYNOS4_PA_GPIO2),
		.length		= SZ_4K,
		.type		= MT_DEVICE,
	}, {
		.virtual	= (unsigned long)S5P_VA_GPIO3,
		.pfn		= __phys_to_pfn(EXYNOS4_PA_GPIO3),
		.length		= SZ_256,
		.type		= MT_DEVICE,
	}, {
		.virtual	= (unsigned long)S5P_VA_DMC0,
		.pfn		= __phys_to_pfn(EXYNOS4_PA_DMC0),
		.length		= SZ_4K,
		.type		= MT_DEVICE,
	}, {
		.virtual	= (unsigned long)S3C_VA_USB_HSPHY,
		.pfn		= __phys_to_pfn(EXYNOS4_PA_HSPHY),
		.length		= SZ_4K,
		.type		= MT_DEVICE,
	},
};

static struct map_desc exynos4_iodesc0[] __initdata = {
	{
		.virtual	= (unsigned long)S5P_VA_SYSRAM,
		.pfn		= __phys_to_pfn(EXYNOS4_PA_SYSRAM0),
		.length		= SZ_4K,
		.type		= MT_DEVICE,
	},
};

static struct map_desc exynos4_iodesc1[] __initdata = {
	{
		.virtual	= (unsigned long)S5P_VA_SYSRAM,
		.pfn		= __phys_to_pfn(EXYNOS4_PA_SYSRAM1),
		.length		= SZ_4K,
		.type		= MT_DEVICE,
	},
};

static void exynos_idle(void)
{
	if (!need_resched())
		cpu_do_idle();

	local_irq_enable();
}

static void exynos4_sw_reset(void)
{
	__raw_writel(0x1, EXYNOS_SWRESET);
}

/*
 * exynos_map_io
 *
 * register the standard cpu IO areas
 */
void __init exynos4_map_io(void)
{
	iotable_init_legacy(exynos_iodesc, ARRAY_SIZE(exynos_iodesc));
	iotable_init_legacy(exynos4_iodesc, ARRAY_SIZE(exynos4_iodesc));

	if (soc_is_exynos4210() && samsung_rev() == EXYNOS4210_REV_0)
		iotable_init_legacy(exynos4_iodesc0, ARRAY_SIZE(exynos4_iodesc0));
	else
		iotable_init_legacy(exynos4_iodesc1, ARRAY_SIZE(exynos4_iodesc1));

	/* initialize device information early */
	exynos4_default_sdhci0();
	exynos4_default_sdhci1();
	exynos4_default_sdhci2();
	exynos4_default_sdhci3();

	s3c_adc_setname("samsung-adc-v3");

	s3c_fimc_setname(0, "exynos4-fimc");
	s3c_fimc_setname(1, "exynos4-fimc");
	s3c_fimc_setname(2, "exynos4-fimc");
	s3c_fimc_setname(3, "exynos4-fimc");

	/* The I2C bus controllers are directly compatible with s3c2440 */
	s3c_i2c0_setname("s3c2440-i2c");
	s3c_i2c1_setname("s3c2440-i2c");
	s3c_i2c2_setname("s3c2440-i2c");

	s5p_fb_setname(0, "exynos4-fb");
	s5p_hdmi_setname("exynos4-hdmi");
}

void __init exynos4_init_clocks(int xtal)
{
	printk(KERN_DEBUG "%s: initializing clocks\n", __func__);

	s3c24xx_register_baseclocks(xtal);
	s5p_register_clocks(xtal);

	if (soc_is_exynos4210())
		exynos4210_register_clocks();
	else if (soc_is_exynos4212() || soc_is_exynos4412())
		exynos4212_register_clocks();

	exynos4_register_clocks();
	exynos4_setup_clocks();
}

static void exynos4_gic_irq_fix_base(struct irq_data *d)
{
	struct gic_chip_data *gic_data = irq_data_get_irq_chip_data(d);

	gic_data->cpu_base = S5P_VA_GIC_CPU +
			    (gic_bank_offset * smp_processor_id());

	gic_data->dist_base = S5P_VA_GIC_DIST +
			    (gic_bank_offset * smp_processor_id());
}

void __init exynos4_init_irq(void)
{
	int irq;
	unsigned int bank_offset;

	gic_bank_offset = soc_is_exynos4412() ? 0x4000 : 0x8000;

	gic_init(0, IRQ_PPI(0), S5P_VA_GIC_DIST, S5P_VA_GIC_CPU);
	gic_arch_extn.irq_eoi = exynos4_gic_irq_fix_base;
	gic_arch_extn.irq_unmask = exynos4_gic_irq_fix_base;
	gic_arch_extn.irq_mask = exynos4_gic_irq_fix_base;

	for (irq = 0; irq < EXYNOS4_MAX_COMBINER_NR; irq++) {

		combiner_init(irq, (void __iomem *)S5P_VA_COMBINER(irq),
				COMBINER_IRQ(irq, 0));
		combiner_cascade_irq(irq, IRQ_SPI(irq));
	}

	/* The parameters of s5p_init_irq() are for VIC init.
	 * Theses parameters should be NULL and 0 because EXYNOS4
	 * uses GIC instead of VIC.
	 */
	s5p_init_irq(NULL, 0);
}

struct sysdev_class exynos4_sysclass = {
	.name	= "exynos4-core",
};

static struct sys_device exynos4_sysdev = {
	.cls	= &exynos4_sysclass,
};

static int __init exynos4_core_init(void)
{
	return sysdev_class_register(&exynos4_sysclass);
}
core_initcall(exynos4_core_init);

#ifdef CONFIG_CACHE_L2X0
static int __init exynos4_l2x0_cache_init(void)
{
	/* TAG, Data Latency Control: 2cycle */
	__raw_writel(0x110, S5P_VA_L2CC + L2X0_TAG_LATENCY_CTRL);

	if (soc_is_exynos4210())
		__raw_writel(0x110, S5P_VA_L2CC + L2X0_DATA_LATENCY_CTRL);
	else if (soc_is_exynos4212() || soc_is_exynos4412())
		__raw_writel(0x120, S5P_VA_L2CC + L2X0_DATA_LATENCY_CTRL);

	/* L2X0 Prefetch Control */
	__raw_writel(0x30000007, S5P_VA_L2CC + L2X0_PREFETCH_CTRL);

	/* L2X0 Power Control */
	__raw_writel(L2X0_DYNAMIC_CLK_GATING_EN | L2X0_STNDBY_MODE_EN,
		     S5P_VA_L2CC + L2X0_POWER_CTRL);

	l2x0_init(S5P_VA_L2CC, 0x7C470001, 0xC200ffff);

	return 0;
}

early_initcall(exynos4_l2x0_cache_init);
#endif

int __init exynos_init(void)
{
	printk(KERN_INFO "EXYNOS: Initializing architecture\n");

	/* set idle function */
	arm_pm_idle = exynos_idle;

	/* set sw_reset function */
	if (soc_is_exynos4210() || soc_is_exynos4212() || soc_is_exynos4412())
		s5p_reset_hook = exynos4_sw_reset;

	return sysdev_register(&exynos4_sysdev);
}
