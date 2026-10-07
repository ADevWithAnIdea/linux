// SPDX-License-Identifier: BSD-3-Clause-Clear
/* MT7932 CONNAC2 boot transport, based on documented hardware behavior. */
#include <linux/firmware.h>
#include <linux/debugfs.h>
#include <linux/ktime.h>
#include <linux/pci.h>
#include <linux/unaligned.h>
#include "mt7921.h"
#include "mcu.h"
#include "mt7932_offload.h"
#include "../mt76_connac2_mac.h"

static u8 mt7932_diag_data[1024 * 1024];
static unsigned int mt7932_diag_used = 4;
static DEFINE_SPINLOCK(mt7932_diag_lock);
static struct debugfs_blob_wrapper mt7932_diag_blob = {
	.data = mt7932_diag_data,
	.size = sizeof(mt7932_diag_data),
};

static ssize_t mt7932_connect_write(struct file *file, const char __user *user,
				   size_t count, loff_t *ppos)
{
	struct mt792x_dev *dev = file->private_data;
	struct mt7932_connect_params *params;
	int ret;

	if (count != sizeof(*params) || *ppos)
		return -EINVAL;
	params = memdup_user(user, count);
	if (IS_ERR(params))
		return PTR_ERR(params);
	ret = mt7932_offload_connect(dev, params);
	kfree_sensitive(params);
	return ret ? ret : count;
}

static const struct file_operations mt7932_connect_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = mt7932_connect_write,
	.llseek = noop_llseek,
};

/* Root-only protocol bring-up transport, limited to reversible runtime settings. */
static ssize_t mt7932_runtime_write(struct file *file, const char __user *user,
				   size_t count, loff_t *ppos)
{
	struct mt792x_dev *dev = file->private_data;
	u8 *request;
	u16 cmd;
	int ret;

	if (count < 2 || count > 1538)
		return -EINVAL;
	request = memdup_user(user, count);
	if (IS_ERR(request))
		return PTR_ERR(request);
	cmd = get_unaligned_le16(request);
	switch (cmd) {
	case 0x70: /* Runtime key/value configuration. */
	case 0x6b: /* Queue management. */
	case 0x93: /* Runtime per-channel configuration. */
	case 0x9b: /* Receive filtering. */
	case 0xdc: /* RTS protection. */
	case 0xaf: /* Beacon IE parse result. */
	case 0x60ea: /* Reduced-neighbor report. */
	case 0x47ea: /* DHCP channel privilege release. */
		mt792x_mutex_acquire(dev);
		ret = mt76_mcu_send_msg(&dev->mt76, cmd, request + 2,
				      count - 2, false);
		mt792x_mutex_release(dev);
		break;
	default:
		ret = -EOPNOTSUPP;
	}
	memzero_explicit(request, count);
	kfree(request);
	return ret ? ret : count;
}

static const struct file_operations mt7932_runtime_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = mt7932_runtime_write,
	.llseek = noop_llseek,
};

void mt7932_diag_init(struct mt792x_dev *dev, struct dentry *dir)
{
	debugfs_create_blob("neo_firmware_events", 0400, dir, &mt7932_diag_blob);
	debugfs_create_file("neo_connect_parameters", 0200, dir, dev,
			    &mt7932_connect_fops);
	debugfs_create_file("neo_runtime_command", 0200, dir, dev,
			    &mt7932_runtime_fops);
}

void mt7932_diag_rx(enum mt76_rxq_id q, struct sk_buff *skb)
{
	unsigned long flags;
	u8 *p;

	/* Opaque firmware diagnostics only; never expose normal data-ring traffic. */
	if (q != MT_RXQ_MCU && q != MT_RXQ_MCU_WA && q != MT_RXQ_BAND2_WA)
		return;
	if (skb->len > 2352)
		return;
	spin_lock_irqsave(&mt7932_diag_lock, flags);
	if (skb->len + 12 <= sizeof(mt7932_diag_data) - mt7932_diag_used) {
		p = mt7932_diag_data + mt7932_diag_used;
		put_unaligned_le16(q, p);
		put_unaligned_le16(skb->len, p + 2);
		put_unaligned_le64(ktime_get_boottime_ns(), p + 4);
		if (!skb_copy_bits(skb, 0, p + 12, skb->len)) {
			mt7932_diag_used += 12 + skb->len;
			put_unaligned_le32(mt7932_diag_used, mt7932_diag_data);
		}
	}
	spin_unlock_irqrestore(&mt7932_diag_lock, flags);
}

static unsigned int mt7932_mlme_config;
module_param(mt7932_mlme_config, uint, 0600);
MODULE_PARM_DESC(mt7932_mlme_config, "MT7932 experimental MLME configuration word");
static unsigned int mt7932_host_bss_index = 1;
module_param(mt7932_host_bss_index, uint, 0600);
MODULE_PARM_DESC(mt7932_host_bss_index, "MT7932 host SME BSS index (0=AIS, 1=host-managed client)");

struct mt7932_shared_memory {
	void *cpu[3];
	dma_addr_t dma[3];
};

static void mt7932_shared_release(struct device *dev, void *res)
{
	struct mt7932_shared_memory *mem = res;
	int i;

	for (i = 0; i < ARRAY_SIZE(mem->cpu); i++)
		if (mem->cpu[i])
			dma_free_coherent(dev, SZ_16K, mem->cpu[i], mem->dma[i]);
}

