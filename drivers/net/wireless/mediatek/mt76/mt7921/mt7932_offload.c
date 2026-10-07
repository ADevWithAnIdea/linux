// SPDX-License-Identifier: BSD-3-Clause-Clear
/* MT7932 firmware MLME and opaque, board-specific calibration cache. */
#include <linux/firmware.h>
#include <linux/etherdevice.h>
#include <linux/unaligned.h>
#include "mt7921.h"
#include "mt7932_offload.h"

/* Observed ABI offsets. Padding and the unclassified security buffer remain zero. */
struct mt7932_connect_command {
	__le32 auth_suite, pairwise_cipher, operation_deadline;
	__le32 reserved0, flags, operation;
	__le32 ssid_len;
	u8 ssid[32];
	__le32 reserved1, password_len, password_format, reserved2;
	u8 password[64];
	u8 reserved3[308];
	__le32 pmf;
	__le32 reserved4, security_buffer_len;
	u8 security_buffer[32], reserved4b[32];
	__le32 allowed_auth;
	u8 reserved5[24];
	__le32 compatibility_policy;
	u8 reserved5b[252];
	__le32 candidate_flags;
	u8 bssid[6], reserved6[6];
	u8 band, channel, reserved7[2];
	u8 candidate_policy[8];
	__le32 reserved8;
	u8 candidate_tail_policy[4];
	u8 reserved9[8];
	__le32 candidates;
} __packed;

static_assert(sizeof(struct mt7932_connect_command) == 856);
static_assert(sizeof(struct mt7932_connect_params) == 106);
static_assert(offsetof(struct mt7932_connect_command, ssid_len) == 24);
static_assert(offsetof(struct mt7932_connect_command, ssid) == 28);
static_assert(offsetof(struct mt7932_connect_command, password_len) == 64);
static_assert(offsetof(struct mt7932_connect_command, password) == 76);
static_assert(offsetof(struct mt7932_connect_command, pmf) == 448);
static_assert(offsetof(struct mt7932_connect_command, security_buffer_len) == 456);
static_assert(offsetof(struct mt7932_connect_command, security_buffer) == 460);
static_assert(offsetof(struct mt7932_connect_command, allowed_auth) == 524);
static_assert(offsetof(struct mt7932_connect_command, bssid) == 812);
static_assert(offsetof(struct mt7932_connect_command, band) == 824);
static_assert(offsetof(struct mt7932_connect_command, candidates) == 852);

