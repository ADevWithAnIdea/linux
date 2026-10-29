/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * EXPERIMENTAL: fixed four-lane T6040 HDMI link, native J616c 26.6.2 MMIO.
 *
 * CIO4 has different PLL/PMA register blocks from T8130. Retain iBoot's
 * board calibration and update only bits changed by the native sequence.
 * Request/acknowledge transitions and PLL lock are checked before proceeding.
 * Only the captured HBR3 link is supported at this bring-up stage. USB and
 * other link rates must not be advertised as implemented by this path.
 */
struct t6040_dp_step {
	u32 reg, mask, value;
	bool wait;
};

static const struct t6040_dp_step t6040_dp_common[] = {
	{ 0x0d094, 0x00080000, 0x00080000, false },
	{ 0x0d098, 0x00080000, 0x00080000, false },
	{ 0x0d0a4, 0x00800000, 0x00800000, false },
	{ 0x0d0a8, 0x00200000, 0x00200000, false },
	{ 0x1d094, 0x00080000, 0x00080000, false },
	{ 0x1d098, 0x00080000, 0x00080000, false },
	{ 0x1d0a4, 0x00800000, 0x00800000, false },
	{ 0x1d0a8, 0x00200000, 0x00200000, false },
	{ 0x00800, 0x00000200, 0x00000000, false },
	{ 0x00818, 0x00003000, 0x00003000, false },
	{ 0x03888, 0x0000000c, 0x00000004, false },
	{ 0x28004, 0x00000008, 0x00000008, false },
	{ 0x03adc, 0x000001d0, 0x00000140, false },
	{ 0x0e0cc, 0x00380000, 0x00380000, false },
	{ 0x1e0cc, 0x00380000, 0x00380000, false },
	{ 0x0d090, 0x00380000, 0x00380000, false },
	{ 0x1d090, 0x00380000, 0x00380000, false },
	{ 0x03224, 0x00000800, 0x00000800, false },
	{ 0x03230, 0x00000007, 0x00000007, false },
	{ 0x03240, 0x00000009, 0x00000009, false },
	{ 0x032b0, 0x09800000, 0x01800000, false },
	{ 0x0006c, 0x0300006c, 0x00000048, false },
	{ 0x00078, 0x0000001e, 0x00000014, false },
	{ 0x00078, 0x00022020, 0x00022000, false },
	{ 0x0000c, 0x0000002a, 0x00000000, false },
	{ 0x28400, 0x00000002, 0x00000002, false },
	{ 0x28400, 0x00000008, 0x00000008, false },
	{ 0x28400, 0x00000200, 0x00000000, false },
};