static int mt7932_publish_shared_memory(struct mt792x_dev *dev)
{
	struct mt7932_shared_memory *mem;
	struct device *device = dev->mt76.dev;
	int i;

	mem = devres_find(device, mt7932_shared_release, NULL, NULL);
	if (!mem) {
		mem = devres_alloc(mt7932_shared_release, sizeof(*mem), GFP_KERNEL);
		if (!mem)
			return -ENOMEM;
		for (i = 0; i < ARRAY_SIZE(mem->cpu); i++) {
			mem->cpu[i] = dma_alloc_coherent(device, SZ_16K,
						       &mem->dma[i], GFP_KERNEL);
			if (!mem->cpu[i] || upper_32_bits(mem->dma[i])) {
				mt7932_shared_release(device, mem);
				devres_free(mem);
				return -ENOMEM;
			}
		}
		devres_add(device, mem);
	}
	for (i = 0; i < ARRAY_SIZE(mem->cpu); i++)
		memset(mem->cpu[i], 0, SZ_16K);
	/* Firmware expects a 0x300-byte status block and 0x320-byte descriptors. */
	dma_wmb();
	mt76_wr(dev, 0x93c28, 1);
	mt76_wr(dev, 0x93a38, BIT(1));
	mt76_wr(dev, 0x93a3c, lower_32_bits(mem->dma[0]));
	mt76_wr(dev, 0x93a30, lower_32_bits(mem->dma[1]));
	mt76_wr(dev, 0x93a54, lower_32_bits(mem->dma[2]));
	dev_info(device, "MT7932 shared host DMA buffers published\n");
	return 0;
}

u8 mt7932_host_bss(void)
{
	return min_t(unsigned int, mt7932_host_bss_index, 1);
}

static int mt7932_basic_config(struct mt76_dev *mdev)
{
	/* MT7932 extends the public 12-byte basic configuration to 24 bytes. */
	u8 basic[24] = {
		[0] = 1, [3] = 3, [12] = 1, [14] = 2, [15] = 3,
		[17] = 0x20, [20] = 2,
	};
	u8 mlme[4];
	/* Queue-management startup records observed on this 7932 firmware ABI. */
	u8 qm[3][32] = {
		{ 0x10, 0x11, 2, 0, 3, 0, 7, 0, 0x2f, 0, 2 },
		{ 0xe0, 0x6e, 2, 0, 0x0f, 0, 0xff, 3, 0, 0, 3 },
		{ 0x0f, 0, 1 },
	};
	int ret = mt76_mcu_send_msg(mdev, 2, basic, sizeof(basic), false);
	int i;

	if (ret)
		return ret;
	/* Test host SME operation; the offload configuration bits are undocumented. */
	put_unaligned_le32(mt7932_mlme_config, mlme);
	ret = mt76_mcu_send_msg(mdev, 0x4eea, mlme, sizeof(mlme), false);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(qm); i++) {
		ret = mt76_mcu_send_msg(mdev, 0x6b, qm[i], sizeof(qm[i]), false);
		if (ret)
			return ret;
	}
	return 0;
}

int mt7932_start(struct mt792x_phy *phy)
{
	struct mt76_dev *mdev = phy->mt76->dev;
	u8 dbdc[36] = { [4] = 2, [6] = 36 };
	u8 power[4] = {};
	int ret;

	ret = mt76_mcu_send_msg(mdev, 4, power, sizeof(power), false);
	if (ret)
		return ret;
	ret = mt76_mcu_send_msg(mdev, 0x28, dbdc, sizeof(dbdc), false);
	if (ret)
		return ret;
	ret = mt76_connac_mcu_set_channel_domain(phy->mt76);
	if (ret)
		return ret;
	set_bit(MT76_STATE_RUNNING, &phy->mt76->state);
	return 0;
}

static int mt7932_fill_rlm(struct mt792x_phy *phy,
			 const struct cfg80211_chan_def *ch, u8 bss_idx, u8 *rlm)
{
	if (!ch->chan)
		return -EINVAL;
	rlm[0] = bss_idx;
	rlm[1] = ch->chan->band == NL80211_BAND_2GHZ ? 1 :
		 ch->chan->band == NL80211_BAND_5GHZ ? 2 : 3;
	rlm[2] = ch->chan->hw_value;
	if (ch->width != NL80211_CHAN_WIDTH_20 &&
	    ch->width != NL80211_CHAN_WIDTH_20_NOHT)
		rlm[3] = ch->center_freq1 > ch->chan->center_freq ? 1 : 3;
	rlm[14] = 1;
	rlm[15] = ch->width == NL80211_CHAN_WIDTH_80 ? 1 :
		  ch->width == NL80211_CHAN_WIDTH_160 ? 2 :
		  ch->width == NL80211_CHAN_WIDTH_80P80 ? 3 : 0;
	if (rlm[15])
		rlm[16] = ieee80211_frequency_to_channel(ch->center_freq1);
	if (ch->center_freq2)
		rlm[17] = ieee80211_frequency_to_channel(ch->center_freq2);
	rlm[20] = rlm[21] = hweight8(phy->mt76->antenna_mask);
	return 0;
}

int mt7932_set_channel(struct mt792x_phy *phy, u8 bss_idx)
{
	u8 rlm[22] = {};
	int ret = mt7932_fill_rlm(phy, &phy->mt76->chandef, bss_idx, rlm);

	if (ret)
		return ret;
	return mt76_mcu_send_msg(phy->mt76->dev, 0x19, rlm, sizeof(rlm), false);
}

