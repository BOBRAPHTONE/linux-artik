/*
 * Copyright (C) 2016  Nexell Co., Ltd.
 * Author: junghyun, kim <jhkim@nexell.co.kr>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <drm/drmP.h>
#include <drm/drm_crtc_helper.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/of_address.h>

#include "nx_drm_drv.h"
#include "nx_drm_crtc.h"
#include "nx_drm_plane.h"
#include "nx_drm_gem.h"
#include "soc/s5pxx18_drm_dp.h"

static void nx_drm_crtc_dpms(struct drm_crtc *crtc, int mode)
{
	struct drm_device *drm = crtc->dev;
	struct nx_drm_crtc *nx_crtc = to_nx_crtc(crtc);

	if (nx_crtc->dpms_mode == mode) {
		DRM_DEBUG_KMS("dpms %d same as previous one.\n", mode);
		return;
	}

	mutex_lock(&drm->struct_mutex);

	switch (mode) {
	case DRM_MODE_DPMS_ON:
		nx_crtc->dpms_mode = mode;
		break;
	case DRM_MODE_DPMS_STANDBY:
	case DRM_MODE_DPMS_SUSPEND:
	case DRM_MODE_DPMS_OFF:
		nx_crtc->dpms_mode = mode;
		break;
	default:
		DRM_ERROR("fail : unspecified mode %d\n", mode);
		goto err_dpms;
	}

	nx_drm_dp_crtc_dpms(crtc, mode);

err_dpms:
	mutex_unlock(&drm->struct_mutex);
}

static void nx_drm_crtc_prepare(struct drm_crtc *crtc)
{
	/* drm framework doesn't check NULL. */
}

static void nx_drm_crtc_commit(struct drm_crtc *crtc)
{
	struct nx_drm_crtc *nx_crtc = to_nx_crtc(crtc);

	/*
	 * when set_crtc is requested from user or at booting time,
	 * crtc->commit would be called without dpms call so if dpms is
	 * no power on then crtc->dpms should be called
	 * with DRM_MODE_DPMS_ON for the hardware power to be on.
	 */
	if (nx_crtc->dpms_mode != DRM_MODE_DPMS_ON) {
		int mode = DRM_MODE_DPMS_ON;
		/*
		 * enable hardware(power on) to all encoders hdmi connected
		 * to current crtc.
		 */
		nx_drm_crtc_dpms(crtc, mode);
	}

	nx_drm_dp_crtc_commit(crtc);
}

static bool nx_drm_crtc_mode_fixup(struct drm_crtc *crtc,
			const struct drm_display_mode *mode,
			struct drm_display_mode *adjusted_mode)
{
	return true;
}

static int nx_drm_crtc_mode_set(struct drm_crtc *crtc,
			struct drm_display_mode *mode,
			struct drm_display_mode *adjusted_mode,
			int x, int y,
			struct drm_framebuffer *old_fb)
{
	struct drm_framebuffer *fb = crtc->primary->fb;
	unsigned int crtc_w;
	unsigned int crtc_h;

	DRM_DEBUG_KMS("enter\n");

	/*
	 * copy the mode data adjusted by mode_fixup() into crtc->mode
	 * so that hardware can be seet to proper mode.
	 */
	memcpy(&crtc->mode, adjusted_mode, sizeof(*adjusted_mode));

	crtc_w = fb->width - x;
	crtc_h = fb->height - y;

	return nx_drm_dp_crtc_mode_set(crtc,
				crtc->primary, crtc->primary->fb,
				0, 0, crtc_w, crtc_h, x, y, crtc_w, crtc_h);
}

static int nx_drm_crtc_mode_set_base(struct drm_crtc *crtc, int x, int y,
			struct drm_framebuffer *old_fb)
{
	struct drm_framebuffer *fb = crtc->primary->fb;
	struct nx_drm_crtc *nx_crtc = to_nx_crtc(crtc);
	unsigned int crtc_w;
	unsigned int crtc_h;

	/* when framebuffer changing is requested, crtc's dpms should be on */
	if (nx_crtc->dpms_mode > DRM_MODE_DPMS_ON) {
		DRM_ERROR("fail : framebuffer changing request.\n");
		return -EPERM;
	}

