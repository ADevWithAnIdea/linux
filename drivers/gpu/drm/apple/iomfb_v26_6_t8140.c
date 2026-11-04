// SPDX-License-Identifier: GPL-2.0-only
/* Copyright The Asahi Linux Contributors */

#include <linux/build_bug.h>

#include "iomfb_v12_3.h"
#include "iomfb_v13_3.h"
#include "iomfb_v14_7.h"
#include "iomfb_v26_6.h"
#include "iomfb_v26_6_t8140.h"
#include "version_utils.h"

static const struct dcp_method_entry dcp_methods[dcpep_num_methods] = {
	IOMFB_METHOD("A000", dcpep_late_init_signal),
	IOMFB_METHOD("A025", dcpep_setup_video_limits),
	IOMFB_METHOD("A131", iomfbep_a131_pmu_service_matched),
	IOMFB_METHOD("A132", iomfbep_a132_backlight_service_matched),
	IOMFB_METHOD("A385", dcpep_set_create_dfb),
	IOMFB_METHOD("A386", iomfbep_a358_vi_set_temperature_hint),
	IOMFB_METHOD("A401", dcpep_start_signal),
	IOMFB_METHOD("A406", dcpep_swap_start),
	IOMFB_METHOD("A408", dcpep_swap_submit),
	IOMFB_METHOD("A411", dcpep_set_display_device),
	IOMFB_METHOD("A412", dcpep_is_main_display),
	IOMFB_METHOD("A413", dcpep_set_digital_out_mode),
	IOMFB_METHOD("A423", iomfbep_set_matrix),
	IOMFB_METHOD("A427", iomfbep_get_color_remap_mode),
	IOMFB_METHOD("A442", dcpep_set_parameter_dcp),
	IOMFB_METHOD("A446", dcpep_create_default_fb),
	IOMFB_METHOD("A450", dcpep_enable_disable_video_power_savings),
	IOMFB_METHOD("A457", dcpep_first_client_open),
	IOMFB_METHOD("A458", iomfbep_last_client_close),
	IOMFB_METHOD("A465", dcpep_set_display_refresh_properties),
	IOMFB_METHOD("A468", dcpep_flush_supports_power),
	IOMFB_METHOD("A469", iomfbep_abort_swaps_dcp),
	IOMFB_METHOD("A472", iomfbep_update_dfb),
	IOMFB_METHOD("A473", dcpep_set_power_state),
};

#define DCP_FW v26_6_t8140
#define DCP_FW_T8140 1
#define DCP_FW_VER DCP_FW_VERSION(26, 6, 0)

#include "iomfb_template.c"

int iomfb_panel_prepare_idle_v26_6_t8140(struct apple_dcp *dcp)
{
	static const struct dcp_method_entry prepare_idle = {
		.name = "dcpep_panel_prepare_idle",
		.tag = { 'A', '5', '0', '0' },
	};
	/*
	 * Captured internal-panel sleep request; the last two bytes are padding.
	 * It follows A473(0) and precedes the retained RTKit idle handshake.
	 */
	const u8 request[4] = { 0, 0, 0xaa, 0xaa };
	struct dcp_wait_cookie *cookie;
	unsigned long remaining;

	cookie = kzalloc(sizeof(*cookie), GFP_KERNEL);
	if (!cookie)
		return -ENOMEM;
	init_completion(&cookie->done);
	kref_init(&cookie->refcount);
	kref_get(&cookie->refcount);
	dcp_push(dcp, false, &prepare_idle, sizeof(request), 0, (void *)request,
		 dcp_on_final, cookie);
	remaining = wait_for_completion_timeout(&cookie->done,
						msecs_to_jiffies(1000));
	kref_put(&cookie->refcount, release_wait_cookie);
	return remaining ? 0 : -ETIMEDOUT;
}

struct dcp_dict_passthrough_req_v26_6 {
	u8 key[0x40];
	u8 value[0x1000];
	u8 value_null;
	u8 padding[3];
} __packed;

struct dcp_dict_passthrough_resp_v26_6 {
	u8 value[0x1000];
	u8 ret;
	u8 padding[3];
} __packed;

/*
 * D114 periodically publishes dictionary-valued DCP statistics.  The value
 * is an in/out pointer: native macOS returns the complete serialized buffer
 * byte-for-byte and reports false.  Returning an all-zero output is not a
 * harmless false result because DCP deserializes the in/out value first.
 */
static bool trampoline_passthrough_dict_v26_6(struct apple_dcp *dcp, int tag,
					       void *out, void *in)
{
	const struct dcp_dict_passthrough_req_v26_6 *req = in;
	struct dcp_dict_passthrough_resp_v26_6 *resp = out;

	trace_iomfb_callback(dcp, tag, "dcpep_cb_passthrough_dict");

	if (!req->value_null)
		memcpy(resp->value, req->value, sizeof(resp->value));

	resp->ret = false;
	memset(resp->padding, 0, sizeof(resp->padding));

	return true;
}