int mt7932_add_interface(struct mt792x_dev *dev, struct ieee80211_vif *vif,
			struct mt792x_vif *mvif, bool enable)
{
	u8 activate[12] = {}, mac[8] = {};
	int ret;

	if (vif->type != NL80211_IFTYPE_STATION || mvif->bss_conf.mt76.idx > 1)
		return -EOPNOTSUPP;
	/* 0xb8 binds the firmware's AIS SME address; host-owned roles use MUAR. */
	if (enable && !mvif->bss_conf.mt76.idx) {
		mac[0] = mvif->bss_conf.mt76.idx;
		memcpy(mac + 2, vif->addr, ETH_ALEN);
		ret = mt76_mcu_send_msg(&dev->mt76, 0xb8, mac, sizeof(mac), false);
		if (ret)
			return ret;
	}
	activate[0] = mvif->bss_conf.mt76.idx;
	activate[1] = enable;
	/* The P2P client role delegates management frames to the host SME. */
	activate[2] = mvif->bss_conf.mt76.idx ? 1 : 0;
	activate[3] = mvif->bss_conf.mt76.omac_idx;
	memcpy(activate + 4, vif->addr, ETH_ALEN);
	/* Scan-only contexts have no group key or allocated firmware BMC row. */
	activate[10] = 0xff;
	activate[11] = 1;
	ret = mt76_mcu_send_msg(&dev->mt76, 0x11, activate, sizeof(activate), false);
	return ret;
}

int mt7932_bss_update(struct mt792x_dev *dev, struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;
	struct ieee80211_bss_conf *info = &vif->bss_conf;
	const struct cfg80211_chan_def *ch;
	u8 bss[116] = {};
	int ret;

	ch = mvif->bss_conf.mt76.ctx ? &mvif->bss_conf.mt76.ctx->def :
		&dev->mphy.chandef;
	if (!ch->chan)
		return -EINVAL;
	bss[0] = mvif->bss_conf.mt76.idx;
	/* Legacy ENUM_PARAM_MEDIA_STATE: connected=0, disconnected=1. */
	bss[1] = !vif->cfg.assoc;
	bss[3] = vif->cfg.ssid_len;
	memcpy(bss + 4, vif->cfg.ssid, bss[3]);
	if (info->bssid)
		memcpy(bss + 0x24, info->bssid, ETH_ALEN);
	bss[0x2a] = info->qos;
	bss[0x2b] = 1;
	put_unaligned_le16(ch->chan->band == NL80211_BAND_2GHZ ? 0xfff : 0xff0,
			   bss + 0x2c);
	put_unaligned_le16(info->basic_rates <<
			   (ch->chan->band == NL80211_BAND_2GHZ ? 0 : 4), bss + 0x2e);
	bss[0x30] = mvif->wep_sta ? mvif->wep_sta->deflink.wcid.idx + 2 : 0xfe;
	bss[0x34] = ch->chan->band == NL80211_BAND_2GHZ ? 1 : 2;
	bss[0x37] = 0x7f;
	bss[0x3a] = 7;
	ret = mt7932_fill_rlm(&dev->phy, ch, bss[0], bss + 0x44);
	if (ret)
		return ret;
	bss[0x5b] = mvif->bss_conf.mt76.wmm_idx;
	bss[0x5d] = hweight8(dev->mphy.antenna_mask);
	memcpy(bss + 0x60, &info->he_oper.params, 3);
	bss[0x63] = info->he_bss_color.color;
	put_unaligned_le16(info->he_oper.nss_set, bss + 0x64);
	dev_info(dev->mt76.dev, "MT7932 BSS bss=%u media_state=%u ap=%u bmc=%u band=%u channel=%u\n",
		 bss[0], bss[1], bss[0x30], bss[0x3a], bss[0x45], bss[0x46]);
	ret = mt76_mcu_send_msg(&dev->mt76, 0x19, bss + 0x44, 22, false);
	if (ret)
		return ret;
	return mt76_mcu_send_msg(&dev->mt76, 0x12, bss, sizeof(bss), false);
}

