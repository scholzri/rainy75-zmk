/*
 * Copyright (c) 2026 scholzri 
 * SPDX-License-Identifier: Apache-2.0
 *
 * Telink B91 hardware watchdog driver.
 * Resets the entire chip on timeout (no interrupt/callback support).
 *
 * Datasheet: TLSR9511B Section 7.1.5
 * Register base: 0x80140140 (timer block)
 */

#define DT_DRV_COMPAT telink_b91_watchdog

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(wdt_b91, CONFIG_WDT_LOG_LEVEL);

/* Register offsets from timer base (0x80140140) */
#define REG_TMR_CTRL2_OFF   0x02
#define REG_TMR_STA_OFF     0x03
#define REG_WT_TARGET_OFF   0x0C

/* TMR_CTRL2 bits */
#define TMR_WD_EN           BIT(7)

/* TMR_STA / TMR_CTRL3 bits */
#define TMR_STA_WD          BIT(2)
#define TMR_WD_CNT_CLR      BIT(3)

/* PCLK frequency in kHz (24 MHz = 24000 kHz) */
#define B91_PCLK_KHZ        24000

/* Max timeout: 0xFFFFFF00 / 24000000 = ~11184 ms */
#define B91_WDT_MAX_TIMEOUT_MS  11184

struct wdt_b91_config {
	uintptr_t base;
};

struct wdt_b91_data {
	bool timeout_installed;
};

static inline uint8_t wdt_b91_read8(const struct wdt_b91_config *cfg,
				    uint32_t offset)
{
	return sys_read8(cfg->base + offset);
}

static inline void wdt_b91_write8(const struct wdt_b91_config *cfg,
				  uint32_t offset, uint8_t val)
{
	sys_write8(val, cfg->base + offset);
}

static inline void wdt_b91_write32(const struct wdt_b91_config *cfg,
				   uint32_t offset, uint32_t val)
{
	sys_write32(val, cfg->base + offset);
}

static int wdt_b91_setup(const struct device *dev, uint8_t options)
{
	const struct wdt_b91_config *cfg = dev->config;
	struct wdt_b91_data *data = dev->data;
	uint8_t ctrl2;

	if (!data->timeout_installed) {
		LOG_ERR("No timeout installed");
		return -EINVAL;
	}

	/* B91 WDT has no pause-in-sleep or pause-halted-by-dbg capability */
	if (options & (WDT_OPT_PAUSE_IN_SLEEP | WDT_OPT_PAUSE_HALTED_BY_DBG)) {
		return -ENOTSUP;
	}

	/* Clear watchdog counter before enabling */
	wdt_b91_write8(cfg, REG_TMR_STA_OFF,
		       TMR_STA_WD | TMR_WD_CNT_CLR);

	/* Enable watchdog */
	ctrl2 = wdt_b91_read8(cfg, REG_TMR_CTRL2_OFF);
	wdt_b91_write8(cfg, REG_TMR_CTRL2_OFF, ctrl2 | TMR_WD_EN);

	LOG_DBG("Watchdog enabled");
	return 0;
}

static int wdt_b91_disable(const struct device *dev)
{
	const struct wdt_b91_config *cfg = dev->config;
	struct wdt_b91_data *data = dev->data;
	uint8_t ctrl2;

	ctrl2 = wdt_b91_read8(cfg, REG_TMR_CTRL2_OFF);
	wdt_b91_write8(cfg, REG_TMR_CTRL2_OFF, ctrl2 & ~TMR_WD_EN);

	data->timeout_installed = false;

	LOG_DBG("Watchdog disabled");
	return 0;
}

static int wdt_b91_install_timeout(const struct device *dev,
				   const struct wdt_timeout_cfg *cfg_timeout)
{
	const struct wdt_b91_config *cfg = dev->config;
	struct wdt_b91_data *data = dev->data;
	uint32_t target;

	if (data->timeout_installed) {
		LOG_ERR("Only one timeout channel supported");
		return -ENOMEM;
	}

	/* No window timeout support */
	if (cfg_timeout->window.min != 0U) {
		return -EINVAL;
	}

	if (cfg_timeout->window.max == 0U ||
	    cfg_timeout->window.max > B91_WDT_MAX_TIMEOUT_MS) {
		LOG_ERR("Timeout out of range (1-%d ms)", B91_WDT_MAX_TIMEOUT_MS);
		return -EINVAL;
	}

	/* B91 WDT can only reset the SoC, no interrupt/callback */
	if (cfg_timeout->callback != NULL) {
		LOG_ERR("Callbacks not supported (B91 WDT resets chip)");
		return -ENOTSUP;
	}

	/* Only SoC reset or none flags accepted */
	uint8_t reset_type = cfg_timeout->flags & WDT_FLAG_RESET_MASK;
	if (reset_type == WDT_FLAG_RESET_CPU_CORE) {
		LOG_ERR("CPU-core-only reset not supported");
		return -ENOTSUP;
	}

	/*
	 * target = period_ms * pclk_khz
	 * At 24 MHz PCLK: target = period_ms * 24000
	 * Lower 8 bits of WT_TARGET are always 0, so actual granularity
	 * is 256/24MHz ≈ 10.67 µs. We round up.
	 */
	target = cfg_timeout->window.max * B91_PCLK_KHZ;

	/* Write 32-bit target (HW ignores lower 8 bits) */
	wdt_b91_write32(cfg, REG_WT_TARGET_OFF, target);

	data->timeout_installed = true;

	LOG_DBG("Timeout set to %u ms (target=0x%08x)",
		cfg_timeout->window.max, target);
	return 0; /* channel 0 */
}

static int wdt_b91_feed(const struct device *dev, int channel_id)
{
	const struct wdt_b91_config *cfg = dev->config;

	ARG_UNUSED(channel_id);

	/* Write TMR_STA_WD | TMR_WD_CNT_CLR to clear the counter */
	wdt_b91_write8(cfg, REG_TMR_STA_OFF,
		       TMR_STA_WD | TMR_WD_CNT_CLR);

	return 0;
}

static int wdt_b91_init(const struct device *dev)
{
	/* Disable watchdog at boot — app will enable via install_timeout + setup */
	wdt_b91_disable(dev);
	return 0;
}

static DEVICE_API(wdt, wdt_b91_api) = {
	.setup = wdt_b91_setup,
	.disable = wdt_b91_disable,
	.install_timeout = wdt_b91_install_timeout,
	.feed = wdt_b91_feed,
};

#define WDT_B91_INIT(n)                                                     \
	static struct wdt_b91_data wdt_b91_data_##n;                        \
	static const struct wdt_b91_config wdt_b91_cfg_##n = {              \
		.base = DT_INST_REG_ADDR(n),                                \
	};                                                                  \
	DEVICE_DT_INST_DEFINE(n, wdt_b91_init, NULL,                        \
			      &wdt_b91_data_##n, &wdt_b91_cfg_##n,         \
			      PRE_KERNEL_1,                                 \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE,           \
			      &wdt_b91_api);

DT_INST_FOREACH_STATUS_OKAY(WDT_B91_INIT)
