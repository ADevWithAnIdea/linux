// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sequence the T8140 DCP retained CPU-domain sideband controls.
 * Uses existing genpd notifications and the existing PMGR regmap; no provider
 * callback replacement. The sequencer has no active runtime-PM usage.
 */
#include <linux/bitfield.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include "dcp.h"

#define NEO_ORDER_SUPPORT_A 0x8c
#define NEO_ORDER_SUPPORT_B 0xd0
#define NEO_ORDER_CPU_AUTO BIT(28)
#define NEO_ORDER_CPU_FLAGS (BIT(8) | BIT(9))
#define NEO_ORDER_CPU_TARGET GENMASK(3, 0)
#define NEO_ORDER_CPU_ACTUAL GENMASK(7, 4)
#define NEO_ORDER_CPU_ACTIVE 0xf

struct neo_order {
	struct device *dev;
	struct notifier_block notifier;
	struct regmap *cpu_map;
	u32 cpu_offset;
	void __iomem *support;
	bool suspended;
	int error;
};
static struct neo_order *neo_order_device;
static struct device *neo_order_owner;

static int neo_order_notify(struct notifier_block *nb, unsigned long event,
			    void *unused)
{
	struct neo_order *order = container_of(nb, struct neo_order, notifier);
	u32 a, b, cpu;
	int ret;

	if (event != GENPD_NOTIFY_PRE_OFF && event != GENPD_NOTIFY_ON)
		return NOTIFY_DONE;
	if (event == GENPD_NOTIFY_ON && !order->suspended)
		return NOTIFY_DONE;
	a = readl(order->support + NEO_ORDER_SUPPORT_A);
	b = readl(order->support + NEO_ORDER_SUPPORT_B);
	ret = regmap_read(order->cpu_map, order->cpu_offset, &cpu);
	if (ret)
		goto failed;
	if (event == GENPD_NOTIFY_PRE_OFF) {
		if (a != 0 || b != 1 ||
		    FIELD_GET(NEO_ORDER_CPU_ACTUAL, cpu) !=
			    NEO_ORDER_CPU_ACTIVE) {
			ret = -EINVAL;
			goto failed;
		}
		ret = regmap_update_bits(
			order->cpu_map, order->cpu_offset,
			NEO_ORDER_CPU_AUTO | NEO_ORDER_CPU_FLAGS, 0);
		if (ret)
			goto failed;
		writel(3, order->support + NEO_ORDER_SUPPORT_A);
		readl(order->support + NEO_ORDER_SUPPORT_A);
		writel(0, order->support + NEO_ORDER_SUPPORT_B);
		readl(order->support + NEO_ORDER_SUPPORT_B);
		order->suspended = true;
	} else {
		if (a != 3 || b != 0 || (cpu & NEO_ORDER_CPU_AUTO) ||
		    FIELD_GET(NEO_ORDER_CPU_TARGET, cpu) !=
			    NEO_ORDER_CPU_ACTIVE ||
		    FIELD_GET(NEO_ORDER_CPU_ACTUAL, cpu) !=
			    NEO_ORDER_CPU_ACTIVE) {
			ret = -EINVAL;
			goto failed;
		}
		writel(0, order->support + NEO_ORDER_SUPPORT_A);
		readl(order->support + NEO_ORDER_SUPPORT_A);
		writel(1, order->support + NEO_ORDER_SUPPORT_B);
		readl(order->support + NEO_ORDER_SUPPORT_B);
		ret = regmap_update_bits(order->cpu_map, order->cpu_offset,
					 NEO_ORDER_CPU_AUTO |
						 NEO_ORDER_CPU_FLAGS,
					 NEO_ORDER_CPU_AUTO);
		if (ret)
			goto failed;
		order->suspended = false;
	}
	order->error = 0;
	regmap_read(order->cpu_map, order->cpu_offset, &cpu);
	dev_info(order->dev,
		 "private CPU-domain notifier %s CPU=%#x A=%#x B=%#x\n",
		 event == GENPD_NOTIFY_PRE_OFF ? "PRE_OFF" : "ON", cpu,
		 readl(order->support + NEO_ORDER_SUPPORT_A),
		 readl(order->support + NEO_ORDER_SUPPORT_B));
	return NOTIFY_OK;
failed:
	order->error = ret;
	dev_err(order->dev,
		"private CPU-domain notifier refused event=%lu error=%d A=%#x B=%#x\n",
		event, ret, a, b);
	return notifier_from_errno(ret);
}