int mt7932_sta_update(struct mt792x_dev *dev, struct ieee80211_sta *sta,
		     struct ieee80211_vif *vif, bool enable,
		     enum mt76_sta_info_state state)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;
	struct mt792x_sta *msta;
	struct ieee80211_link_sta *link;
	u8 body[200] = {};
	int idx, band, shift;

	if (!sta)
		return mt7932_bss_update(dev, vif);
	msta = (void *)sta->drv_priv;
	link = &sta->deflink;
	idx = msta->deflink.wcid.idx;
	if (idx > 12)
		return -ENOSPC;
	if (!enable) {
		u8 remove[5] = { 0, idx + 2, mvif->bss_conf.mt76.idx };

		return mt76_mcu_send_msg(&dev->mt76, 0x14, remove, sizeof(remove), false);
	}
	band = mvif->bss_conf.mt76.ctx ? mvif->bss_conf.mt76.ctx->def.chan->band :
		NL80211_BAND_2GHZ;
	shift = band == NL80211_BAND_2GHZ ? 0 : 4;
	body[0] = idx + 2;
	body[1] = mvif->bss_conf.mt76.idx ? 0x42 : 0x41;
	memcpy(body + 2, sta->addr, ETH_ALEN);
	put_unaligned_le16(sta->aid, body + 8);
	put_unaligned_le16(dev->mphy.hw->conf.listen_interval, body + 10);
	body[0xc] = mvif->bss_conf.mt76.idx;
	body[0xd] = band == NL80211_BAND_2GHZ ? 7 : 8;
	put_unaligned_le16(link->supp_rates[band] << shift, body + 0xe);
	put_unaligned_le16(vif->bss_conf.basic_rates << shift, body + 0x10);
	body[0x12] = sta->wme;
	body[0x14] = state == MT76_STA_INFO_STATE_ASSOC ? 2 : 0;
	body[0x17] = 1;
	if (link->ht_cap.ht_supported) {
		body[0xd] |= BIT(4);
		body[0x15] = link->ht_cap.mcs.rx_mask[0];
		memcpy(body + 0x18, link->ht_cap.mcs.rx_mask, 10);
		memcpy(body + 0x22, &link->ht_cap.mcs.rx_highest, 2);
		memcpy(body + 0x24, &link->ht_cap.mcs.tx_params, 1);
		put_unaligned_le16(link->ht_cap.cap, body + 0x28);
		body[0x30] = link->ht_cap.ampdu_factor | link->ht_cap.ampdu_density << 2;
	}
	body[0x32] = to_rcpi(-ewma_rssi_read(&mvif->bss_conf.rssi));
	body[0x33] = body[0x14] == 2;
	body[0x36] = idx;
	body[0x37] = 0xff;
	if (link->vht_cap.vht_supported) {
		body[0xd] |= BIT(5);
		put_unaligned_le32(link->vht_cap.cap, body + 0x38);
		memcpy(body + 0x3c, &link->vht_cap.vht_mcs, 8);
	}
	body[0x44] = 3;
	put_unaligned_le16(0xffff, body + 0x60);
	body[0x7d] = 1;
	if (link->he_cap.has_he) {
		body[0xd] |= BIT(6);
		memcpy(body + 0x88, link->he_cap.he_cap_elem.mac_cap_info, 6);
		memcpy(body + 0x8e, link->he_cap.he_cap_elem.phy_cap_info, 11);
		memcpy(body + 0x9c, &link->he_cap.he_mcs_nss_supp, 12);
		memcpy(body + 0xa8, &link->he_6ghz_capa.capa, 2);
	}
	dev_info(dev->mt76.dev, "MT7932 STA sta=%u wcid=%u bss=%u state=%u\n",
		 body[0], body[0x36], body[0xc], body[0x14]);
	return mt76_mcu_send_msg(&dev->mt76, 0x13, body, sizeof(body), body[0x33]);
}

int mt7932_set_key(struct mt792x_dev *dev, struct ieee80211_vif *vif,
		   struct ieee80211_sta *sta, struct ieee80211_key_conf *key,
		   enum set_key_cmd cmd)
{
	struct mt792x_vif *mvif = (void *)vif->drv_priv;
	struct mt792x_sta *msta = sta ? (void *)sta->drv_priv : &mvif->sta;
	struct mt76_wcid *wcid = &msta->deflink.wcid;
	u8 body[64] = {};
	int ret;

	if (key->cipher != WLAN_CIPHER_SUITE_CCMP || key->keylen != 16)
		return -EOPNOTSUPP;
	body[0] = cmd == SET_KEY;
	body[1] = !!(key->flags & IEEE80211_KEY_FLAG_PAIRWISE);
	body[2] = body[1];
	if (sta)
		memcpy(body + 4, sta->addr, ETH_ALEN);
	else
		eth_broadcast_addr(body + 4);
	body[10] = mvif->bss_conf.mt76.idx;
	body[11] = 4;
	body[12] = key->keyidx;
	body[13] = key->keylen;
	body[14] = wcid->idx;
	memcpy(body + 16, key->key, key->keylen);
	mt792x_mutex_acquire(dev);
	ret = mt76_mcu_send_msg(&dev->mt76, 7, body, sizeof(body), false);
	if (!ret) {
		wcid->hw_key_idx = cmd == SET_KEY ? key->keyidx : -1;
		mt76_wcid_key_setup(&dev->mt76, wcid, cmd == SET_KEY ? key : NULL);
	}
	mt792x_mutex_release(dev);
	memzero_explicit(body, sizeof(body));
	return ret;
}

