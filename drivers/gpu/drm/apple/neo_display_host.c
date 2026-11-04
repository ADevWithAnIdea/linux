// SPDX-License-Identifier: GPL-2.0-only
/* T8140 DCP retained power handshake and protected-host display callbacks. */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/workqueue.h>
#include <linux/soc/apple/mailbox.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/unaligned.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/soc/apple/rtkit.h>
#include "dcp.h"
#define PROXY_TX_CONTROL 0x0
#define PROXY_RX_CONTROL 0x4
#define PROXY_TX_DATA 0x180
#define PROXY_TX_ENDPOINT 0x188
#define PROXY_RX_DATA 0x1b0
#define PROXY_RX_ENDPOINT 0x1b8
#define PROXY_FIFO_EMPTY BIT(17)
#define PROXY_FIFO_FULL BIT(16)
#define MGMT_TYPE_SHIFT 52
#define MGMT_HELLO 1
#define MGMT_HELLO_REPLY 2
#define MGMT_ENDPOINT_MAP 8
#define MGMT_AP_POWER 0xb
#define MGMT_ENDPOINT_MAP_LAST BIT_ULL(51)
#define MGMT_ENDPOINT_MAP_BITMAP GENMASK_ULL(31, 0)
#define MGMT_ENDPOINT_MAP_REPLY_MORE BIT_ULL(0)
#define AP_POWER_IDLE 0x201
#define AP_POWER_NATIVE_WAKE 0x220
#define AP_POWER_ON_ACK 0x20
#define PROTOCOL_VERSION 12
/*
 * Endpoint-48 transport uses the mailbox framing shared with the existing
 * Neo AOP second-host client. No ExclaveOS
 * execution, DART implementation changes, or borrowed firmware pointers.
 * The link and DMA queues remain owned until reboot for late replies.
 */
#define NEO_DISPLAY_PROTECTED_EP 0x30
#define NEO_PROTECTED_QUEUE_BYTES 1024
#define NEO_PROTECTED_ADDRESS_SHIFT 4
/* Runtime-captured method selector, not an address or monitor parameter. */
#define NEO_PROTECTED_DISPLAY_STATE_METHOD 0xf5708ca12609e19cULL
#define NEO_PROTECTED_DISPLAY_RESTART_METHOD 0xd08ae9387df1f2ddULL
#define NEO_PROTECTED_WIRE_OFFSET GENMASK_ULL(35, 0)
static struct {
	struct apple_dcp *dcp;
	struct platform_device *pdev;
	void __iomem *registers;
	void *rx, *tx;
	dma_addr_t rx_dva, tx_dva;
	u64 vm_base;
	struct apple_mbox *mbox;
	struct workqueue_struct *wq;
	struct completion queues_ready;
	struct mutex lock;
	bool running, pending;
	int error;
	unsigned int requests;
} display_transport;
static bool neo_protected_display_initialized;
static bool neo_protected_display_attempted;
static int neo_diag_unused_host_state(struct apple_dcp *dcp, bool waking);