static void neo_order_remove_notifier(void *data)
{
	struct neo_order *order = data;

	dev_pm_genpd_remove_notifier(order->dev);
}

static int neo_order_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *cpu;
	struct neo_order *order;
	const char *label;
	u32 state;
	int ret;

	if (!neo_order_owner)
		return -ENODEV;
	cpu = of_parse_phandle(dev->of_node, "power-domains", 0);
	if (!cpu)
		return -EINVAL;
	if (of_property_read_string(cpu, "label", &label) ||
	    strcmp(label, "disp_cpu") ||
	    !of_property_read_bool(cpu, "apple,no-auto-enable")) {
		of_node_put(cpu);
		return -EINVAL;
	}
	order = devm_kzalloc(dev, sizeof(*order), GFP_KERNEL);
	if (!order) {
		of_node_put(cpu);
		return -ENOMEM;
	}
	order->dev = dev;
	order->cpu_map = syscon_node_to_regmap(cpu->parent);
	ret = of_property_read_u32(cpu, "reg", &order->cpu_offset);
	of_node_put(cpu);
	if (ret)
		return ret;
	if (IS_ERR(order->cpu_map))
		return PTR_ERR(order->cpu_map);
	order->support = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(order->support))
		return PTR_ERR(order->support);
	if (resource_size(platform_get_resource(pdev, IORESOURCE_MEM, 0)) !=
	    0x100)
		return -EINVAL;
	ret = regmap_read(order->cpu_map, order->cpu_offset, &state);
	if (ret)
		return ret;
	if (FIELD_GET(NEO_ORDER_CPU_ACTUAL, state) != NEO_ORDER_CPU_ACTIVE ||
	    readl(order->support + NEO_ORDER_SUPPORT_A) != 0 ||
	    readl(order->support + NEO_ORDER_SUPPORT_B) != 1)
		return -EINVAL;
	/*
	 * Initial active state matches native; the DT prevents the generic
	 * provider from enabling AUTO before the resume notification.
	 */
	ret = regmap_update_bits(order->cpu_map, order->cpu_offset,
				 NEO_ORDER_CPU_AUTO | NEO_ORDER_CPU_FLAGS,
				 NEO_ORDER_CPU_AUTO);
	if (ret)
		return ret;
	ret = pm_runtime_set_suspended(dev);
	if (ret)
		return ret;
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;
	order->notifier.notifier_call = neo_order_notify;
	ret = dev_pm_genpd_add_notifier(dev, &order->notifier);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, neo_order_remove_notifier, order);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, order);
	neo_order_device = order;
	dev_info(
		dev,
		"private CPU-domain notifier ready; observer runtime suspended\n");
	return 0;
}
static void neo_order_remove(struct platform_device *pdev)
{
	neo_order_device = NULL;
}
static const struct of_device_id neo_order_match[] = {
	{ .compatible = "apple,t8140-dcp-power-sequencer" },
	{}
};
static struct platform_driver neo_order_driver = {
	.probe = neo_order_probe,
	.remove = neo_order_remove,
	.driver = { .name = "neo-private-pmgr-order",
		    .of_match_table = neo_order_match,
		    .probe_type = PROBE_FORCE_SYNCHRONOUS,
		    .suppress_bind_attrs = true },
};
static void neo_order_unregister(void *unused)
{
	platform_driver_unregister(&neo_order_driver);
	neo_order_owner = NULL;
}
int neo_diag_pmgr_order_init(struct apple_dcp *dcp)
{
	int ret;

	if (!dcp_has_panel(dcp))
		return 0;
	if (neo_order_owner)
		return -EBUSY;
	neo_order_owner = dcp->dev;
	ret = platform_driver_register(&neo_order_driver);
	if (ret) {
		neo_order_owner = NULL;
		return ret;
	}
	if (!neo_order_device) {
		neo_order_unregister(NULL);
		return -ENODEV;
	}
	return devm_add_action_or_reset(dcp->dev, neo_order_unregister, NULL);
}
int neo_diag_pmgr_order_status(struct apple_dcp *dcp)
{
	if (!dcp_has_panel(dcp))
		return 0;
	if (!neo_order_device)
		return -ENODEV;
	return neo_order_device->error;
}