static const struct t6040_dp_step t6040_dp_hbr3[] = {
	{ 0x07000, 0x00001110, 0x00001110, false },
	{ 0x03234, 0x00000001, 0x00000000, false },
	{ 0x03080, 0x37838333, 0x16020223, false },
	{ 0x03084, 0x0bff7ffc, 0x0bff7ffc, false },
	{ 0x03088, 0x00034000, 0x00024000, false },
	{ 0x03208, 0x001f0000, 0x00070000, false },
	{ 0x03218, 0x00000001, 0x00000001, false },
	{ 0x03200, 0x00000aaa, 0x00000aaa, false },
	{ 0x01030, 0x00004000, 0x00004000, false },
	{ 0x01018, 0x00002000, 0x00002000, false },
	{ 0x0101c, 0x00000007, 0x00000007, false },
	{ 0x0101c, 0x00000008, 0x00000008, false },
	{ 0x0102c, 0x0000011f, 0x0000011f, true },
	{ 0x0101c, 0x00000008, 0x00000000, false },
	{ 0x0102c, 0x00000100, 0x00000000, true },
	{ 0x07034, 0x00000008, 0x00000008, true },
	{ 0x0c03c, 0x00010000, 0x00010000, false },
	{ 0x1c03c, 0x00010000, 0x00010000, false },
	{ 0x07000, 0x0000000c, 0x0000000c, false },
	{ 0x00120, 0x00000010, 0x00000010, false },
	{ 0x00120, 0x00010000, 0x00010000, true },
	{ 0x00120, 0x00000001, 0x00000001, false },
	{ 0x00120, 0x00020000, 0x00020000, true },
	{ 0x00120, 0x00000100, 0x00000000, false },
	{ 0x0f044, 0x0aafa880, 0x0aafa800, false },
	{ 0x0f044, 0x01000000, 0x01000000, false },
	{ 0x0f044, 0x04000000, 0x04000000, false },
	{ 0x0f044, 0x00000300, 0x00000300, false },
	{ 0x0f044, 0x00068000, 0x00000000, false },
	{ 0x0e100, 0x00100000, 0x00100000, false },
	{ 0x0e104, 0x20000000, 0x20000000, false },
	{ 0x0e100, 0x00001f81, 0x00000001, false },
	{ 0x0f004, 0x02000000, 0x02000000, false },
	{ 0x0f004, 0x30000000, 0x30000000, false },
	{ 0x0f00c, 0x04000000, 0x04000000, false },
	{ 0x0f008, 0x00000002, 0x00000002, false },
	{ 0x0f010, 0x00000600, 0x00000600, false },
	{ 0x0f01c, 0x00800000, 0x00800000, false },
	{ 0x00120, 0x00000020, 0x00000020, false },
	{ 0x00120, 0x00000002, 0x00000002, false },
	{ 0x00120, 0x00000200, 0x00000000, false },
	{ 0x1f044, 0x0aafa880, 0x0aafa800, false },
	{ 0x1f044, 0x01000000, 0x01000000, false },
	{ 0x1f044, 0x04000000, 0x04000000, false },
	{ 0x1f044, 0x00000300, 0x00000300, false },
	{ 0x1f044, 0x00068000, 0x00000000, false },
	{ 0x1e100, 0x00100000, 0x00100000, false },
	{ 0x1e104, 0x20000000, 0x20000000, false },
	{ 0x1e100, 0x00001f81, 0x00000001, false },
	{ 0x1f004, 0x02000000, 0x02000000, false },
	{ 0x1f004, 0x30000000, 0x30000000, false },
	{ 0x1f00c, 0x04000000, 0x04000000, false },
	{ 0x1f008, 0x00000002, 0x00000002, false },
	{ 0x1f010, 0x00000600, 0x00000600, false },
	{ 0x1f01c, 0x00800000, 0x00800000, false },
	{ 0x0c0f4, 0x00003000, 0x00003000, false },
	{ 0x09188, 0x00000001, 0x00000001, false },
	{ 0x091d4, 0x00000300, 0x00000300, false },
	{ 0x0c024, 0x003f0000, 0x002a0000, false },
	{ 0x0c078, 0x00080000, 0x00080000, false },
	{ 0x0c07c, 0x00000080, 0x00000080, false },
	{ 0x0c084, 0x0000c000, 0x00008000, false },
	{ 0x0c0d4, 0x00400000, 0x00400000, false },
	{ 0x0c0d8, 0x00000300, 0x00000200, false },
	{ 0x0c040, 0x00000800, 0x00000800, false },
	{ 0x0c048, 0x00000400, 0x00000400, false },
	{ 0x0c028, 0x0019c020, 0x0011c020, false },
	{ 0x0c02c, 0x000000b0, 0x000000b0, false },
	{ 0x0c028, 0x00000800, 0x00000800, false },
	{ 0x0c028, 0x000001c0, 0x00000180, false },
	{ 0x00010, 0x00000010, 0x00000010, false },
	{ 0x00010, 0x00000040, 0x00000040, false },
	{ 0x00010, 0x00000001, 0x00000001, false },
	{ 0x00010, 0x00000004, 0x00000004, false },
	{ 0x0000c, 0x00000400, 0x00000400, false },
	{ 0x0000c, 0x00800000, 0x00800000, true },
	{ 0x0000c, 0x00000040, 0x00000040, false },
	{ 0x0000c, 0x01000000, 0x01000000, true },
	{ 0x0000c, 0x00004000, 0x00000000, false },
	{ 0x091d0, 0x00000001, 0x00000001, false },
	{ 0x091d0, 0x00000002, 0x00000002, false },
	{ 0x0c040, 0x00030000, 0x00030000, false },
	{ 0x0c040, 0x00008000, 0x00008000, false },
	{ 0x0c028, 0x00001010, 0x00001010, false },
	{ 0x0c028, 0x00000200, 0x00000200, false },
	{ 0x0c028, 0x00001000, 0x00000000, false },
	{ 0x0c024, 0x30000000, 0x30000000, false },
	{ 0x0c024, 0xc0000000, 0xc0000000, false },
	{ 0x0c028, 0x00000400, 0x00000400, false },
	{ 0x0c028, 0x00000200, 0x00000000, false },
	{ 0x0c028, 0x00000400, 0x00000000, false },
	{ 0x0c044, 0x18000000, 0x18000000, false },
	{ 0x0c040, 0x00000020, 0x00000020, false },
	{ 0x0c044, 0x40000000, 0x40000000, false },
	{ 0x0c044, 0x06000000, 0x06000000, false },
	{ 0x0c040, 0x00004000, 0x00000000, false },
	{ 0x0d0ac, 0x20000000, 0x20000000, false },
	{ 0x0d0a8, 0x00001f81, 0x00000001, false },
	{ 0x0d018, 0x00000028, 0x00000020, false },
	{ 0x1c0f4, 0x00003000, 0x00003000, false },
	{ 0x19188, 0x00000001, 0x00000001, false },
	{ 0x191d4, 0x00000300, 0x00000300, false },
	{ 0x1c024, 0x003f0000, 0x002a0000, false },
	{ 0x1c078, 0x00080000, 0x00080000, false },
	{ 0x1c07c, 0x00000080, 0x00000080, false },
	{ 0x1c084, 0x0000c000, 0x00008000, false },
	{ 0x1c0d4, 0x00400000, 0x00400000, false },
	{ 0x1c0d8, 0x00000300, 0x00000200, false },
	{ 0x1c040, 0x00000800, 0x00000800, false },
	{ 0x1c048, 0x00000400, 0x00000400, false },
	{ 0x1c028, 0x0019c020, 0x0011c020, false },
	{ 0x1c02c, 0x000000b0, 0x000000b0, false },
	{ 0x1c028, 0x00000800, 0x00000800, false },
	{ 0x1c028, 0x000001c0, 0x00000180, false },
	{ 0x00010, 0x00000020, 0x00000020, false },
	{ 0x00010, 0x00000080, 0x00000080, false },
	{ 0x00010, 0x00000002, 0x00000002, false },
	{ 0x00010, 0x00000008, 0x00000008, false },
	{ 0x0000c, 0x00000800, 0x00000800, false },
	{ 0x0000c, 0x00000080, 0x00000080, false },
	{ 0x0000c, 0x00008000, 0x00000000, false },
	{ 0x191d0, 0x00000001, 0x00000001, false },
	{ 0x191d0, 0x00000002, 0x00000002, false },
	{ 0x1c040, 0x00030000, 0x00030000, false },
	{ 0x1c040, 0x00008000, 0x00008000, false },
	{ 0x1c028, 0x00001010, 0x00001010, false },
	{ 0x1c028, 0x00000200, 0x00000200, false },
	{ 0x1c028, 0x00001000, 0x00000000, false },
	{ 0x1c024, 0x30000000, 0x30000000, false },
	{ 0x1c024, 0xc0000000, 0xc0000000, false },
	{ 0x1c028, 0x00000400, 0x00000400, false },
	{ 0x1c028, 0x00000200, 0x00000000, false },
	{ 0x1c028, 0x00000400, 0x00000000, false },
	{ 0x1c044, 0x18000000, 0x18000000, false },
	{ 0x1c040, 0x00000020, 0x00000020, false },
	{ 0x1c044, 0x40000000, 0x40000000, false },
	{ 0x1c044, 0x06000000, 0x06000000, false },
	{ 0x1c040, 0x00004000, 0x00000000, false },
	{ 0x1d0ac, 0x20000000, 0x20000000, false },
	{ 0x1d0a8, 0x00001f81, 0x00000001, false },
	{ 0x1d018, 0x00000028, 0x00000020, false },
	{ 0x07000, 0x00000001, 0x00000000, false },
};

