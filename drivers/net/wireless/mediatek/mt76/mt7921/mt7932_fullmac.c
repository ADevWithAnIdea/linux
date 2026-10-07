// SPDX-License-Identifier: BSD-3-Clause-Clear
/* Native MT7932 Ethernet transport. cfg80211 control is under bring-up. */
#include <linux/etherdevice.h>
#include <linux/unaligned.h>
#include "mt7921.h"
#include "mt7932_offload.h"
#include "../mt76_connac2_mac.h"

struct mt7932_fullmac {
	struct mt792x_dev *dev;
	struct net_device *netdev;
	bool active;
	u8 peer_wcid, cal_band;
	u32 cal_center_khz;
	bool cal_applied;
	u8 bssid[ETH_ALEN];
	struct mt7932_cal_cache *cal;
	struct sk_buff_head events;
	struct work_struct event_work;
};

static void mt7932_fullmac_event_work(struct work_struct *work)
{
	struct mt7932_fullmac *fm = container_of(work, struct mt7932_fullmac, event_work);
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&fm->events))) {
		u8 eid = skb->data[28];
		const u8 *p = skb->data + 36;
		int ret = 0;

		if (eid == 0xd7) {
			ret = mt7932_precal_request(fm->dev, fm->cal, p, skb->len - 36);
		} else if (get_unaligned_le32(p + 4) == 0x4d) {
			ret = mt7932_precal_preload(fm->dev, fm->cal, READ_ONCE(fm->cal_band));
		} else if (!READ_ONCE(fm->cal_applied) &&
			   get_unaligned_le32(p + 4) == 0x41) {
			ret = mt7932_precal_apply(fm->dev, fm->cal,
				READ_ONCE(fm->cal_band), READ_ONCE(fm->cal_center_khz));
			if (!ret)
				WRITE_ONCE(fm->cal_applied, true);
		}
		if (ret)
			dev_warn(fm->dev->mt76.dev, "MT7932 calibration event %#x failed: %d\n", eid, ret);
		dev_kfree_skb(skb);
	}
}

void mt7932_fullmac_connecting(struct mt792x_dev *dev, const u8 *bssid)
{
	struct mt7932_fullmac *fm = READ_ONCE(dev->fullmac);

	if (fm) {
		netif_carrier_off(fm->netdev);
		WRITE_ONCE(fm->peer_wcid, 0xff);
		WRITE_ONCE(fm->cal_applied, false);
		WRITE_ONCE(fm->cal_band, 0);
		WRITE_ONCE(fm->cal_center_khz, 0);
		ether_addr_copy(fm->bssid, bssid);
		/* Discovery management frames must reach the existing RX path. */
		WRITE_ONCE(fm->active, false);
	}
}

void mt7932_fullmac_reset(struct mt792x_dev *dev)
{
	struct mt7932_fullmac *fm = READ_ONCE(dev->fullmac);

	if (!fm)
		return;
	netif_carrier_off(fm->netdev);
	WRITE_ONCE(fm->active, false);
	WRITE_ONCE(fm->peer_wcid, 0xff);
	WRITE_ONCE(fm->cal_applied, false);
	WRITE_ONCE(fm->cal_band, 0);
	WRITE_ONCE(fm->cal_center_khz, 0);
}

int mt7932_fullmac_preload(struct mt792x_dev *dev, u8 band)
{
	struct mt7932_fullmac *fm = READ_ONCE(dev->fullmac);

	return fm ? mt7932_precal_preload(dev, fm->cal, band) : -ENODEV;
}

static int mt7932_native_open(struct net_device *netdev)
{
	netif_start_queue(netdev);
	return 0;
}

static int mt7932_native_stop(struct net_device *netdev)
{
	netif_stop_queue(netdev);
	return 0;
}

static netdev_tx_t mt7932_native_tx(struct sk_buff *skb,
				 struct net_device *netdev)
{
	struct mt7932_fullmac *fm = *(void **)netdev_priv(netdev);
	struct mt76_phy *phy = &fm->dev->mphy;
	struct mt76_dev *mdev = &fm->dev->mt76;
	struct mt76_queue *q = phy->q_tx[MT_TXQ_BE];
	unsigned int bytes = skb->len;
	int ret;

