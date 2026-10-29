/* SPDX-License-Identifier: GPL-2.0-only */
/* EXPERIMENTAL: CIO4 USB4 mode, native J616c 26.6.2 register protocol.
 * Analog/board calibration is supplied through m1n1-populated FDT properties.
 * The PHY's resident firmware manages lane training; host boot is RTKit.
 */
#define CIO4_SRAM_IOVA_BASE 0x10000000ULL

static int t6040_cio4_shmem_setup(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct apple_atcphy *atcphy = cookie;
	u64 offset;

	if (bfr->iova < CIO4_SRAM_IOVA_BASE)
		return -EFAULT;
	offset = bfr->iova - CIO4_SRAM_IOVA_BASE;
	if (offset > resource_size(atcphy->res.cio4_sram) ||
	    bfr->size > resource_size(atcphy->res.cio4_sram) - offset)
		return -EFAULT;
	bfr->iomem = atcphy->regs.cio4_sram + offset;
	bfr->is_mapped = true;
	return 0;
}

static const struct apple_rtkit_ops t6040_cio4_rtkit_ops = {
	.shmem_setup = t6040_cio4_shmem_setup,
};

static const struct t6040_dp_step t6040_cio4_start_steps[] = {
	/* CIO4 USB4 crossbar encoding and common power overrides. */
	{ 0x0006c, 0x0000007f, 0x00000002, false },
	{ 0x00078, 0x0000001e, 0x00000000, false },
	{ 0x0000c, 0x0000002a, 0x00000000, false },
	{ 0x00010, BIT(4), BIT(4), false },
	{ 0x00010, BIT(6), BIT(6), false },
	{ 0x00010, BIT(0), BIT(0), false },
	{ 0x00010, BIT(2), BIT(2), false },
	{ 0x0000c, BIT(10), BIT(10), false },
	{ 0x0000c, BIT(23), BIT(23), true },
	{ 0x0000c, BIT(6), BIT(6), false },
	{ 0x0000c, BIT(24), BIT(24), true },
	{ 0x0000c, BIT(14), 0, false },
	{ 0x00120, BIT(6) | BIT(2) | BIT(10), 0, false },
	{ 0x00010, BIT(5), BIT(5), false },
	{ 0x00010, BIT(7), BIT(7), false },
	{ 0x00010, BIT(1), BIT(1), false },
	{ 0x00010, BIT(3), BIT(3), false },
	{ 0x0000c, BIT(11), BIT(11), false },
	{ 0x0000c, BIT(7), BIT(7), false },
	{ 0x0000c, BIT(15), 0, false },
	{ 0x00120, BIT(7) | BIT(3) | BIT(11), 0, false },
};

static int t6040_cio4_disable(struct apple_atcphy *atcphy)
{
	int ret = 0;

	/* Type-C tears down the router and its DMA children before this call. */
	if (atcphy->cio4_running) {
		ret = apple_rtkit_shutdown(atcphy->cio4_rtk);
		clear32(atcphy->regs.cio4_cpu + 0x40, BIT(0));
		clear32(atcphy->regs.cio4_power, BIT(4));
		atcphy->cio4_running = false;
		apple_rtkit_reinit(atcphy->cio4_rtk);
	}
	atcphy->mode = APPLE_ATCPHY_MODE_OFF;
	return ret;
}

static int t6040_cio4_enable(struct apple_atcphy *atcphy)
{
	int ret;

	if (atcphy->mode == APPLE_ATCPHY_MODE_USB4)
		return 0;
	/* The available reference qualifies normal insertion only for now. */
	if (atcphy->swap_lanes)
		return -EOPNOTSUPP;
	ret = t6040_dp_power_on(atcphy);
	if (ret)
		return ret;
	apple_tunable_apply(atcphy->regs.cio4_fabric, atcphy->tunables.cio4_fabric);
	apple_tunable_apply(atcphy->regs.cio4_common, atcphy->tunables.cio4_common);
	apple_tunable_apply(atcphy->regs.core, atcphy->tunables.cio4_analog);
	ret = t6040_dp_apply(atcphy, t6040_cio4_start_steps,
			     ARRAY_SIZE(t6040_cio4_start_steps));
	if (ret)
		return ret;
	if (!atcphy->cio4_rtk) {
		atcphy->cio4_rtk = devm_apple_rtkit_init(atcphy->dev, atcphy, NULL, 0,
						      &t6040_cio4_rtkit_ops);
		if (IS_ERR(atcphy->cio4_rtk)) {
			ret = PTR_ERR(atcphy->cio4_rtk);
			atcphy->cio4_rtk = NULL;
			return ret;
		}
	}
	set32(atcphy->regs.cio4_cpu + 0x40, BIT(0));
	writel(BIT(4), atcphy->regs.cio4_cpu + 0x28);
	ret = apple_rtkit_boot(atcphy->cio4_rtk);
	if (ret) {
		clear32(atcphy->regs.cio4_cpu + 0x40, BIT(0));
		apple_rtkit_reinit(atcphy->cio4_rtk);
		return dev_err_probe(atcphy->dev, ret, "CIO4 PHY RTKit boot failed\n");
	}
	atcphy->cio4_running = true;
	set32(atcphy->regs.cio4_power, BIT(4));
	atcphy->mode = APPLE_ATCPHY_MODE_USB4;
	dev_info(atcphy->dev, "CIO4 USB4 PHY firmware ready\n");
	return 0;
}

static int t6040_cio4_map(struct platform_device *pdev, struct apple_atcphy *atcphy)
{
	struct resource *res;

	atcphy->regs.cio4_cpu = devm_platform_ioremap_resource_byname(pdev, "coproc");
	if (IS_ERR(atcphy->regs.cio4_cpu))
		return PTR_ERR(atcphy->regs.cio4_cpu);
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "sram");
	atcphy->regs.cio4_sram = devm_ioremap_resource(atcphy->dev, res);
	if (IS_ERR(atcphy->regs.cio4_sram))
		return PTR_ERR(atcphy->regs.cio4_sram);
	atcphy->res.cio4_sram = res;
	atcphy->tunables.cio4_fabric = devm_apple_tunable_parse(atcphy->dev, atcphy->np,
		"apple,tunable-cio4-fabric", platform_get_resource_byname(pdev, IORESOURCE_MEM, "fabric"));
	if (IS_ERR(atcphy->tunables.cio4_fabric))
		return PTR_ERR(atcphy->tunables.cio4_fabric);
	atcphy->tunables.cio4_common = devm_apple_tunable_parse(atcphy->dev, atcphy->np,
		"apple,tunable-cio4-common", platform_get_resource_byname(pdev, IORESOURCE_MEM, "common"));
	if (IS_ERR(atcphy->tunables.cio4_common))
		return PTR_ERR(atcphy->tunables.cio4_common);
	atcphy->tunables.cio4_analog = devm_apple_tunable_parse(atcphy->dev, atcphy->np,
		"apple,tunable-cio4-analog", atcphy->res.core);
	return PTR_ERR_OR_ZERO(atcphy->tunables.cio4_analog);
}