static int t6040_dp_apply(struct apple_atcphy *atcphy,
			   const struct t6040_dp_step *steps, size_t count)
{
	u32 value;
	int ret;

	for (size_t i = 0; i < count; i++) {
		const struct t6040_dp_step *step = &steps[i];

		if (step->wait) {
			ret = readl_poll_timeout(atcphy->regs.core + step->reg, value,
					 (value & step->mask) == step->value, 1, 10000);
			if (ret) {
				dev_err(atcphy->dev, "CIO4 DP register %#x: timeout (%#x, mask %#x, expected %#x)\n",
					step->reg, value, step->mask, step->value);
				return ret;
			}
		} else {
			core_mask32(atcphy, step->reg, step->mask, step->value);
		}
	}
	return 0;
}

static int t6040_dp_power_on(struct apple_atcphy *atcphy)
{
	static const u32 fabric_ungate[] = {
		0x010, 0x024, 0x038, 0x048, 0x058, 0x068, 0x074, 0x08c,
		0x09c, 0x0a8, 0x0b8, 0x0c8, 0x0e0, 0x0ec, 0x0f8, 0x108,
		0x110, 0x118, 0x128, 0x134, 0x144, 0x154, 0x15c, 0x16c,
		0x17c, 0x18c,
	};
	void __iomem *power = atcphy->regs.cio4_power;
	u32 value;
	int ret;

	/* CIO4 moved the PHY power/APB controls outside the analog core. */
	set32(power, BIT(0));
	ret = readl_poll_timeout(power + 4, value, value & BIT(0), 1, 10000);
	if (ret)
		return ret;
	set32(power, BIT(1));
	ret = readl_poll_timeout(power + 4, value, value & BIT(1), 1, 10000);
	if (ret)
		return ret;
	clear32(power, BIT(2));
	clear32(power + 0x28, BIT(0));
	set32(power, BIT(3));
	for (size_t i = 0; i < ARRAY_SIZE(fabric_ungate); i++)
		clear32(atcphy->regs.cio4_fabric + fabric_ungate[i], BIT(15));
	set32(atcphy->regs.cio4_common + 8, BIT(0));
	return 0;
}