	if (!netif_carrier_ok(netdev) || skb_linearize(skb)) {
		netdev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	memset(skb->cb, 0, sizeof(skb->cb));
	IEEE80211_SKB_CB(skb)->flags = IEEE80211_TX_CTL_HW_80211_ENCAP;
	skb_set_queue_mapping(skb, 0);
	spin_lock_bh(&q->lock);
	if (q->queued >= q->ndesc - 8 || mdev->token_count >= mdev->token_size - 8) {
		spin_unlock_bh(&q->lock);
		return NETDEV_TX_BUSY;
	}
	ret = mdev->queue_ops->tx_queue_skb(phy, q, MT_TXQ_BE, skb, NULL, NULL);
	if (ret >= 0)
		mdev->queue_ops->kick(mdev, q);
	spin_unlock_bh(&q->lock);
	if (ret >= 0) {
		netdev->stats.tx_packets++;
		netdev->stats.tx_bytes += bytes;
	} else {
		netdev->stats.tx_errors++;
	}
	return NETDEV_TX_OK;
}

static const struct net_device_ops mt7932_native_ops = {
	.ndo_open = mt7932_native_open,
	.ndo_stop = mt7932_native_stop,
	.ndo_start_xmit = mt7932_native_tx,
	.ndo_validate_addr = eth_validate_addr,
};

int mt7932_fullmac_init(struct mt792x_dev *dev)
{
	struct mt7932_fullmac *fm;
	struct net_device *netdev;
	int ret;

	fm = kzalloc(sizeof(*fm), GFP_KERNEL);
	if (!fm)
		return -ENOMEM;
	netdev = alloc_etherdev(sizeof(fm));
	if (!netdev) {
		kfree(fm);
		return -ENOMEM;
	}
	fm->dev = dev;
	fm->netdev = netdev;
	INIT_WORK(&fm->event_work, mt7932_fullmac_event_work);
	skb_queue_head_init(&fm->events);
	ret = mt7932_precal_load(dev, &fm->cal);
	if (ret) {
		free_netdev(netdev);
		kfree(fm);
		return ret;
	}
	/* Assigned from the firmware association event before authorizing TX. */
	fm->peer_wcid = 0xff;
	*(void **)netdev_priv(netdev) = fm;
	strscpy(netdev->name, "neo%d", IFNAMSIZ);
	netdev->netdev_ops = &mt7932_native_ops;
	SET_NETDEV_DEV(netdev, dev->mt76.dev);
	eth_hw_addr_set(netdev, dev->mphy.macaddr);
	netif_carrier_off(netdev);
	ret = register_netdev(netdev);
	if (ret) {
		free_netdev(netdev);
		kfree(fm);
		return ret;
	}
	WRITE_ONCE(dev->fullmac, fm);
	dev_info(dev->mt76.dev, "MT7932 experimental native Ethernet interface %s\n",
		 netdev->name);
	return 0;
}

bool mt7932_fullmac_is_tx(struct mt792x_dev *dev, struct sk_buff *skb)
{
	struct mt7932_fullmac *fm = READ_ONCE(dev->fullmac);

	return fm && skb && skb->dev == fm->netdev;
}
EXPORT_SYMBOL_GPL(mt7932_fullmac_is_tx);

void mt7932_fullmac_txwi(struct mt792x_dev *dev, __le32 *txwi,
			struct sk_buff *skb)
{
	struct mt76_wcid wcid = { .idx = dev->fullmac->peer_wcid };