int mt7932_load_calibration(struct mt792x_dev *dev)
{
	const struct firmware *fw;
	struct sk_buff *reply;
	u8 query[20] = { [3] = 9 };
	char name[80];
	size_t pos;
	u32 count, i;
	int ret;

	ret = mt7932_basic_config(&dev->mt76);
	if (ret)
		return ret;
	snprintf(name, sizeof(name), "mediatek/mt7932/calibration-%pm.bin",
		 dev->mphy.macaddr);
	ret = request_firmware(&fw, name, dev->mt76.dev);
	if (ret)
		return ret;
	ret = -EINVAL;
	if (fw->size < 20 || fw->size > 65536 ||
	    memcmp(fw->data, "MT7932C1", 8) ||
	    memcmp(fw->data + 8, dev->mphy.macaddr, ETH_ALEN))
		goto out;
	count = get_unaligned_le32(fw->data + 16);
	if (!count || count > 128)
		goto out;
	/* Validate the entire replay before provisioning this individual board. */
	for (pos = 20, i = 0; i < count; i++) {
		const u8 *h;
		u32 size;

		if (pos > fw->size || fw->size - pos < 8)
			goto out;
		h = fw->data + pos;
		size = get_unaligned_le32(h + 4);
		pos += 8;
		if (!size || size > 1536 || size > fw->size - pos ||
		    h[2] > 1 || h[3] > 1)
			goto out;
		if (h[0] == 0xed) {
			if (h[1] != 0x21 || size < 4 || size > 1028 ||
			    fw->data[pos] != 3 ||
			    get_unaligned_le16(fw->data + pos + 2) != size - 4)
				goto out;
		} else if ((h[0] != 0xd6 && h[0] != 0xd7) || h[1]) {
			goto out;
		}
		pos += size;
	}
	if (pos != fw->size)
		goto out;
	ret = mt76_mcu_send_and_get_msg(&dev->mt76, 0xbf | __MCU_CMD_FIELD_QUERY,
					query, sizeof(query), true, &reply);
	if (ret)
		goto out;
	if (reply->len < 20 || reply->data[12] != 0xbf) {
		dev_err(dev->mt76.dev, "MT7932 calibration-type query invalid (len %u)\n",
			 reply->len);
		ret = -EINVAL;
	} else {
		dev_info(dev->mt76.dev, "MT7932 calibration profile: %*ph\n", 20, reply->data);
	}
	dev_kfree_skb(reply);
	if (ret)
		goto out;
	for (pos = 20, i = 0; i < count; i++) {
		const u8 *h = fw->data + pos;
		u32 size = get_unaligned_le32(h + 4);
		int cmd = h[0] | (h[1] << 8);

		if (!h[2])
			cmd |= __MCU_CMD_FIELD_QUERY;
		pos += 8;
		ret = mt76_mcu_send_msg(&dev->mt76, cmd, fw->data + pos, size,
				      !h[2] || h[3]);
		if (ret)
			goto out;
		pos += size;
	}
	dev_info(dev->mt76.dev, "MT7932 board calibration uploaded (%u records)\n", count);
out:
	release_firmware(fw);
	return ret;
}