static int t6040_dp_enable(struct apple_atcphy *atcphy)
{
	int ret;

	if (atcphy->mode == APPLE_ATCPHY_MODE_DP)
		return 0;
	ret = t6040_dp_power_on(atcphy);
	if (ret)
		return ret;
	ret = t6040_dp_apply(atcphy, t6040_dp_common, ARRAY_SIZE(t6040_dp_common));
	if (!ret) {
		set32(atcphy->regs.cio4_power, BIT(4));
		atcphy->mode = APPLE_ATCPHY_MODE_DP;
		atcphy->dp_link_rate = -1;
	}
	return ret;
}

static int t6040_dp_configure(struct apple_atcphy *atcphy,
			       struct phy_configure_opts_dp *opts)
{
	int ret;

	guard(mutex)(&atcphy->lock);
	if (opts->set_voltages || (opts->set_lanes && opts->lanes != 4))
		return -EOPNOTSUPP;
	if (!opts->set_rate)
		return 0;
	if (!opts->link_rate) {
		/* Native rate-zero disconnect leaves board calibration intact. */
		core_set32(atcphy, 0x7000, BIT(0));
		atcphy->dp_link_rate = -1;
		return 0;
	}
	if (opts->link_rate != 8100)
		return -EOPNOTSUPP;
	if (atcphy->mode != APPLE_ATCPHY_MODE_DP)
		return -EPIPE;
	if (atcphy->dp_link_rate == ATCPHY_DP_LINK_RATE_HBR3)
		return 0;
	ret = t6040_dp_apply(atcphy, t6040_dp_hbr3, ARRAY_SIZE(t6040_dp_hbr3));
	if (!ret) {
		atcphy->dp_link_rate = ATCPHY_DP_LINK_RATE_HBR3;
		dev_info(atcphy->dev, "CIO4 DP HBR3 PLL locked; four lanes configured\n");
	}
	return ret;
}
