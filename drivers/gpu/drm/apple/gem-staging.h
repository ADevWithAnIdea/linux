/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef APPLE_GEM_STAGING_H
#define APPLE_GEM_STAGING_H

#include <drm/drm_gem_dma_helper.h>

struct apple_dcp;
struct dcp_fb_reference;

void apple_drm_retire_scanout(struct apple_dcp *dcp,
			      struct drm_gem_dma_object *obj, u32 swap_id);
void apple_drm_release_fb_reference(struct dcp_fb_reference *entry);

bool apple_drm_gem_is_cpu_import(struct drm_gem_dma_object *obj);
struct drm_gem_dma_object *
apple_drm_gem_create_scanout(struct drm_device *drm, size_t size);
int apple_drm_gem_copy_import(struct drm_gem_dma_object *src,
			      struct drm_gem_dma_object *dst);

#endif