int mt7932_mcu_send_message(struct mt76_dev *mdev, struct sk_buff *skb,
			  int cmd, int *wait_seq)
{
	struct mt792x_dev *dev = container_of(mdev, struct mt792x_dev, mt76);
	struct mt76_connac2_mcu_txd *hdr;
	u8 seq = ++mdev->mcu.msg_seq;
	bool running = test_bit(MT76_STATE_MCU_RUNNING, &dev->mphy.state);

	if (!seq)
		seq = ++mdev->mcu.msg_seq;
	if (wait_seq)
		*wait_seq = seq;
	mdev->mcu.timeout = 6 * HZ;
	if (cmd == MCU_CMD(FW_SCATTER))
		return mt76_tx_queue_skb_raw(dev, mdev->q_mcu[MT_MCUQ_FWDL], skb, 0);
	/* MT7932 uses legacy flat commands; do not send CONNAC UNI TLVs. */
	if (cmd & __MCU_CMD_FIELD_UNI) {
		dev_err(mdev->dev, "MT7932 unsupported UNI command %#x\n", cmd);
		dev_kfree_skb(skb);
		return -EOPNOTSUPP;
	}
	if (running && cmd == MCU_CE_CMD(SET_CHAN_DOMAIN)) {
		u8 *body = skb->data;
		unsigned int count, i;

		if (skb->len < 12) {
			dev_kfree_skb(skb);
			return -EINVAL;
		}
		count = body[8] + body[9] + body[10];
		if (skb->len != 12 + 8 * count) {
			dev_kfree_skb(skb);
			return -EINVAL;
		}
		if (body[0] == '0' && body[1] == '0') {
			body[0] = 'X';
			body[1] = 'Z';
		}
		put_unaligned_le16(1, body + 4);
		body[6] = dev->mphy.cap.has_6ghz ? 2 : 0;
		body[7] = (mdev->region ? BIT(0) | mdev->region << 4 : 0) |
			  (body[10] ? BIT(3) : 0);
		body[11] = mdev->region == NL80211_DFS_FCC ? 0x0f : 0;
		for (i = 0; i < count; i++) {
			u8 *entry = body + 12 + 8 * i;
			u32 flags = get_unaligned_le32(entry + 4);

			/* Legacy firmware uses bit 8 for passive and bit 3 for DFS. */
			flags = (flags & IEEE80211_CHAN_NO_IR ? BIT(8) : 0) |
				(flags & IEEE80211_CHAN_RADAR ? BIT(3) : 0);
			put_unaligned_le32(flags, entry + 4);
		}
	}
	if (running && cmd == MCU_CE_CMD(START_HW_SCAN)) {
		struct mt76_connac_hw_scan_req *req = (void *)skb->data;
		struct sk_buff *wire;
		u8 *body;

		if (skb->len != sizeof(*req)) {
			dev_kfree_skb(skb);
			return -EINVAL;
		}
		wire = mt76_mcu_msg_alloc(mdev, NULL, 1236);
		if (!wire) {
			dev_kfree_skb(skb);
			return -ENOMEM;
		}
		body = skb_put_zero(wire, 1236);
		memcpy(body, req, offsetof(struct mt76_connac_hw_scan_req, ext_ssids));
		memcpy(body + 0x380, req->ext_ssids, sizeof(req->ext_ssids));
		memcpy(body + 0x458, req->bssid, ETH_ALEN);
		memcpy(body + 0x45e, req->random_mac, ETH_ALEN);
		/* Start with a bounded passive/active dwell and no split-scan policy. */
		body[6] &= ~SCAN_FUNC_SPLIT_SCAN;
		body[6] |= BIT(1); /* Disable DBDC scanning on this single-radio setup. */
		body[5] = 0;
		body[7] = 1;
		/* Receive beacons while the active-probe firmware ABI is brought up. */
		body[2] = 0;
		if (req->ssid_type_ext && body[4])
			body[3] = BIT(3); /* Specified SSIDs without a wildcard probe. */
		/* Firmware generates rate and PHY capability IEs itself. Keep the
		 * caller's additional IEs without duplicating those capabilities.
		 */
		{
			u8 *ies = body + 0xe2;
			unsigned int len = get_unaligned_le16(body + 0xe0);
			unsigned int pos = 0, out = 0;

			if (len > 600)
				goto invalid_scan;
			while (pos < len) {
				unsigned int size;
				u8 id;
				bool generated;

				if (len - pos < 2)
					goto invalid_scan;
				id = ies[pos];
				size = 2 + ies[pos + 1];
				if (size > len - pos)
					goto invalid_scan;
				generated = id == WLAN_EID_SSID ||
					id == WLAN_EID_SUPP_RATES ||
					id == WLAN_EID_EXT_SUPP_RATES ||
					id == WLAN_EID_HT_CAPABILITY ||
					id == WLAN_EID_VHT_CAPABILITY ||
					id == WLAN_EID_EXT_CAPABILITY ||
					id == WLAN_EID_SUPPORTED_REGULATORY_CLASSES ||
					(id == WLAN_EID_EXTENSION && size > 2 &&
					 (ies[pos + 2] == WLAN_EID_EXT_HE_CAPABILITY ||
					  ies[pos + 2] == WLAN_EID_EXT_HE_6GHZ_CAPA));
				if (!generated) {
					memmove(ies + out, ies + pos, size);
					out += size;
				}
				pos += size;
			}
			memset(ies + out, 0, 600 - out);
			put_unaligned_le16(out, body + 0xe0);
		}
		goto scan_valid;
invalid_scan:
		dev_kfree_skb(wire);
		dev_kfree_skb(skb);
		return -EINVAL;
scan_valid:
		dev_info(mdev->dev, "MT7932 scan seq=%u bss=%u type=%u mask=%#x channels=%u+%u band=%u\n",
			 body[0], body[1], body[2], body[6], body[0x9f], body[0x33a], body[0x9e]);
		put_unaligned_le16(150, body + 0x9a);
		put_unaligned_le16(10000, body + 0x9c);
		put_unaligned_le16(120, body + 0x33c);
		dev_kfree_skb(skb);
		skb = wire;
	}
	if (running && cmd == MCU_CE_CMD(SET_RATE_TX_POWER)) {
		struct mt76_connac_tx_power_limit_tlv *req = (void *)skb->data;
		struct mt76_connac_sku_tlv *rates;
		struct sk_buff *wire;
		u8 *body;
		int i, j, len;

		if (skb->len < sizeof(*req) || req->n_chan > 8 ||
		    skb->len != sizeof(*req) + req->n_chan * sizeof(*rates)) {
			dev_kfree_skb(skb);
			return -EINVAL;
		}
		len = sizeof(*req) + 122 * req->n_chan;
		wire = mt76_mcu_msg_alloc(mdev, NULL, len);
		if (!wire) {
			dev_kfree_skb(skb);
			return -ENOMEM;
		}
		body = skb_put_zero(wire, len);
		memcpy(body, req, sizeof(*req));
		body[0] = 3;
		body[1] = 0x0a;
		put_unaligned_le16(len, body + 2);
		rates = (void *)(skb->data + sizeof(*req));
		for (i = 0; i < req->n_chan; i++) {
			s8 limit = 20; /* Conservative 10 dBm bring-up ceiling. */

			for (j = 0; j < ARRAY_SIZE(rates[i].pwr_limit); j++)
				limit = min(limit, rates[i].pwr_limit[j]);
			body[sizeof(*req) + 122 * i] = rates[i].channel;
			memset(body + sizeof(*req) + 122 * i + 1, limit, 121);
		}
		dev_kfree_skb(skb);
		skb = wire;
	}
	hdr = (void *)skb_push(skb, sizeof(*hdr));
	memset(hdr, 0, sizeof(*hdr));
	hdr->txd[0] = cpu_to_le32(FIELD_PREP(MT_TXD0_TX_BYTES, skb->len) |
				FIELD_PREP(MT_TXD0_PKT_FMT, MT_TX_TYPE_CMD));
	hdr->txd[1] = cpu_to_le32(FIELD_PREP(MT_TXD1_HDR_FORMAT, MT_HDR_FORMAT_CMD));
	hdr->len = cpu_to_le16(skb->len - sizeof(hdr->txd));
	hdr->cid = FIELD_GET(__MCU_CMD_FIELD_ID, cmd);
	hdr->pkt_type = MCU_PKT_ID;
	hdr->seq = seq;
	if (running) {
		hdr->ext_cid = FIELD_GET(__MCU_CMD_FIELD_EXT_ID, cmd);
		hdr->set_query = !(cmd & __MCU_CMD_FIELD_QUERY) && hdr->cid != 0x8a;
		hdr->ext_cid_ack = !!wait_seq;
	}
	return mt76_tx_queue_skb_raw(dev, mdev->q_mcu[MT_MCUQ_WM], skb, 0);
}
EXPORT_SYMBOL_GPL(mt7932_mcu_send_message);

