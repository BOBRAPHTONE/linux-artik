/*
 * Copyright (C) 2010, 2012-2016 ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation, and any use by you of this program is subject to the terms of such GNU licence.
 *
 * A copy of the licence is included with the program, and can also be obtained from Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

/**
 * @file s5pxx18.c
 * Platform specific Mali driver functions for:
 * - Nexell s5p6818 platforms with ARM CortexA53 8 cores.
 * - Nexell s5p4418 platforms with ARM CortexA9 4 cores.
 */
#include <linux/platform_device.h>
#include <linux/version.h>
#include <linux/pm.h>
#include "mali_kernel_linux.h"
#ifdef CONFIG_PM_RUNTIME
#include <linux/pm_runtime.h>
#endif
#include <asm/io.h>
#include <linux/mali/mali_utgard.h>
#include "mali_kernel_common.h"
#include <linux/dma-mapping.h>
#include <linux/moduleparam.h>
#include <linux/reset.h>
#include <linux/clk.h>

#include "s5pxx18_core_scaling.h"
#include "mali_executor.h"

#if defined(CONFIG_MALI_DEVFREQ) && defined(CONFIG_DEVFREQ_THERMAL)
#include <linux/devfreq_cooling.h>
#include <linux/thermal.h>
#endif

static int mali_core_scaling_enable = 0;
struct clk *clk_mali;
struct reset_control *rst_mali;

void mali_gpu_utilization_callback(struct mali_gpu_utilization_data *data);

static struct mali_gpu_device_data mali_gpu_data = {
	.max_job_runtime = 60000, /* 60 seconds */

	/* Some framebuffer drivers get the framebuffer dynamically, such as through GEM,
	* in which the memory resource can't be predicted in advance.
	*/
	.fb_start = 0x0,
	.fb_size = 0xFFFFF000,
	.control_interval = 1000, /* 1000ms */
	.utilization_callback = mali_gpu_utilization_callback,
	.get_clock_info = NULL,
	.get_freq = NULL,
	.set_freq = NULL,
	.secure_mode_init = NULL,
	.secure_mode_deinit = NULL,
	.gpu_reset_and_secure_mode_enable = NULL,
	.gpu_reset_and_secure_mode_disable = NULL,
};

int mali_platform_device_init(struct platform_device *device)
{
	int num_pp_cores = 2;
	int err = -1;
	struct device *dev = &device->dev;

	clk_mali = devm_clk_get(dev, "clk_mali");
	if (IS_ERR_OR_NULL(clk_mali)) {
		dev_err(dev, "failed to get mali clock\n");
		return -ENODEV;
	}

	clk_prepare_enable(clk_mali);

	rst_mali = devm_reset_control_get(dev, "vr-reset");

	if (IS_ERR(rst_mali)) {
		dev_err(dev, "failed to get reset_control\n");
		return -EINVAL;
	}

	reset_control_reset(rst_mali);

#ifdef CONFIG_MALI_PLATFORM_S5P6818
	num_pp_cores = 4;
#endif
	/* After kernel 3.15 device tree will default set dev
	 * related parameters in of_platform_device_create_pdata.
	 * But kernel changes from version to version,
	 * For example 3.10 didn't include device->dev.dma_mask parameter setting,
	 * if we didn't include here will cause dma_mapping error,
	 * but in kernel 3.15 it include  device->dev.dma_mask parameter setting,
	 * so it's better to set must need paramter by DDK itself.
	 */
	if (!device->dev.dma_mask)
		device->dev.dma_mask = &device->dev.coherent_dma_mask;
#ifdef CONFIG_ARM64
	device->dev.archdata.dma_ops = dma_ops;
#else
	device->dev.archdata.dma_ops = &arm_dma_ops;
#endif

	err = platform_device_add_data(device, &mali_gpu_data, sizeof(mali_gpu_data));

	if (0 == err) {
#ifdef CONFIG_PM_RUNTIME
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 37))
		pm_runtime_set_autosuspend_delay(&(device->dev), 1000);
		pm_runtime_use_autosuspend(&(device->dev));
#endif
		pm_runtime_enable(&(device->dev));
#endif
		MALI_DEBUG_ASSERT(0 < num_pp_cores);
		mali_core_scaling_init(num_pp_cores);
	}

#if defined(CONFIG_MALI_DEVFREQ) && defined(CONFIG_DEVFREQ_THERMAL)
	/* Get thermal zone */
	gpu_tz = thermal_zone_get_zone_by_name("soc_thermal");
	if (IS_ERR(gpu_tz)) {
		MALI_DEBUG_PRINT(2, ("Error getting gpu thermal zone (%ld), not yet ready?\n",
				     PTR_ERR(gpu_tz)));
		gpu_tz = NULL;

		err =  -EPROBE_DEFER;
	}
#endif

	return err;
}

int mali_platform_device_deinit(struct platform_device *device)
{
	MALI_IGNORE(device);

	MALI_DEBUG_PRINT(4, ("mali_platform_device_deinit() called\n"));

	mali_core_scaling_term();

	if (rst_mali)
		reset_control_assert(rst_mali);

	if (clk_mali)
		clk_disable_unprepare(clk_mali);

#ifdef CONFIG_PM_RUNTIME
	pm_runtime_disable(&(device->dev));
#endif

	return 0;
}

static int param_set_core_scaling(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_int(val, kp);

	if (1 == mali_core_scaling_enable) {
		mali_core_scaling_sync(mali_executor_get_num_cores_enabled());
	}
	return ret;
}

static struct kernel_param_ops param_ops_core_scaling = {
	.set = param_set_core_scaling,
	.get = param_get_int,
};

module_param_cb(mali_core_scaling_enable, &param_ops_core_scaling, &mali_core_scaling_enable, 0644);
MODULE_PARM_DESC(mali_core_scaling_enable, "1 means to enable core scaling policy, 0 means to disable core scaling policy");

void mali_gpu_utilization_callback(struct mali_gpu_utilization_data *data)
{
	if (1 == mali_core_scaling_enable) {
		mali_core_scaling_update(data);
	}
}