	crtc_w = fb->width - x;
	crtc_h = fb->height - y;

	return nx_drm_dp_plane_update(crtc->primary, fb, 0, 0,
				   crtc_w, crtc_h, x, y, crtc_w, crtc_h);
}

static void nx_drm_crtc_disable(struct drm_crtc *crtc)
{
	struct drm_plane *plane;
	int ret;

	nx_drm_crtc_dpms(crtc, DRM_MODE_DPMS_OFF);

	drm_for_each_legacy_plane(plane, &crtc->dev->mode_config.plane_list) {
		if (plane->crtc != crtc)
			continue;
		ret = plane->funcs->disable_plane(plane);
		if (ret)
			DRM_ERROR("fail : disable plane %d\n", ret);
	}
}

static struct drm_crtc_helper_funcs nx_crtc_helper_funcs = {
	.dpms = nx_drm_crtc_dpms,
	.prepare = nx_drm_crtc_prepare,
	.commit = nx_drm_crtc_commit,
	.mode_fixup = nx_drm_crtc_mode_fixup,
	.mode_set = nx_drm_crtc_mode_set,
	.mode_set_base = nx_drm_crtc_mode_set_base,
	.disable = nx_drm_crtc_disable,
};

static int nx_drm_crtc_page_flip(struct drm_crtc *crtc,
			struct drm_framebuffer *fb,
			struct drm_pending_vblank_event *event,
			uint32_t flags)
{
	struct drm_device *dev = crtc->dev;
	struct nx_drm_crtc *nx_crtc = to_nx_crtc(crtc);
	struct drm_framebuffer *old_fb = crtc->primary->fb;
	unsigned int crtc_w, crtc_h;
	int ret;

	DRM_DEBUG_KMS("page flip crtc.%d\n", nx_crtc->pipe);

	/* when the page flip is requested, crtc's dpms should be on */
	if (nx_crtc->dpms_mode > DRM_MODE_DPMS_ON) {
		DRM_ERROR("fail : page flip request.\n");
		return -EINVAL;
	}

	if (!event)
		return -EINVAL;

	spin_lock_irq(&dev->event_lock);
	if (nx_crtc->event) {
		ret = -EBUSY;
		goto out;
	}

	ret = drm_vblank_get(dev, nx_crtc->pipe);
	if (ret) {
		DRM_DEBUG("fail : to acquire vblank counter\n");
		goto out;
	}

	nx_crtc->event = event;
	spin_unlock_irq(&dev->event_lock);

	/*
	 * the pipe from user always is 0 so we can set pipe number
	 * of current owner to event.
	 */
	event->pipe = nx_crtc->pipe;

	crtc->primary->fb = fb;
	crtc_w = fb->width - crtc->x;
	crtc_h = fb->height - crtc->y;

	ret = nx_drm_dp_plane_update(crtc->primary, fb, 0, 0,
			crtc_w, crtc_h, crtc->x, crtc->y,
			crtc_w, crtc_h);

	if (ret) {
		crtc->primary->fb = old_fb;
		spin_lock_irq(&dev->event_lock);
		nx_crtc->event = NULL;
		drm_vblank_put(dev, nx_crtc->pipe);
		spin_unlock_irq(&dev->event_lock);
		return ret;
	}

	return 0;

out:
	spin_unlock_irq(&dev->event_lock);
	return ret;
}

static void nx_drm_crtc_destroy(struct drm_crtc *crtc)
{
	struct drm_device *drm = crtc->dev;
	struct nx_drm_priv *priv = crtc->dev->dev_private;
	struct nx_drm_crtc *nx_crtc = to_nx_crtc(crtc);
	int pipe = nx_crtc->pipe;

	DRM_DEBUG_KMS("enter crtc.%d\n", nx_crtc->pipe);

	priv->crtcs[pipe] = NULL;

	drm_crtc_cleanup(crtc);
	devm_kfree(drm->dev, nx_crtc);
}

static void nx_drm_crtc_reset(struct drm_crtc *crtc)
{
	DRM_DEBUG_KMS("enter\n");
}

static struct drm_crtc_funcs nx_crtc_funcs = {
	.reset = nx_drm_crtc_reset,
	.set_config = drm_crtc_helper_set_config,
	.page_flip = nx_drm_crtc_page_flip,
	.destroy = nx_drm_crtc_destroy,
};