int mt7932_mcu_parse_response(struct mt76_dev *mdev, int cmd,
			    struct sk_buff *skb, int seq)
{
	struct mt792x_dev *dev = container_of(mdev, struct mt792x_dev, mt76);
	u16 type;
	u8 cid = FIELD_GET(__MCU_CMD_FIELD_ID, cmd);

	if (!skb) {
		dev_err(mdev->dev, "MT7932 command %#x seq %u timed out\n", cmd, seq);
		return -ETIMEDOUT;
	}
	if (cid == 0x10 || cid == 0x8a)
		dev_info(mdev->dev, "MT7932 CID %#x reply len=%u: %*ph\n", cid,
			 skb->len, min_t(unsigned int, skb->len, 48), skb->data);
	if (skb->len < 32)
		return -EINVAL;
	if (skb->data[29] != seq)
		return -EAGAIN;
	type = get_unaligned_le16(skb->data + 26);
	/* The Neo ROM emits packet type 7 in its eight-byte event header. */
	if (type == 0xe000 || (type == 7 &&
	    !test_bit(MT76_STATE_MCU_RUNNING, &dev->mphy.state))) {
		u8 eid = skb->data[28], status;

		if (skb->len < 36)
			return -EINVAL;
		if (eid != (cid == 0x10 ? 4 : cid == 3 ? 2 : 1))
			return -EAGAIN;
		status = skb->data[32];
		skb_pull(skb, 32);
		if (cid == 0x10) {
			dev_info(mdev->dev, "MT7932 patch semaphore status %u\n", status);
			return status;
		}
		if (cid == 3)
			return 0;
		if (status != 1)
			dev_err(mdev->dev, "MT7932 boot CID %#x status %u\n", cid, status);
		return status == 1 ? 0 : -EIO;
	}
	if (type != 0x3800 || skb->len < 36)
		return -EINVAL;
	if (cmd == MCU_CE_QUERY(REG_READ)) {
		if (skb->len < 44)
			return -EINVAL;
		return (int)get_unaligned_le32(skb->data + 40);
	}
	/* A generic result includes the CID; reject unrelated command replies. */
	if (skb->data[28] == 0xfd) {
		if (skb->len < 40)
			return -EINVAL;
		if (skb->data[36] != cid)
			return -EAGAIN;
		if (skb->data[37])
			return -EIO;
	}
	skb_pull(skb, 36);
	return 0;
}
EXPORT_SYMBOL_GPL(mt7932_mcu_parse_response);

static int mt7932_scatter(struct mt792x_dev *dev, const u8 *data, size_t len)
{
	struct mt76_queue *q = dev->mt76.q_mcu[MT_MCUQ_FWDL];
	unsigned long expires;

	while (len) {
		size_t size = min_t(size_t, len, 2048);
		int ret;

		expires = jiffies + 6 * HZ;
		while (READ_ONCE(q->queued) + 1 >= q->ndesc - 1) {
			mt76_queue_tx_cleanup(dev, q, false);
			if (time_after(jiffies, expires)) {
				dev_err(dev->mt76.dev, "MT7932 firmware download ring stalled\n");
				return -ETIMEDOUT;
			}
			usleep_range(100, 200);
		}
		ret = mt76_mcu_send_msg(&dev->mt76, MCU_CMD(FW_SCATTER),
				       data, size, false);
		if (ret)
			return ret;
		mt76_queue_tx_cleanup(dev, q, false);
		data += size;
		len -= size;
	}
	/* Finish consuming this target before issuing the next boot command. */
	expires = jiffies + 6 * HZ;
	while (READ_ONCE(q->queued)) {
		mt76_queue_tx_cleanup(dev, q, false);
		if (time_after(jiffies, expires))
			return -ETIMEDOUT;
		usleep_range(100, 200);
	}
	return 0;
}

static int mt7932_download(struct mt792x_dev *dev, u8 cid, u32 addr,
			  const u8 *data, u32 len)
{
	struct { __le32 addr, len, mode; } req = {
		cpu_to_le32(addr), cpu_to_le32(len), cpu_to_le32(BIT(31)),
	};
	int ret = mt76_mcu_send_msg(&dev->mt76, cid, &req,
				   sizeof(req), true);

	if (ret)
		return ret;
	return mt7932_scatter(dev, data, len);
}

static int mt7932_secure_command(struct mt792x_dev *dev, u8 cid,
				 const void *data, size_t len)
{
	int ret;

	mt76_wr(dev, 0xfe24c, 0x18451807);
	udelay(2);
	if (!mt76_poll_msec(dev, 0x40060, BIT(0), BIT(0), 5000)) {
		ret = -ETIMEDOUT;
		goto restore;
	}
	ret = mt76_mcu_send_msg(&dev->mt76, cid, data, len, true);
	mt76_wr(dev, 0x40260, 1);
restore:
	mt76_wr(dev, 0xfe24c, 0x1845184f);
	return ret;
}