int mt7932_offload_connect(struct mt792x_dev *dev,
			   const struct mt7932_connect_params *p)
{
	struct mt7932_connect_command *cmd;
	u8 mac[8] = {}, profile[4] = { 7, 4, 2, 0 };
	u8 basic[24] = { [3] = 3, [12] = 1, [14] = 2, [15] = 3,
			 [17] = 0x20, [20] = 2 };
	int ret;

	if (!p->ssid_len || p->ssid_len > sizeof(p->ssid) ||
	    !p->password_len || p->password_len > sizeof(p->password) ||
	    !is_valid_ether_addr(p->bssid) || p->band < 1 || p->band > 3 ||
	    !p->channel || (p->band == 1 && p->channel > 14) ||
	    (p->band == 3 && (p->channel > 233 || (p->channel - 1) % 4)))
		return -EINVAL;
	cmd = kzalloc(sizeof(*cmd), GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;
	/* SAE password offload, CCMP, mandatory PMF; no host-supplied PMK. */
	cmd->auth_suite = cpu_to_le32(BIT(10));
	cmd->pairwise_cipher = cpu_to_le32(BIT(4));
	cmd->operation_deadline = cpu_to_le32(120);
	cmd->flags = cpu_to_le32(BIT(16));
	cmd->operation = cpu_to_le32(8);
	cmd->ssid_len = cpu_to_le32(p->ssid_len);
	memcpy(cmd->ssid, p->ssid, p->ssid_len);
	cmd->password_len = cpu_to_le32(p->password_len);
	/* This field is 10 for the observed password encoding; it is not
	 * gen4m's AUTH_MODE enum (SAE is 11 in that separate namespace).
	 */
	cmd->password_format = cpu_to_le32(10);
	memcpy(cmd->password, p->password, p->password_len);
	cmd->pmf = cpu_to_le32(2);
	/* No cached security value for fresh SAE. Its role in the captured
	 * request is unclassified. An absent buffer, zero buffer and newly
	 * derived PSK are being checked independently of radio controls.
	 */
	/* Observed firmware authentication policy. Bit 2's meaning is pending
	 * independent verification; this is not the public gen4m auth enum.
	 */
	cmd->allowed_auth = cpu_to_le32(BIT(30) | BIT(2));
	/* Compatibility fields under investigation. These contain public
	 * policy/PHY information, not any network identity or secret. Keep
	 * them explicit so individual fields can be tested independently.
	 */
	cmd->compatibility_policy = cpu_to_le32(20);
	cmd->candidate_policy[0] = 1;
	cmd->candidate_policy[1] = 2;
	cmd->candidate_policy[2] = 4;
	cmd->candidate_policy[4] = 6;
	cmd->candidate_policy[5] = 7;
	cmd->candidate_tail_policy[0] = 4;
	cmd->candidate_tail_policy[1] = 16;
	cmd->candidate_tail_policy[2] = 18;
	cmd->candidate_tail_policy[3] = 20;
	memcpy(cmd->bssid, p->bssid, ETH_ALEN);
	cmd->band = p->band;
	cmd->channel = p->channel;
	cmd->candidates = cpu_to_le32(1);

	mt792x_mutex_acquire(dev);
	ret = mt7932_start(&dev->phy);
	if (!ret)
		ret = mt76_mcu_send_msg(&dev->mt76, 2, basic, sizeof(basic), false);
	if (!ret)
		ret = mt76_mcu_send_msg(&dev->mt76, 0x4eea, profile, sizeof(profile), false);
	memcpy(mac + 2, dev->mphy.macaddr, ETH_ALEN);
	if (!ret)
		ret = mt76_mcu_send_msg(&dev->mt76, 0xb8, mac, sizeof(mac), false);
	if (!ret) {
		mt7932_fullmac_connecting(dev, p->bssid);
		ret = mt76_mcu_send_msg(&dev->mt76, 0x40ea, cmd, sizeof(*cmd), false);
	}
	mt792x_mutex_release(dev);
	kfree_sensitive(cmd);
	return ret;
}

/* On disk: MT7932P1, MAC[6], LE16 record count, LE32 total size, records.
 * Each record is calibration metadata followed by opaque RF data. There are
 * no MCU descriptors, sequence numbers, commands or connection credentials.
 */
struct mt7932_cal_record {
	u8 class, operation;
	__le16 id;
	__le32 len, selector, frequency, control;
	u8 data[];
} __packed;
static_assert(sizeof(struct mt7932_cal_record) == 20);

struct mt7932_cal_cache {
	unsigned int count;
	u8 data[];
};

int mt7932_precal_load(struct mt792x_dev *dev, struct mt7932_cal_cache **out)
{
	struct mt7932_cal_cache *cache;
	const struct firmware *fw;
	char name[80];
	size_t pos;
	u16 count;
	int i, ret;

	*out = NULL;
	snprintf(name, sizeof(name), "mediatek/mt7932/precal-%pm.bin", dev->mphy.macaddr);
	ret = request_firmware(&fw, name, dev->mt76.dev);
	if (ret)
		return ret;
	ret = -EINVAL;
	if (fw->size < 20 || fw->size > 409600 ||
	    memcmp(fw->data, "MT7932P1", 8) ||
	    !ether_addr_equal(fw->data + 8, dev->mphy.macaddr) ||
	    get_unaligned_le32(fw->data + 16) != fw->size)
		goto out;
	count = get_unaligned_le16(fw->data + 14);
	if (!count || count > 128)
		goto out;
	for (i = 0, pos = 20; i < count; i++) {
		const struct mt7932_cal_record *r;
		u32 len;

		if (fw->size - pos < sizeof(*r))
			goto out;
		r = (const void *)(fw->data + pos);
		len = le32_to_cpu(r->len);
		if (r->operation || r->class < 1 || r->class > 3 ||
		    len != (r->class == 1 ? 1080 : r->class == 2 ? 604 : 16) ||
		    len > fw->size - pos - sizeof(*r))
			goto out;
		pos += sizeof(*r) + len;
	}
	if (pos != fw->size)
		goto out;
	cache = devm_kzalloc(dev->mt76.dev, sizeof(*cache) + fw->size - 20, GFP_KERNEL);
	if (!cache) {
		ret = -ENOMEM;
		goto out;
	}
	cache->count = count;
	memcpy(cache->data, fw->data + 20, fw->size - 20);
	*out = cache;
	dev_info(dev->mt76.dev, "MT7932 board precal cache loaded (%u records)\n", count);
	ret = 0;
out:
	release_firmware(fw);
	return ret;
}

static int mt7932_precal_send(struct mt792x_dev *dev,
			    const struct mt7932_cal_record *r)
{
	struct mt7932_cal_record *req;
	u32 len = le32_to_cpu(r->len);
	int ret;

	req = kzalloc(sizeof(*req) + len, GFP_KERNEL);
	if (!req)
		return -ENOMEM;
	req->class = r->class;
	req->id = r->id;
	req->len = cpu_to_le32(len);
	req->selector = r->selector;
	req->frequency = r->frequency;
	req->control = r->control;
	memcpy(req->data, r->data, len);
	/* Completion is an unsolicited D9, never a sequence-number reply. */
	ret = mt76_mcu_send_msg(&dev->mt76, 0xd6, req, sizeof(*req) + len, false);
	kfree(req);
	return ret;
}

int mt7932_precal_preload(struct mt792x_dev *dev, struct mt7932_cal_cache *cache,
			   u8 band)
{
	const struct mt7932_cal_record *r;
	size_t pos = 0;
	int i, sent = 0, ret = 0;

	if (!cache)
		return -ENOENT;
	mt792x_mutex_acquire(dev);
	for (i = 0; i < cache->count; i++) {
		u32 selector;

		r = (const void *)(cache->data + pos);
		pos += sizeof(*r) + le32_to_cpu(r->len);
		selector = le32_to_cpu(r->selector);
		if (band != 3 || r->class != 1 || (selector & 0xffff) != 28 ||
		    !(selector >> 16))
			continue;
		ret = mt7932_precal_send(dev, r);
		if (ret)
			break;
		sent++;
	}
	mt792x_mutex_release(dev);
	dev_info(dev->mt76.dev, "MT7932 group precal preload band=%u records=%d result=%d\n",
		 band, sent, ret);
	return ret ?: (sent ? 0 : -ENOENT);
}

int mt7932_precal_apply(struct mt792x_dev *dev, struct mt7932_cal_cache *cache,
			 u8 band, u32 center_khz)
{
	const struct mt7932_cal_record *r;
	size_t pos = 0;
	int i, sent = 0, ret = 0;

	if (!cache)
		return -ENOENT;
	mt792x_mutex_acquire(dev);
	for (i = 0; i < cache->count; i++) {
		u32 frequency, group;

		r = (const void *)(cache->data + pos);
		pos += sizeof(*r) + le32_to_cpu(r->len);
		frequency = le32_to_cpu(r->frequency) & GENMASK(23, 0);
		group = le32_to_cpu(r->selector) & 0xffff;
		/* Preload-marked group records were submitted before connect. */
		if (r->class == 1 && le32_to_cpu(r->selector) >> 16)
			continue;
		/* This bank covers the observed 6 GHz group 28 only. Never apply
		 * another channel's DPD data to an unsupported frequency.
		 */
		if (band != 3 || (r->class == 1 ? group != 28 : frequency != center_khz))
			continue;
		ret = mt7932_precal_send(dev, r);
		if (ret)
			break;
		sent++;
	}
	mt792x_mutex_release(dev);
	dev_info(dev->mt76.dev, "MT7932 precal apply band=%u center=%u kHz records=%d result=%d\n",
		 band, center_khz, sent, ret);
	return ret ?: (sent ? 0 : -ENOENT);
}

int mt7932_precal_request(struct mt792x_dev *dev, struct mt7932_cal_cache *cache,
			   const u8 *request, size_t len)
{
	const struct mt7932_cal_record *r;
	size_t pos = 0;
	int i, ret = -ENOENT;

	if (!cache || len < 16)
		return -EINVAL;
	/* The D7 key split is still undocumented. Match all three words
	 * exactly, rather than guessing a frequency or returning a wrong block.
	 */
	mt792x_mutex_acquire(dev);
	for (i = 0; i < cache->count; i++) {
		r = (const void *)(cache->data + pos);
		pos += sizeof(*r) + le32_to_cpu(r->len);
		if (memcmp(&r->selector, request + 4, 12))
			continue;
		ret = mt7932_precal_send(dev, r);
		if (ret)
			break;
	}
	mt792x_mutex_release(dev);
	return ret;
}