static_assert(sizeof(struct dcp_swap_v26_6_t8140) == 0x588);
static_assert(offsetof(struct dcp_swap_v26_6_t8140, bl_update) == 0x354);
static_assert(offsetof(struct dcp_swap_v26_6_t8140, bl_flags) == 0x358);
static_assert(offsetof(struct dcp_swap_v26_6_t8140, bl_nits) == 0x35e);
static_assert(offsetof(struct dcp_swap_v26_6_t8140, bl_limit_nits) == 0x376);
static_assert(offsetof(struct dcp_swap_v26_6_t8140, bl_scale) == 0x37e);
static_assert(offsetof(struct dcp_swap_v26_6_t8140, bl_scale_inverse) == 0x3e6);
static_assert(sizeof(struct dcp_swap_start_req_v26_6_t8140) == 0x10);
static_assert(sizeof(struct dcp_swap_start_resp_v26_6_t8140) == 0x8);
static_assert(sizeof(struct dcp_swap_submit_req_v26_6_t8140) == 0xe9c);
static_assert(offsetof(struct dcp_swap_submit_req_v26_6_t8140, surf_iova) == 0xe38);
static_assert(sizeof(struct dcp_swap_submit_resp_v26_6_t8140) == 0xc);
static_assert(sizeof(struct dc_swap_complete_resp_v26_6_t8140) == 0x730);
static_assert(sizeof(struct dcp_map_buf_resp_v26_6_t8140) == 0x10);
static_assert(sizeof(struct dcp_set_power_state_req_v26_6) == 0x10);
static_assert(sizeof(struct dcp_set_parameter_dcp) == 0x28);
static_assert(sizeof(struct dcp_get_mode_info_req_v26_6) == 0x7c);
static_assert(sizeof(struct dcp_get_mode_info_resp_v26_6) == 0x74);
static_assert(sizeof(struct iomfb_abort_swaps_dcp_req_v26_6) == 0x8);
static_assert(sizeof(struct iomfb_abort_swaps_dcp_resp_v26_6) == 0x4);
static_assert(sizeof(struct dcp_swap_complete_intent_gated_v26_6) == 0x14);
static_assert(sizeof(struct dcp_hotplug_req_v26_6) == 0x58);
static_assert(sizeof(struct dcp_hotplug_resp_v26_6) == 0x4c);
static_assert(sizeof(struct iomfb_property) == 0x10);
static_assert(sizeof(struct dcp_read_edt_data_resp_v26_6) == 0x24);
static_assert(sizeof(struct dcp_bool_arg) == 0x4);
static_assert(sizeof(struct dcp_set_frame_sync_props_req_v26_6_0) == 0x54);
static_assert(sizeof(struct dcp_set_frame_sync_props_resp_v26_6_0) == 0x50);
static_assert(sizeof(struct dcp_dict_passthrough_req_v26_6) == 0x1044);
static_assert(sizeof(struct dcp_dict_passthrough_resp_v26_6) == 0x1004);

/* D003 on T8140 takes a display index and returns two MMIO addresses.
 * Unlike T8132, it has no leading opaque word or trailing reserved array. */
struct dcp_rt_bandwidth_t8140 {
	u64 scratch;
	u64 doorbell;
	u32 bit;
} __packed;
static_assert(sizeof(struct dcp_rt_bandwidth_t8140) == 20);

static bool trampoline_rt_bandwidth_t8140(struct apple_dcp *dcp, int tag,
					 void *out, void *in)
{
	struct dcp_rt_bandwidth_t8140 *resp = out;

	trace_iomfb_callback(dcp, tag, "rt_bandwidth_t8140");
	memset(resp, 0, sizeof(*resp));
	if (!dcp_disable_rt_bandwidth) {
		resp->scratch = dcp->disp_registers[dcp->disp_bw_scratch_index]->start +
				dcp->disp_bw_scratch_offset;
		resp->doorbell = dcp->disp_registers[dcp->disp_bw_doorbell_index]->start;
	}
	return true;
}

/* Preserve the verified zero-valued reply to this metadata query. */
static bool trampoline_power_up_dart_t8140(struct apple_dcp *dcp, int tag,
					  void *out, void *in)
{
	put_unaligned_le64(0, out);
	return true;
}

static bool trampoline_scanout_clock_t8140(struct apple_dcp *dcp, int tag,
					   void *out, void *in)
{
	u8 enable = *(u8 *)in;
	int ret = enable <= 1 ? 0 : -EINVAL;

	trace_iomfb_callback(dcp, tag, "power_up_scanout_dart_clock");
	put_unaligned_le32(ret, out);
	return true;
}

/*
 * D400 is a provider-property copy: 3072 output bytes plus u32 length.
 * Serve exact data from the owned diagnostic DT child. Never synthesize
 * calibration values or replay native uninitialized output padding.
 */