int nx_drm_crtc_enable_vblank(struct drm_device *drm, int crtc)
{
	struct nx_drm_priv *priv = drm->dev_private;
	struct nx_drm_crtc *nx_crtc = to_nx_crtc(priv->crtcs[crtc]);

	DRM_DEBUG_KMS("enter crtc.%d\n", nx_crtc->pipe);

	if (nx_crtc->dpms_mode != DRM_MODE_DPMS_ON)
		return -EPERM;

	return 0;
}

void nx_drm_crtc_disable_vblank(struct drm_device *drm, int crtc)
{
	struct nx_drm_priv *priv = drm->dev_private;
	struct nx_drm_crtc *nx_crtc = to_nx_crtc(priv->crtcs[crtc]);

	DRM_DEBUG_KMS("enter crtc.%d\n", nx_crtc->pipe);

	if (nx_crtc->dpms_mode != DRM_MODE_DPMS_ON)
		return;
}

static int __of_graph_get_port_num_index(struct drm_device *drm,
			int *pipe, int pipe_size)
{
	struct device *dev = &drm->platformdev->dev;
	struct device_node *parent = dev->of_node;
	struct device_node *node, *port;
	int num = 0;

	node = of_get_child_by_name(parent, "ports");
	if (node)
		parent = node;

	for_each_child_of_node(parent, port) {
		u32 port_id = 0;

		if (of_node_cmp(port->name, "port") != 0)
			continue;
		if (of_property_read_u32(port, "reg", &port_id))
			continue;

		pipe[num] = port_id;
		num++;

		if (num > (pipe_size - 1))
			break;
	}
	of_node_put(node);

	return num;
}

#define parse_read_prop(n, s, v)	{ \
	u32 _v;	\
	if (!of_property_read_u32(n, s, &_v))	\
		v = _v;	\
	}

static int nx_drm_crtc_parse_dt(struct drm_device *drm,
			struct drm_crtc *crtc, int pipe)
{
	struct device *dev = &drm->platformdev->dev;
	struct device_node *node = dev->of_node;
	struct device_node *child;
	struct dp_plane_top *top = &to_nx_crtc(crtc)->top;
	const char *strings[10];
	int i, size = 0;

	DRM_DEBUG_KMS("crtc.%d for %s\n", pipe, dev_name(dev));

	child = of_graph_get_port_by_id(node, pipe);
	if (!child)
		return -EINVAL;

	parse_read_prop(child, "back_color", top->back_color);
	parse_read_prop(child, "color_key", top->color_key);

	size = of_property_read_string_array(child,
				"plane-names", strings, 10);

	top->num_planes = size;

	for (i = 0; size > i; i++) {
		if (!strcmp("primary", strings[i])) {
			top->plane_type[i] = DRM_PLANE_TYPE_PRIMARY;
			top->plane_flag[i] = PLANE_FLAG_RGB;
		} else if (!strcmp("cursor", strings[i])) {
			top->plane_type[i] = DRM_PLANE_TYPE_CURSOR;
			top->plane_flag[i] = PLANE_FLAG_RGB;
		} else if (!strcmp("rgb", strings[i])) {
			top->plane_type[i] = DRM_PLANE_TYPE_OVERLAY;
			top->plane_flag[i] = PLANE_FLAG_RGB;
		} else if (!strcmp("video", strings[i])) {
			top->plane_type[i] = DRM_PLANE_TYPE_OVERLAY;
			top->plane_flag[i] = PLANE_FLAG_VIDEO;	/* video */
			top->video_prior = i;	/* priority */
		} else {
			top->plane_flag[i] = PLANE_FLAG_UNKNOWN;
			DRM_ERROR("fail : unknown plane name [%d] %s\n",
				i, strings[i]);
		}
		DRM_DEBUG_KMS("crtc.%d planes[%d]: %s, bg:0x%08x, key:0x%08x\n",
			pipe, i, strings[i], top->back_color, top->color_key);
	}

	return 0;
}