	mt76_connac2_mac_write_txwi(&dev->mt76, txwi, skb, &wcid,
				  NULL, 0, MT_TXQ_BE, 0);
	/* Firmware installed the pairwise key; hardware supplies PN and sequence. */
	txwi[3] |= cpu_to_le32(MT_TXD3_PROTECT_FRAME);
}
EXPORT_SYMBOL_GPL(mt7932_fullmac_txwi);

bool mt7932_fullmac_tx_free(struct mt792x_dev *dev, struct mt76_txwi_cache *t)
{
	if (!mt7932_fullmac_is_tx(dev, t->skb))
		return false;
	mt76_connac_txp_skb_unmap(&dev->mt76, t);
	dev_consume_skb_any(t->skb);
	t->skb = NULL;
	mt76_put_txwi(&dev->mt76, t);
	return true;
}

void mt7932_fullmac_remove(struct mt792x_dev *dev)
{
	struct mt7932_fullmac *fm = READ_ONCE(dev->fullmac);
	struct mt76_txwi_cache *t;
	int id;

	if (!fm)
		return;
	cancel_work_sync(&fm->event_work);
	skb_queue_purge(&fm->events);
	unregister_netdev(fm->netdev);
	/* Caller has quiesced DMA and NAPI before reclaiming native packets. */
	spin_lock_bh(&dev->mt76.token_lock);
	idr_for_each_entry(&dev->mt76.token, t, id) {
		if (!mt7932_fullmac_is_tx(dev, t->skb))
			continue;
		mt76_connac_txp_skb_unmap(&dev->mt76, t);
		dev_kfree_skb_any(t->skb);
		t->skb = NULL;
	}
	spin_unlock_bh(&dev->mt76.token_lock);
	WRITE_ONCE(dev->fullmac, NULL);
	synchronize_net();
	free_netdev(fm->netdev);
	kfree(fm);
}
EXPORT_SYMBOL_GPL(mt7932_fullmac_remove);

void mt7932_fullmac_event(struct mt792x_dev *dev, struct sk_buff *skb)
{
	struct mt7932_fullmac *fm = READ_ONCE(dev->fullmac);
	u32 type;
	struct sk_buff *copy;
	u32 len;

	if (!fm || skb->len < 36)
		return;
	if (skb->data[28] == 0xd9) {
		if (skb->len >= 40)
			dev_info(dev->mt76.dev, "MT7932 precal completion %*ph\n", 4, skb->data + 36);
		return;
	}
	if (skb->data[28] == 0xd7)
		goto queue;
	if (skb->len < 48 || (skb->data[28] != 0xee && skb->data[28] != 0xea))
		return;
	if (get_unaligned_le32(skb->data + 36))
		return;
	len = get_unaligned_le32(skb->data + 44);
	if (len > skb->len - 48)
		return;
	type = get_unaligned_le32(skb->data + 40);
	if (type == 0x4d && len >= 24 &&
	    ether_addr_equal(skb->data + 52, fm->bssid)) {
		u8 band = skb->data[60], center = skb->data[58];
		int frequency;

		if (band < 1 || band > 3)
			return;
		frequency = ieee80211_channel_to_frequency(center,
			band == 1 ? NL80211_BAND_2GHZ :
			band == 2 ? NL80211_BAND_5GHZ : NL80211_BAND_6GHZ);
		WRITE_ONCE(fm->cal_band, band);
		WRITE_ONCE(fm->cal_center_khz, frequency * 1000);
		WRITE_ONCE(fm->active, true);
		/* Queue only the preload-marked records; defer runtime records. */
		goto queue;
	}
	if (type == 0x40 && len >= 22 && !get_unaligned_le32(skb->data + 48) &&
	    ether_addr_equal(skb->data + 53, fm->bssid) && skb->data[69] < 48) {
		WRITE_ONCE(fm->peer_wcid, skb->data[69]);
		dev_info(dev->mt76.dev, "MT7932 native peer STA=%u WCID=%u\n",
			 skb->data[68], skb->data[69]);
	}
	if (type == 0x41 && len >= 20 && fm->peer_wcid < 48 &&
	    ether_addr_equal(skb->data + 52, fm->bssid) &&
	    !get_unaligned_le32(skb->data + 48)) {
		netif_carrier_on(fm->netdev);
		dev_info(dev->mt76.dev, "MT7932 native link authorized\n");
		goto queue;
	} else if (type == 0x44) {
		netif_carrier_off(fm->netdev);
		dev_info(dev->mt76.dev, "MT7932 native link disconnected\n");
	}
	return;
queue:
	if (skb_queue_len(&fm->events) >= 64)
		return;
	copy = skb_copy(skb, GFP_ATOMIC);
	if (!copy)
		return;
	skb_queue_tail(&fm->events, copy);
	schedule_work(&fm->event_work);
}

bool mt7932_fullmac_rx(struct mt792x_dev *dev, struct sk_buff *skb)
{
	struct mt7932_fullmac *fm = READ_ONCE(dev->fullmac);
	struct mt76_rx_status *status = (void *)skb->cb;
	unsigned int bytes;

	if (!fm || !READ_ONCE(fm->active))
		return false;
	if (!(status->flag & RX_FLAG_8023) || skb->len < ETH_HLEN ||
	    !netif_running(fm->netdev) ||
	    status->flag & (RX_FLAG_ONLY_MONITOR | RX_FLAG_FAILED_FCS_CRC)) {
		dev_kfree_skb(skb);
		return true;
	}
	bytes = skb->len;
	skb->dev = fm->netdev;
	memset(skb->cb, 0, sizeof(skb->cb));
	skb->ip_summed = CHECKSUM_NONE;
	skb->protocol = eth_type_trans(skb, fm->netdev);
	fm->netdev->stats.rx_packets++;
	fm->netdev->stats.rx_bytes += bytes;
	netif_rx(skb);
	return true;
}