struct neo_provider_property_request {
	u8 object[4];
	char key[64];
	u32 capacity;
	u8 value_null, length_null, padding[2];
} __packed;
struct neo_provider_property_reply {
	u8 value[3072];
	u32 length;
} __packed;
static_assert(sizeof(struct neo_provider_property_request) == 76);
static_assert(sizeof(struct neo_provider_property_reply) == 3076);
static bool trampoline_provider_property_t8140(struct apple_dcp *dcp, int tag,
					       void *out, void *in)
{
	const struct neo_provider_property_request *req = in;
	struct neo_provider_property_reply *resp = out;
	struct device_node *provider;
	const void *value;
	int size = 0;

	trace_iomfb_callback(dcp, tag, "get_provider_property_from_dt");
	if (memcmp(req->object, "VORP", 4) ||
	    !memchr(req->key, 0, sizeof(req->key)) ||
	    req->capacity > sizeof(resp->value) || req->value_null ||
	    req->length_null)
		return true;
	provider = of_get_child_by_name(dcp->dev->of_node,
					"apple,provider-properties");
	if (!provider)
		return true;
	value = of_get_property(provider, req->key, &size);
	if (value && size > 0 && size <= req->capacity) {
		memcpy(resp->value, value, size);
		resp->length = size;
	}
	dev_info(dcp->dev,
		 "private provider property %s capacity=%u returned=%u\n",
		 req->key, req->capacity, resp->length);
	of_node_put(provider);
	return true;
}

static bool trampoline_hotplug_notify_t8140(struct apple_dcp *dcp, int tag,
					    void *out, void *in)
{
	trace_iomfb_callback(dcp, tag, "hotplug_notification_preserve_output");
	dcpep_cb_hotplug_state(dcp, get_unaligned_le64(in));
	return true;
}

static const iomfb_cb_handler cb_handlers[IOMFB_MAX_CB] = {
	[0] = trampoline_native_boot_capability,
	[1] = trampoline_true,
	[2] = trampoline_nop,
	[3] = trampoline_rt_bandwidth_t8140,
	[6] = trampoline_set_frame_sync_props,
	[100] = iomfbep_cb_match_pmu_service,
	[101] = trampoline_zero,
	[102] = trampoline_nop,
	[104] = trampoline_nop,
	[107] = trampoline_nop,
	[108] = trampoline_true,
	[109] = trampoline_true,
	[110] = trampoline_true,
	[111] = trampoline_true,
	[112] = trampoline_create_backlight_service,
	[113] = trampoline_true,
	[114] = trampoline_passthrough_dict_v26_6,
	/* set_tiling_state: 26.6 returns the value followed by success. */
	[115] = trampoline_get_tiling_state,
	[121] = dcpep_cb_boot_1,
	[122] = trampoline_false, /* native display sleep/wake query */
	[123] = trampoline_false,
	[125] = trampoline_read_edt_data,
	[127] = trampoline_prop_start,
	[128] = trampoline_prop_chunk,
	[129] = trampoline_prop_end,
	[134] = trampoline_zero,
	[201] = trampoline_map_piodma,
	[206] = iomfbep_cb_match_pmu_service_2,
	[207] = iomfbep_cb_match_backlight_service,
	[208] = trampoline_nop,
	[209] = trampoline_get_time,
	[300] = trampoline_pr_publish,
	[400] = trampoline_provider_property_t8140,
	[401] = trampoline_get_uint_prop,
	[406] = trampoline_nop, /* set_fx_prop: host property notification */
	[408] = trampoline_get_frequency,
	[411] = trampoline_map_reg,
	[413] = trampoline_true,
	[414] = trampoline_sr_set_property_int,
	[415] = trampoline_true,
	[451] = trampoline_allocate_buffer,
	[552] = trampoline_true,
	[561] = trampoline_true,
	[563] = trampoline_true,
	[565] = trampoline_true,
	[567] = trampoline_true,
	/* Native 26.6 returns a zero status for both start and stop events. */
	[572] = trampoline_scanout_clock_t8140,
	[574] = trampoline_power_up_dart_t8140,
	[575] = trampoline_hotplug_notify_t8140,
	[576] = trampoline_nop,
	[581] = trampoline_nop,
	[582] = trampoline_create_default_fb, /* create_default_fb_surface */
	[590] = trampoline_swap_complete,
	[592] = trampoline_swap_complete_intent_gated,
	[594] = trampoline_nop, /* native display re-enable notification: empty ACK */
	[595] = trampoline_nop,
	[598] = trampoline_nop,
	[599] = trampoline_nop,
};

void DCP_FW_NAME(iomfb_start)(struct apple_dcp *dcp)
{
	dcp->cb_handlers = cb_handlers;
	dcp_start_signal(dcp, false, dcp_started, NULL);
}

#undef DCP_FW_T8140
#undef DCP_FW_VER
#undef DCP_FW