static int nx_drm_crtc_create_planes(struct drm_device *drm,
			struct drm_crtc *crtc, int pipe)
{
	struct drm_plane **planes;
	struct drm_plane *plane;
	struct nx_drm_crtc *nx_crtc = to_nx_crtc(crtc);
	struct dp_plane_top *top = &nx_crtc->top;
	int i = 0, ret = 0;
	int num = 0, plane_num = 0;

	/* setup crtc's planes */
	planes = devm_kzalloc(drm->dev,
				sizeof(struct drm_plane *) * top->num_planes,
				GFP_KERNEL);
	if (IS_ERR(planes))
		return PTR_ERR(planes);

	for (i = 0; top->num_planes > i; i++) {
		unsigned int plane_type = top->plane_type[i];
		bool video = top->plane_flag[i] == PLANE_FLAG_VIDEO ?
						true : false;

		if (PLANE_FLAG_UNKNOWN == top->plane_flag[i])
			continue;

		plane_num = video ? PLANE_VIDEO_NUM : num++;

		plane = nx_drm_plane_init(
				drm, crtc, (1 << pipe), plane_type, plane_num);
		if (IS_ERR(plane)) {
			ret = PTR_ERR(plane);
			goto err_plane;
		}

		if (DRM_PLANE_TYPE_PRIMARY == plane_type) {
			top->primary_plane = i;
			ret = drm_crtc_init_with_planes(drm,
					crtc, plane, NULL, &nx_crtc_funcs);
			if (0 > ret)
				goto err_plane;
		}
		planes[i] = plane;
	}

	drm_crtc_helper_add(crtc, &nx_crtc_helper_funcs);
	devm_kfree(drm->dev, planes);

	return 0;

err_plane:
	for (i = 0; top->num_planes > i; i++) {
		plane = planes[i];
		if (plane)
			plane->funcs->destroy(plane);
	}

	if (planes)
		devm_kfree(drm->dev, planes);

	return ret;
}

int nx_drm_crtc_init(struct drm_device *drm)
{
	struct nx_drm_crtc **nx_crtcs;
	int pipes[10], num_crtcs = 0;
	int size = ARRAY_SIZE(pipes);
	int i = 0, ret = 0;

	num_crtcs = __of_graph_get_port_num_index(drm, pipes, size);
	DRM_DEBUG_KMS("enter num of crtcs %d\n", num_crtcs);

	/* setup crtc's planes */
	nx_crtcs = devm_kzalloc(drm->dev,
				sizeof(struct nx_drm_crtc *) * num_crtcs,
				GFP_KERNEL);
	if (IS_ERR(nx_crtcs))
		goto err_crtc;

	for (i = 0; num_crtcs > i; i++) {
		struct nx_drm_priv *priv;
		struct nx_drm_crtc *nx_crtc;
		int pipe = pipes[i];

		nx_crtc = devm_kzalloc(drm->dev,
					sizeof(struct nx_drm_crtc), GFP_KERNEL);
		if (IS_ERR(nx_crtc))
			goto err_crtc;

		priv = drm->dev_private;
		priv->crtcs[i] = &nx_crtc->crtc;	/* sequentially link */
		priv->num_crtcs++;

		nx_crtc->pipe = pipe;
		nx_crtc->pipe_irq = priv->hw_irq_no[pipe];
		nx_crtc->dpms_mode = DRM_MODE_DPMS_OFF;

		ret = nx_drm_crtc_parse_dt(drm, &nx_crtc->crtc, pipe);
		if (0 > ret)
			return ret;

		nx_drm_dp_crtc_init(drm, &nx_crtc->crtc, pipe);
		ret = nx_drm_crtc_create_planes(drm, &nx_crtc->crtc, pipe);
		if (0 > ret)
			goto err_crtc;

		nx_crtcs[i]	= nx_crtc;
		DRM_INFO("crtc[%d]: pipe.%d (irq.%d)\n",
			i, pipe, nx_crtc->pipe_irq);
	}

	devm_kfree(drm->dev, nx_crtcs);

	DRM_DEBUG_KMS("done\n");
	return 0;

err_crtc:
	for (i = 0; num_crtcs > i; i++) {
		if (nx_crtcs[i])
			devm_kfree(drm->dev, nx_crtcs[i]);
	}

	if (nx_crtcs)
		devm_kfree(drm->dev, nx_crtcs);

	return ret;
}

