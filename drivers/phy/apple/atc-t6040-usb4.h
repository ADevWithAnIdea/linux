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

/* power-domains order is part of the CIO4 binding. */
#define CIO4_PD_PHYMXWRAP 2
#define CIO4_CONTROL_EP 0x20
#define CIO4_CONTROL_DRAIN 0x0100000000000000ULL
#define CIO4_CONTROL_DRAIN_ACK 0x8000010000000000ULL
#define CIO4_CONTROL_DRAIN_ACK_MASK GENMASK_ULL(63, 32)

static bool t6040_cio4_control_rx(void *cookie, u8 endpoint, u64 message)
{
	struct apple_atcphy *atcphy = cookie;

	if (endpoint != CIO4_CONTROL_EP ||
	    (message & CIO4_CONTROL_DRAIN_ACK_MASK) != CIO4_CONTROL_DRAIN_ACK)
		return false;
	/* Native repeated hotplug replies carry 1, 2, 3, 4 in the low word.
	 * This is a running drain count, not a success/error result.
	 */
	dev_dbg(atcphy->dev, "CIO4 drain acknowledged, count %u\n", (u32)message);
	complete(&atcphy->cio4_drain_completion);
	return true;
}

static const struct apple_rtkit_ops t6040_cio4_rtkit_ops = {
	.shmem_setup = t6040_cio4_shmem_setup,
	.recv_message_early = t6040_cio4_control_rx,
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

static const struct t6040_dp_step t6040_cio4_stop_steps[] = {
	{ 0x0000c, BIT(14), BIT(14), false },
	{ 0x0000c, BIT(6), 0, false },
	{ 0x0000c, BIT(24), 0, true },
	{ 0x0000c, BIT(10), 0, false },
	{ 0x00010, BIT(0), 0, false },
	{ 0x00010, BIT(4), 0, false },
	{ 0x0000c, BIT(23), 0, true },
	{ 0x0000c, BIT(15), BIT(15), false },
	{ 0x0000c, BIT(7), 0, false },
	{ 0x0000c, BIT(11), 0, false },
	{ 0x00010, BIT(1), 0, false },
	{ 0x00010, BIT(5), 0, false },
};

static int t6040_cio4_domain_on(struct apple_atcphy *atcphy, unsigned int i)
{
	struct dev_pm_domain_list *list = atcphy->cio4_pd_list;
	struct device_link *link;

	link = device_link_add(atcphy->dev, list->pd_devs[i],
		DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME | DL_FLAG_RPM_ACTIVE);
	if (!link)
		return -ENODEV;
	list->pd_links[i] = link;
	return 0;
}

static void t6040_cio4_domain_off(struct apple_atcphy *atcphy, unsigned int i)
{
	struct device_link **link = &atcphy->cio4_pd_list->pd_links[i];

	if (*link)
		device_link_del(*link);
	*link = NULL;
}

static int t6040_cio4_power_off(struct apple_atcphy *atcphy)
{
	struct dev_pm_domain_list *list = atcphy->cio4_pd_list;
	void __iomem *power = atcphy->regs.cio4_power;
	struct device *mxwrap = list->pd_devs[CIO4_PD_PHYMXWRAP];
	bool suspended;
	u32 value;
	int ret;

	if (!atcphy->cio4_powered)
		return 0;
	/* Native order: stop the firmware, remove PHYMXWRAP, then gate analog. */
	if (list->pd_links[CIO4_PD_PHYMXWRAP]) {
		writel(~(u32)BIT(4), atcphy->regs.cio4_cpu + 0x28);
		t6040_cio4_domain_off(atcphy, CIO4_PD_PHYMXWRAP);
		pm_runtime_barrier(mxwrap);
		ret = read_poll_timeout(pm_runtime_suspended, suspended, suspended,
			1000, 500000, false, mxwrap);
		if (ret)
			return dev_err_probe(atcphy->dev, ret, "PHYMXWRAP failed to power down\n");
	}
	set32(power + 0x28, BIT(0));
	clear32(power, BIT(4));
	clear32(power, BIT(3));
	set32(power, BIT(2));
	clear32(power, BIT(1));
	ret = readl_poll_timeout(power + 4, value, !(value & BIT(1)), 1, 10000);
	if (ret)
		return ret;
	clear32(power, BIT(0));
	ret = readl_poll_timeout(power + 4, value, !(value & BIT(0)), 1, 10000);
	if (ret)
		return ret;
	writel(0, power + 8);
	for (int i = list->num_pds - 1; i >= 0; i--)
		t6040_cio4_domain_off(atcphy, i);
	atcphy->cio4_powered = false;
	return 0;
}

static int t6040_cio4_power_on(struct apple_atcphy *atcphy)
{
	void __iomem *power = atcphy->regs.cio4_power;
	u32 value;
	int ret;

	if (atcphy->cio4_powered)
		return -EBUSY;
	/* Hold the common/APB supplies while PHYMXWRAP is still reset. */
	for (unsigned int i = 0; i < atcphy->cio4_pd_list->num_pds; i++) {
		if (i == CIO4_PD_PHYMXWRAP)
			continue;
		ret = t6040_cio4_domain_on(atcphy, i);
		if (ret)
			goto err_domains;
	}
	atcphy->cio4_powered = true;
	set32(power, BIT(0));
	ret = readl_poll_timeout(power + 4, value, value & BIT(0), 1, 10000);
	if (ret)
		goto err_power;
	set32(power, BIT(1));
	ret = readl_poll_timeout(power + 4, value, value & BIT(1), 1, 10000);
	if (ret)
		goto err_power;
	clear32(power, BIT(2));
	ret = t6040_cio4_domain_on(atcphy, CIO4_PD_PHYMXWRAP);
	if (ret)
		goto err_power;
	clear32(power + 0x28, BIT(0));
	set32(power, BIT(3));
	apple_tunable_apply(atcphy->regs.cio4_fabric, atcphy->tunables.cio4_fabric);
	apple_tunable_apply(atcphy->regs.cio4_common, atcphy->tunables.cio4_common);
	return 0;

err_power:
	t6040_cio4_power_off(atcphy);
	return ret;
err_domains:
	for (int i = atcphy->cio4_pd_list->num_pds - 1; i >= 0; i--)
		t6040_cio4_domain_off(atcphy, i);
	return ret;
}

static int t6040_cio4_disable(struct apple_atcphy *atcphy)
{
	int ret;

	/* Type-C tears down the router and its DMA children before this call. */
	if (atcphy->cio4_running) {
		ret = t6040_dp_apply(atcphy, t6040_cio4_stop_steps,
			ARRAY_SIZE(t6040_cio4_stop_steps));
		if (ret)
			return ret;
		reinit_completion(&atcphy->cio4_drain_completion);
		ret = apple_rtkit_send_message(atcphy->cio4_rtk, CIO4_CONTROL_EP,
			CIO4_CONTROL_DRAIN, NULL, false);
		if (ret)
			return ret;
		/* Native AP/IOP state is 0x201, followed by the control-EP ack. */
		ret = apple_rtkit_idle(atcphy->cio4_rtk);
		if (ret)
			return ret;
		if (!wait_for_completion_timeout(&atcphy->cio4_drain_completion,
			msecs_to_jiffies(500)))
			return -ETIMEDOUT;
		apple_rtkit_free(atcphy->cio4_rtk);
		atcphy->cio4_rtk = NULL;
		atcphy->cio4_running = false;
	}
	atcphy->mode = APPLE_ATCPHY_MODE_OFF;
	return t6040_cio4_power_off(atcphy);
}

/* Native right-port USB3 attachment: PMA0. Board calibration above is
 * supplied by m1n1. These are protocol controls, not calibration constants.
 * Crossbar masks are experimental and cover the observed mode transitions.
 */
static const struct t6040_dp_step t6040_usb3_start_steps[] = {
	{ 0x00800, BIT(9), 0, false },
	{ 0x00818, 0x3000, 0x3000, false },
	{ 0x03888, 0xc, 0x4, false },
	{ 0x28004, BIT(3), BIT(3), false },
	{ 0x03adc, 0x1d0, 0x140, false },
	{ 0x0e0cc, 0x380000, 0x380000, false },
	{ 0x1e0cc, 0x380000, 0x380000, false },
	{ 0x0d090, 0x380000, 0x380000, false },
	{ 0x1d090, 0x380000, 0x380000, false },
	{ 0x0006c, 0x0300006c, 0x01000044, false },
	{ 0x00078, 0x0002233e, 0x00000110, false },
	{ 0x0000c, 0x2a, 0, false },
	{ 0x00010, BIT(0), 0, false },
	{ 0x00010, BIT(2), 0, false },
	{ 0x0000c, BIT(16), 0, false },
	{ 0x0000c, BIT(8), 0, false },
	{ 0x0000c, BIT(12), 0, false },
	{ 0x00120, BIT(6), 0, false },
	{ 0x00120, BIT(2), 0, false },
	{ 0x00120, BIT(10), 0, false },
};

static int t6040_cio4_start(struct apple_atcphy *atcphy, enum atcphy_mode mode)
{
	int ret;

	if (atcphy->cio4_running && atcphy->mode == mode)
		return apple_rtkit_is_running(atcphy->cio4_rtk) ? 0 : -EIO;
	ret = t6040_cio4_power_on(atcphy);
	if (ret)
		return ret;
	apple_tunable_apply(atcphy->regs.core, atcphy->tunables.cio4_analog);
	if (mode == APPLE_ATCPHY_MODE_USB3)
		ret = t6040_dp_apply(atcphy, t6040_usb3_start_steps,
				     ARRAY_SIZE(t6040_usb3_start_steps));
	else
		ret = t6040_dp_apply(atcphy, t6040_cio4_start_steps,
				     ARRAY_SIZE(t6040_cio4_start_steps));
	if (ret)
		goto err_power;
	/* Native capture: core+0x78 bit0 changes with plug orientation. */
	core_mask32(atcphy, 0x78, BIT(0), atcphy->swap_lanes ? BIT(0) : 0);
	atcphy->cio4_rtk = apple_rtkit_init(atcphy->dev, atcphy, NULL, 0,
		&t6040_cio4_rtkit_ops);
	if (IS_ERR(atcphy->cio4_rtk)) {
		ret = PTR_ERR(atcphy->cio4_rtk);
		atcphy->cio4_rtk = NULL;
		goto err_power;
	}
	set32(atcphy->regs.cio4_cpu + 0x40, BIT(0));
	writel(BIT(4), atcphy->regs.cio4_cpu + 0x28);
	ret = apple_rtkit_boot(atcphy->cio4_rtk);
	if (ret)
		goto err_rtkit;
	/* This control endpoint is implicit in CIO4's firmware boot epmap. */
	ret = apple_rtkit_start_ep_implicit(atcphy->cio4_rtk, CIO4_CONTROL_EP);
	if (ret)
		goto err_rtkit;
	atcphy->cio4_running = true;
	set32(atcphy->regs.cio4_power, BIT(4));
	atcphy->mode = mode;
	dev_info(atcphy->dev, "CIO4 %s PHY firmware ready (orientation %s)\n",
		 mode == APPLE_ATCPHY_MODE_USB3 ? "USB3" : "USB4",
		 atcphy->swap_lanes ? "reverse" : "normal");
	return 0;

err_rtkit:
	apple_rtkit_free(atcphy->cio4_rtk);
	atcphy->cio4_rtk = NULL;
err_power:
	t6040_cio4_power_off(atcphy);
	return dev_err_probe(atcphy->dev, ret, "CIO4 PHY startup failed\n");
}

static int t6040_cio4_enable(struct apple_atcphy *atcphy)
{
	return t6040_cio4_start(atcphy, APPLE_ATCPHY_MODE_USB4);
}

static int t6040_cio4_usb3_enable(struct apple_atcphy *atcphy)
{
	return t6040_cio4_start(atcphy, APPLE_ATCPHY_MODE_USB3);
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
	if (IS_ERR(atcphy->tunables.cio4_analog))
		return PTR_ERR(atcphy->tunables.cio4_analog);
	if (atcphy->regs.usb2phy) {
		struct resource *usb2 = platform_get_resource_byname(pdev, IORESOURCE_MEM, "usb2phy");
		struct {
			const char *name;
			struct apple_tunable **table;
			struct resource *res;
		} tables[] = {
			{ "apple,tunable-usb2-default", &atcphy->tunables.usb2_default, usb2 },
			{ "apple,tunable-usb2-host", &atcphy->tunables.usb2_host, usb2 },
			{ "apple,tunable-usb2-device", &atcphy->tunables.usb2_device, usb2 },
			{ "apple,tunable-pipehandler", &atcphy->tunables.pipehandler, atcphy->res.pipehandler },
		};

		for (int i = 0; i < ARRAY_SIZE(tables); i++) {
			*tables[i].table = devm_apple_tunable_parse(atcphy->dev, atcphy->np,
								 tables[i].name, tables[i].res);
			if (IS_ERR(*tables[i].table))
				return PTR_ERR(*tables[i].table);
		}
	}
	return 0;
}