static int neo_protected_send(void __iomem *registers, u8 endpoint, u64 word)
{
	struct apple_mbox_msg message = { .msg0 = word, .msg1 = endpoint };

	if (!display_transport.mbox)
		return -ENODEV;
	pr_info("NEO_PROTECTED_EP TX ep=%x word=%016llx\n", endpoint, word);
	dma_wmb();
	return apple_mbox_send(display_transport.mbox, message, false);
}
static int neo_protected_message(void __iomem *registers, u8 endpoint, u64 word)
{
	struct device *dev;
	int ret;
	u16 type = word >> 48;

	if (!display_transport.pdev || endpoint != NEO_DISPLAY_PROTECTED_EP)
		return -EOPNOTSUPP;
	dev = &display_transport.pdev->dev;
	pr_info("NEO_PROTECTED_EP RX ep=%x word=%016llx\n", endpoint, word);
	switch (type) {
	case 0x1200:
		if ((word & GENMASK_ULL(47, 0)) != NEO_PROTECTED_QUEUE_BYTES)
			return -EMSGSIZE;
		if (!display_transport.rx) {
			display_transport.rx = dma_alloc_coherent(
				dev, NEO_PROTECTED_QUEUE_BYTES,
				&display_transport.rx_dva, GFP_KERNEL);
			display_transport.tx = dma_alloc_coherent(
				dev, NEO_PROTECTED_QUEUE_BYTES,
				&display_transport.tx_dva, GFP_KERNEL);
			if (!display_transport.rx || !display_transport.tx)
				return -ENOMEM;
		}
		if ((display_transport.rx_dva & ~NEO_PROTECTED_WIRE_OFFSET) !=
			    display_transport.vm_base ||
		    (display_transport.tx_dva & ~NEO_PROTECTED_WIRE_OFFSET) !=
			    display_transport.vm_base)
			return -ERANGE;
		dev_info(
			dev,
			"private protected display queues RX=%pad TX=%pad size=%u\n",
			&display_transport.rx_dva, &display_transport.tx_dva,
			NEO_PROTECTED_QUEUE_BYTES);
		memset(display_transport.rx, 0, NEO_PROTECTED_QUEUE_BYTES);
		memset(display_transport.tx, 0, NEO_PROTECTED_QUEUE_BYTES);
		ret = neo_protected_send(registers, endpoint, 0x1200ULL << 48);
		/*
		 * Published pointers are in 16-byte units. The prior byte-offset
		 * encoding caused a firmware range check at precisely wire << 4.
		 */
		if (!ret)
			ret = neo_protected_send(
				registers, endpoint,
				(0x5041ULL << 48) |
					(display_transport.rx_dva >>
					 NEO_PROTECTED_ADDRESS_SHIFT));
		if (!ret)
			ret = neo_protected_send(
				registers, endpoint,
				(0x5001ULL << 48) |
					(display_transport.tx_dva >>
					 NEO_PROTECTED_ADDRESS_SHIFT));
		if (!ret)
			complete(&display_transport.queues_ready);
		return ret;
	case 0x3000:
		if (!display_transport.rx || display_transport.pending ||
		    (word & GENMASK_ULL(47, 0)) != 12)
			return -EPROTO;
		dma_rmb();
		/*
		 * 5001 is the firmware-to-host request buffer; 5041 holds the reply.
		 * The corrected native capture contains a method selector plus one u32.
		 * Only the observed sleep/wake values are accepted.
		 */
		dev_info(dev, "private protected display request bytes=%*phN\n",
			 64, display_transport.tx);
		if (memchr_inv(display_transport.tx + 12, 0,
			       NEO_PROTECTED_QUEUE_BYTES - 12))
			return -EOPNOTSUPP;
		switch (get_unaligned_le64(display_transport.tx)) {
		case NEO_PROTECTED_DISPLAY_STATE_METHOD:
			if (get_unaligned_le32(display_transport.tx + 8) != 0 &&
			    get_unaligned_le32(display_transport.tx + 8) != 2)
				return -EOPNOTSUPP;
			dev_info(
				dev,
				"private protected display state callback argument=%u\n",
				get_unaligned_le32(display_transport.tx + 8));
			break;
		case NEO_PROTECTED_DISPLAY_RESTART_METHOD:
			/*
			 * Native runtime capture: full primary CPU stop/start, then this
			 * selector with argument 1, zero-length reply, final firmware ACK.
			 * Semantic name is provisional. No unobserved arguments accepted.
			 */
			if (get_unaligned_le32(display_transport.tx + 8) != 1)
				return -EOPNOTSUPP;
			dev_info(
				dev,
				"private protected display restart callback argument=1\n");
			break;
		default:
			return -EOPNOTSUPP;
		}
		memset(display_transport.rx, 0, NEO_PROTECTED_QUEUE_BYTES);
		ret = neo_protected_send(registers, endpoint, 0x2000ULL << 48);
		if (!ret) {
			display_transport.pending = true;
			display_transport.requests++;
		}
		return ret;
	case 0x4000:
		if (!display_transport.pending || (word & GENMASK_ULL(47, 0)))
			return -EPROTO;
		display_transport.pending = false;
		dev_info(
			dev,
			"private protected display exchange completed count=%u\n",
			display_transport.requests);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}
struct neo_protected_packet {
	struct work_struct work;
	struct apple_mbox_msg message;
};
static void neo_protected_receive_work(struct work_struct *work)
{
	struct neo_protected_packet *packet =
		container_of(work, struct neo_protected_packet, work);
	int ret;

	mutex_lock(&display_transport.lock);
	if (!display_transport.running || display_transport.error)
		goto unlock;
	dma_rmb();
	ret = neo_protected_message(display_transport.registers,
				    packet->message.msg1 & 0xff,
				    packet->message.msg0);
	if (ret) {
		display_transport.error = ret;
		dev_err(&display_transport.pdev->dev,
			"private protected display IRQ receive failed: %d\n",
			ret);
		complete_all(&display_transport.queues_ready);
	}
unlock:
	mutex_unlock(&display_transport.lock);
	kfree(packet);
}
static void neo_protected_receive(struct apple_mbox *mbox,
				  struct apple_mbox_msg message, void *cookie)
{
	struct neo_protected_packet *packet =
		kmalloc(sizeof(*packet), GFP_ATOMIC);
	if (!packet) {
		WRITE_ONCE(display_transport.error, -ENOMEM);
		complete_all(&display_transport.queues_ready);
		return;
	}
	packet->message = message;
	INIT_WORK(&packet->work, neo_protected_receive_work);
	queue_work(display_transport.wq, &packet->work);
}
static void neo_protected_pause(void)
{
	if (!display_transport.pdev)
		return;
	/*
	 * Drain every message already received before beginning polled AP idle.
	 * The mailbox IRQ is disabled before its FIFO is read synchronously.
	 */
	apple_mbox_stop(display_transport.mbox);
	flush_workqueue(display_transport.wq);
	WRITE_ONCE(display_transport.running, false);
}
static void neo_protected_continue(void)
{
	int ret;

	if (!display_transport.pdev || display_transport.error)
		return;
	WRITE_ONCE(display_transport.running, true);
	ret = apple_mbox_start(display_transport.mbox);
	if (ret) {
		display_transport.error = ret;
		dev_err(&display_transport.pdev->dev,
			"private protected mailbox start failed: %d\n", ret);
	}
}
static int neo_protected_transport_start(struct apple_dcp *dcp)
{
	struct device_node *node;
	struct platform_device *primary, *child;
	int ret;

	if (neo_protected_display_initialized)
		return 0;
	if (neo_protected_display_attempted)
		return -EBUSY;
	primary = to_platform_device(dcp->dev);
	get_device(&primary->dev);
	if (!dcp || dcp->fw_compat != DCP_FIRMWARE_V_26_6_T8140 ||
	    !dcp_has_panel(dcp) || !dcp->active || dcp->crashed ||
	    dcp->diag_idle_done || dcp->diag_unused_host_quiesced) {
		put_device(&primary->dev);
		return -EBUSY;
	}
	node = of_get_child_by_name(primary->dev.of_node, "protected-host");
	if (!node) {
		put_device(&primary->dev);
		return -ENODEV;
	}
	child = of_platform_device_create(node, NULL, &primary->dev);
	if (!child) {
		of_node_put(node);
		put_device(&primary->dev);
		return -ENOMEM;
	}
	ret = dma_set_mask_and_coherent(&child->dev, DMA_BIT_MASK(42));
	if (!ret)
		ret = of_dma_configure(&child->dev, node, true);
	if (!ret) {
		struct iommu_domain *domain =
			iommu_get_domain_for_dev(&child->dev);
		if (!domain)
			ret = -ENODEV;
		else if (!domain->geometry.force_aperture ||
			 (domain->geometry.aperture_start &
			  NEO_PROTECTED_WIRE_OFFSET) ||
			 domain->geometry.aperture_end <
				 domain->geometry.aperture_start ||
			 domain->geometry.aperture_end -
					 domain->geometry.aperture_start >
				 NEO_PROTECTED_WIRE_OFFSET)
			ret = -ERANGE;
		else
			display_transport.vm_base =
				domain->geometry.aperture_start;
	}
	of_node_put(node);
	if (ret) {
		of_platform_device_destroy(&child->dev, NULL);
		put_device(&primary->dev);
		return ret;
	}
	display_transport.wq =
		alloc_ordered_workqueue("neo-dcp-protected", WQ_MEM_RECLAIM);
	if (!display_transport.wq) {
		of_platform_device_destroy(&child->dev, NULL);
		put_device(&primary->dev);
		return -ENOMEM;
	}
	display_transport.mbox = apple_mbox_get_byname(&child->dev, "mbox");
	if (IS_ERR(display_transport.mbox)) {
		ret = PTR_ERR(display_transport.mbox);
		display_transport.mbox = NULL;
		destroy_workqueue(display_transport.wq);
		display_transport.wq = NULL;
		of_platform_device_destroy(&child->dev, NULL);
		put_device(&primary->dev);
		return ret;
	}
	/*
	 * Controller owns its mapping and IRQs; this client owns only its link.
	 * Synchronous management FIFO reads occur only while that IRQ is stopped.
	 */
	display_transport.registers = display_transport.mbox->regs;
	display_transport.pdev = child;
	display_transport.dcp = dcp;
	mutex_init(&display_transport.lock);
	init_completion(&display_transport.queues_ready);
	display_transport.mbox->rx = neo_protected_receive;
	display_transport.mbox->cookie = &display_transport;
	neo_protected_display_attempted = true;
	/* Preserve callbacks and DMA until full reboot, including setup failure. */
	__module_get(THIS_MODULE);
	ret = neo_diag_unused_host_state(dcp, true);
	if (!ret &&
	    !wait_for_completion_timeout(&display_transport.queues_ready,
					 msecs_to_jiffies(3000)))
		ret = -ETIMEDOUT;
	if (!ret)
		ret = READ_ONCE(display_transport.error);
	if (!ret) {
		neo_protected_display_initialized = true;
		dev_info(
			dcp->dev,
			"private protected display transport initialized; queues ready, retained until reboot\n");
	} else
		dev_err(dcp->dev,
			"private protected display transport setup failed: %d; reboot required\n",
			ret);
	return ret;
}
static int send_management(void __iomem *registers, u64 word)
{
	u32 control;
	int ret = readl_poll_timeout(registers + PROXY_TX_CONTROL, control,
				     !(control & PROXY_FIFO_FULL), 10, 100000);
	if (ret)
		return ret;
	pr_info("NEO_UNUSED_HOST TX ep=0 word=%016llx\n", word);
	return neo_protected_send(registers, 0, word);
}
static int set_unused_host_power(void __iomem *registers, bool handshake,
				 bool waking)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(3000);
	bool hello = false, map_done = false, idle_sent = false;
	u32 request = waking ? AP_POWER_NATIVE_WAKE : AP_POWER_IDLE;
	u32 expected = waking ? AP_POWER_ON_ACK : AP_POWER_IDLE;

	if (!handshake) {
		int ret = send_management(registers, ((u64)MGMT_AP_POWER
						      << MGMT_TYPE_SHIFT) |
							     request);
		if (ret)
			return ret;
		idle_sent = true;
	}
	while (time_before(jiffies, deadline)) {
		u64 word, endpoint;
		unsigned int type;
		int ret;

		if (readl(registers + PROXY_RX_CONTROL) & PROXY_FIFO_EMPTY) {
			usleep_range(500, 1000);
			continue;
		}
		word = readq(registers + PROXY_RX_DATA);
		endpoint = readq(registers + PROXY_RX_ENDPOINT) & 0xff;
		pr_info("NEO_UNUSED_HOST RX ep=%llx word=%016llx\n", endpoint,
			word);
		if (endpoint) {
			ret = neo_protected_message(registers, endpoint, word);
			if (ret)
				return ret;
			continue;
		}
		type = (word >> MGMT_TYPE_SHIFT) & 0xff;
		switch (type) {
		case MGMT_HELLO:
			if (!handshake || hello ||
			    (word & 0xffff) > PROTOCOL_VERSION ||
			    ((word >> 16) & 0xffff) < PROTOCOL_VERSION)
				return -EPROTO;
			ret = send_management(registers,
					      ((u64)MGMT_HELLO_REPLY
					       << MGMT_TYPE_SHIFT) |
						      (PROTOCOL_VERSION << 16) |
						      PROTOCOL_VERSION);
			if (ret)
				return ret;
			hello = true;
			break;
		case MGMT_ENDPOINT_MAP:
			if (!hello || map_done)
				return -EPROTO;
			/*
			 * Advertise only the display application endpoint on this host.
			 * Keep the other host services unstarted.
			 */
			{
				u64 bitmap = 0;

				if (display_transport.pdev) {
					unsigned int page = (word >> 32) & 7;
					/* Keep management and the display endpoint. */
					if (page == 0)
						bitmap = (word & 1);
					if (page == 1)
						bitmap =
							word &
							BIT_ULL(NEO_DISPLAY_PROTECTED_EP -
								32);
				}
				ret = send_management(
					registers,
					(word & ~MGMT_ENDPOINT_MAP_BITMAP) |
						bitmap |
						((word &
						  MGMT_ENDPOINT_MAP_LAST) ?
							 0 :
							 MGMT_ENDPOINT_MAP_REPLY_MORE));
			}
			if (ret)
				return ret;
			if (word & MGMT_ENDPOINT_MAP_LAST) {
				map_done = true;
				ret = send_management(registers,
						      ((u64)MGMT_AP_POWER
						       << MGMT_TYPE_SHIFT) |
							      request);
				if (ret)
					return ret;
				idle_sent = true;
			}
			break;
		case MGMT_AP_POWER:
			if (!idle_sent || (word & 0xffff) != expected)
				return -EPROTO;
			return 0;
		default:
			return -EOPNOTSUPP;
		}
	}
	return -ETIMEDOUT;
}
static int neo_diag_unused_host_state(struct apple_dcp *dcp, bool waking)
{
	void __iomem *registers;
	bool handshake;
	int ret;

	if (dcp->fw_compat != DCP_FIRMWARE_V_26_6_T8140 || !dcp_has_panel(dcp))
		return 0;
	neo_protected_pause();
	if (display_transport.pdev && display_transport.error)
		return display_transport.error;
	registers = display_transport.registers;
	if (!registers || !display_transport.mbox)
		return -ENODEV;
	dev_info(dcp->dev,
		 "NEO_UNUSED_HOST state preparation wake=%u tx=%#x rx=%#x\n",
		 waking, readl(registers + PROXY_TX_CONTROL),
		 readl(registers + PROXY_RX_CONTROL));
	handshake = waking || !dcp->diag_unused_host_quiesced;
	if (!handshake && !dcp->diag_unused_host_awake &&
	    (readl(registers + PROXY_RX_CONTROL) & PROXY_FIFO_EMPTY))
		ret = 0;
	else
		ret = set_unused_host_power(registers, handshake, waking);
	if (!ret) {
		dcp->diag_unused_host_quiesced = true;
		dcp->diag_unused_host_awake = waking;
	}
	dev_info(dcp->dev,
		 "NEO_UNUSED_HOST state result=%d wake=%u tx=%#x rx=%#x\n", ret,
		 waking, readl(registers + PROXY_TX_CONTROL),
		 readl(registers + PROXY_RX_CONTROL));
	if (!ret && waking)
		neo_protected_continue();
	return ret;
}
int neo_diag_quiesce_unused_host(struct apple_dcp *dcp)
{
	return neo_diag_unused_host_state(dcp, false);
}
int neo_diag_resume_unused_host(struct apple_dcp *dcp)
{
	return neo_diag_unused_host_state(dcp, true);
}

/*
 * Initialize once when DRM has observed a healthy, active primary DCP.
 * This exercises normal startup without userspace module-parameter arming.
 */
int neo_diag_display_transport_init(struct apple_dcp *dcp)
{
	if (dcp->fw_compat != DCP_FIRMWARE_V_26_6_T8140 || !dcp_has_panel(dcp))
		return 0;
	return neo_protected_transport_start(dcp);
}