static int mt7932_load_patch(struct mt792x_dev *dev)
{
	const struct mt76_connac2_patch_hdr *hdr;
	const struct firmware *fw;
	u8 sem[4] = { 2 };
	u32 n, i, wifi_sections = 0;
	int ret, tries;

	for (tries = 0; tries < 50; tries++) {
		ret = mt76_mcu_send_msg(&dev->mt76, MCU_CMD(PATCH_SEM_CONTROL),
					sem, sizeof(sem), true);
		if (ret != 1)
			break;
		msleep(100);
	}
	if (ret == 2)
		return 0;
	if (ret != 3)
		dev_err(dev->mt76.dev, "MT7932 patch semaphore request failed: %d\n", ret);
	if (ret != 3)
		return ret < 0 ? ret : -EIO;
	ret = request_firmware(&fw, MT7932_ROM_PATCH, dev->mt76.dev);
	if (ret)
		return ret;
	ret = -EINVAL;
	if (fw->size < sizeof(*hdr))
		goto out;
	hdr = (const void *)fw->data;
	n = be32_to_cpu(hdr->desc.n_region);
	if (!n || n > (fw->size - sizeof(*hdr)) / sizeof(struct mt76_connac2_patch_sec))
		goto out;
	/* Reject malformed sections before sending any of the patch. */
	for (i = 0; i < n; i++) {
		const struct mt76_connac2_patch_sec *sec =
			(const void *)(fw->data + sizeof(*hdr) + i * sizeof(*sec));
		u32 off = be32_to_cpu(sec->offs), len = be32_to_cpu(sec->info.len);

		if (get_unaligned_be32(&sec->info.rsv[0]) != 0x100)
			continue;
		wifi_sections++;
		if (!len || off < sizeof(*hdr) + n * sizeof(*sec) ||
		    off > fw->size || len > fw->size - off ||
		    len > be32_to_cpu(sec->size))
			goto out;
		if (be32_to_cpu(sec->info.sec_key_idx) != 0xffffffff) {
			ret = -EOPNOTSUPP;
			goto out;
		}
	}
	if (!wifi_sections)
		goto out;
	for (i = 0; i < n; i++) {
		const struct mt76_connac2_patch_sec *sec =
			(const void *)(fw->data + sizeof(*hdr) + i * sizeof(*sec));
		u32 off = be32_to_cpu(sec->offs), len = be32_to_cpu(sec->info.len);

		if (get_unaligned_be32(&sec->info.rsv[0]) != 0x100)
			continue;
		ret = mt7932_download(dev, 5, be32_to_cpu(sec->info.addr),
				      fw->data + off, len);
		if (ret)
			goto out;
	}
	{
		u8 finish[4] = {};
		ret = mt7932_secure_command(dev, 7, finish, sizeof(finish));
	}
out:
	release_firmware(fw);
	return ret;
}

static int mt7932_load_ram(struct mt792x_dev *dev)
{
	const struct mt76_connac2_fw_trailer *trailer;
	const struct mt76_connac2_fw_region *regions;
	const struct firmware *fw;
	struct { __le32 option, addr; } start = {};
	size_t data_size, off = 0;
	int ret, i;

	ret = request_firmware(&fw, MT7932_FIRMWARE_WM, dev->mt76.dev);
	if (ret)
		return ret;
	ret = -EINVAL;
	if (fw->size < sizeof(*trailer))
		goto out;
	trailer = (const void *)(fw->data + fw->size - sizeof(*trailer));
	if (!trailer->n_region || trailer->n_region > 10 ||
	    fw->size < sizeof(*trailer) + trailer->n_region * sizeof(*regions))
		goto out;
	data_size = fw->size - sizeof(*trailer) - trailer->n_region * sizeof(*regions);
	regions = (const void *)(fw->data + data_size);
	/* Validate every region before issuing the first download command. */
	for (i = 0; i < trailer->n_region; i++) {
		u32 len = le32_to_cpu(regions[i].len);
		if (off > data_size || len > data_size - off)
			goto out;
		if (regions[i].feature_set & FW_FEATURE_SET_ENCRYPT) {
			ret = -EOPNOTSUPP;
			goto out;
		}
		off += len;
	}
	off = 0;
	for (i = 0; i < trailer->n_region; i++) {
		u32 len = le32_to_cpu(regions[i].len);
		if (!(regions[i].feature_set & FW_FEATURE_NON_DL)) {
			ret = mt7932_download(dev, 1, le32_to_cpu(regions[i].addr),
					      fw->data + off, len);
			if (ret)
				goto out;
		}
		if (regions[i].feature_set & FW_FEATURE_OVERRIDE_ADDR) {
			start.option = cpu_to_le32(FW_START_OVERRIDE);
			start.addr = regions[i].addr;
		}
		off += len;
	}
	ret = mt7932_secure_command(dev, 2, &start, sizeof(start));
out:
	release_firmware(fw);
	return ret;
}

int mt7932_load_firmware(struct mt792x_dev *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev->mt76.dev);
	u32 state;
	int ret, i;

	mt7932_fullmac_reset(dev);
	clear_bit(MT76_STATE_MCU_RUNNING, &dev->mphy.state);
	ret = mt7932_publish_shared_memory(dev);
	if (ret)
		return ret;
	ret = mt7932_load_patch(dev);
	if (ret)
		return ret;
	ret = mt7932_load_ram(dev);
	if (ret)
		return ret;
	for (i = 0; i < 1000; i++) {
		ret = pci_read_config_dword(pdev, 0x48c, &state);
		if (ret || state == ~0U)
			return -EIO;
		if (FIELD_GET(GENMASK(19, 16), state) == 2) {
			set_bit(MT76_STATE_MCU_RUNNING, &dev->mphy.state);
			dev_info(dev->mt76.dev, "MT7932 firmware ready\n");
			return mt76_mcu_send_msg(&dev->mt76, __MCU_CMD_FIELD_CE, NULL, 0, false);
		}
		msleep(5);
	}
	return -ETIMEDOUT;
}
MODULE_FIRMWARE(MT7932_FIRMWARE_WM);
MODULE_FIRMWARE(MT7932_ROM_PATCH);
