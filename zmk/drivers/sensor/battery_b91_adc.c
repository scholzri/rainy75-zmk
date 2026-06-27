/*
 * Copyright (c) 2025 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Telink B91 SAR ADC battery voltage sensor.
 *
 * Reads battery voltage via the B91's 10-channel SAR ADC through a resistor
 * divider. ADC channel and divider ratio configured via devicetree.
 *
 * SAR ADC register sequences from TLSR9511B datasheet Section 11 + SDK.
 * Analog registers accessed via serial interface (not memory-mapped).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(bat_b91_adc, LOG_LEVEL_INF);

#define DT_DRV_COMPAT telink_b91_battery_adc

/* ========================================================================
 * Analog register access (serial interface)
 *
 * Same protocol as USB driver (usb_dc_b91.c) and BLE driver (b91_bt.c).
 * Duplicated here to keep drivers independent.
 * ======================================================================== */

#define ANA_ADDR_REG    0x80140180UL
#define ANA_CTRL_REG    0x80140182UL
#define ANA_LEN_REG     0x80140183UL
#define ANA_DATA_REG    0x80140184UL
#define ANA_CTRL_RW     BIT(5)  /* 1=write, 0=read */
#define ANA_CTRL_CYC    BIT(6)  /* cycle/trigger */
#define ANA_CTRL_BUSY   BIT(7)  /* busy status (same register) */
#define ANA_BUF_CNT_REG 0x80140188UL
#define ANA_TX_BUFCNT   0xF0    /* bits 4-7: TX buffer count */

/* Digital reset register (for SARADC reset) */
#define REG_RST3        0x80001323UL

static void ana_wait(void)
{
	while (sys_read8(ANA_CTRL_REG) & ANA_CTRL_BUSY) {
	}
}

static void ana_wait_txbuf(void)
{
	while (!(sys_read8(ANA_BUF_CNT_REG) & ANA_TX_BUFCNT)) {
	}
}

static uint8_t ana_read(uint8_t addr)
{
	unsigned int key = irq_lock();

	sys_write8(addr, ANA_ADDR_REG);
	sys_write8(1, ANA_LEN_REG);
	sys_write8(ANA_CTRL_CYC, ANA_CTRL_REG);
	ana_wait();
	uint8_t val = sys_read8(ANA_DATA_REG);

	irq_unlock(key);
	return val;
}

static void ana_write(uint8_t addr, uint8_t data)
{
	unsigned int key = irq_lock();

	sys_write8(addr, ANA_ADDR_REG);
	sys_write8(data, ANA_DATA_REG);
	ana_wait_txbuf();
	sys_write8(ANA_CTRL_CYC | ANA_CTRL_RW, ANA_CTRL_REG);
	ana_wait();
	sys_write8(0, ANA_CTRL_REG);

	irq_unlock(key);
}

/* ========================================================================
 * SAR ADC initialization (datasheet Section 11.4)
 * ======================================================================== */

static void adc_init(void)
{
	/* Power off ADC */
	ana_write(0xFC, ana_read(0xFC) | BIT(5));

	/* Reset SARADC block */
	sys_write8(sys_read8(REG_RST3) & ~BIT(6), REG_RST3);
	sys_write8(sys_read8(REG_RST3) | BIT(6), REG_RST3);

	/* Enable 24MHz clock to SAR ADC */
	ana_write(0x82, ana_read(0x82) | BIT(6));

	/* Clock divider: 24MHz/(5+1) = 4 MHz ADC clock */
	ana_write(0xF4, 5);

	/* Vref = 1.2V */
	ana_write(0xEA, 0x02);

	/* Prescale 1/4 (bits[7:6] = 0x02) */
	ana_write(0xFA, (ana_read(0xFA) & 0x3F) | (0x02 << 6));

	/* Resolution: 14-bit */
	ana_write(0xEC, (ana_read(0xEC) & 0xFC) | 0x03);

	/* Differential mode */
	ana_write(0xEC, ana_read(0xEC) | BIT(6));

	/* Sampling cycles: 12 cycles @ 4MHz = 3us */
	ana_write(0xEE, 3);

	/* Capture timing: r_max_mc = 490 -> 20.4us -> ~48 kHz */
	ana_write(0xEF, 490 & 0xFF);                          /* r_max_mc[7:0] */
	ana_write(0xF1, ((490 >> 8) << 6) | 10);              /* r_max_mc[9:8] | r_max_s */

	/* Enable Misc channel, max state index = 2 */
	ana_write(0xF2, BIT(2) | (0x02 << 4));

	/* Power on ADC */
	ana_write(0xFC, ana_read(0xFC) & ~BIT(5));

	/* Wait for ADC to stabilize */
	k_busy_wait(1000);
}

/* ========================================================================
 * Read single ADC channel
 * ======================================================================== */

static int adc_read_channel(uint8_t channel_code)
{
	/* Set input: positive = channel, negative = GND (0xF) */
	ana_write(0xEB, (channel_code << 4) | 0x0F);

	/* Wait for settling + conversion */
	k_busy_wait(200);

	/* Lock ADC output register */
	ana_write(0xF3, ana_read(0xF3) | BIT(0));

	/* Read 16-bit result */
	uint16_t code = ana_read(0xF7) | ((uint16_t)ana_read(0xF8) << 8);

	/* Unlock */
	ana_write(0xF3, ana_read(0xF3) & ~BIT(0));

	/* Discard negative values (bit13 = sign) */
	if (code & BIT(13)) {
		return 0;
	}
	code &= 0x1FFF;

	/* Convert to millivolts: mv = (code x prescale x Vref) >> 13
	 * prescale = 4 (1/4 scaling), Vref = 1175 mV (calibrated 1.2V) */
	return (int)((uint32_t)code * 4 * 1175 >> 13);
}

/* ========================================================================
 * Battery sensor driver (Zephyr sensor API)
 * ======================================================================== */

struct b91_battery_config {
	uint8_t adc_channel;
	uint16_t div_num;
	uint16_t div_den;
	uint16_t full_mv;
	uint16_t empty_mv;
};

struct b91_battery_data {
	int voltage_mv;
	int state_of_charge;
};

static int b91_battery_sample_fetch(const struct device *dev,
				    enum sensor_channel chan)
{
	const struct b91_battery_config *cfg = dev->config;
	struct b91_battery_data *data = dev->data;

	if (chan != SENSOR_CHAN_ALL &&
	    chan != SENSOR_CHAN_GAUGE_VOLTAGE &&
	    chan != SENSOR_CHAN_GAUGE_STATE_OF_CHARGE) {
		return -ENOTSUP;
	}

	int pin_mv = adc_read_channel(cfg->adc_channel);

	/* Undo voltage divider: battery_mv = pin_mv x den / num */
	data->voltage_mv = pin_mv * cfg->div_den / cfg->div_num;

	/* Linear percentage */
	int range = cfg->full_mv - cfg->empty_mv;
	if (range <= 0) {
		data->state_of_charge = 0;
	} else {
		data->state_of_charge = (data->voltage_mv - cfg->empty_mv) * 100 / range;
		data->state_of_charge = CLAMP(data->state_of_charge, 0, 100);
	}

	LOG_DBG("Battery: %d mV (%d%%)", data->voltage_mv, data->state_of_charge);
	return 0;
}

static int b91_battery_channel_get(const struct device *dev,
				   enum sensor_channel chan,
				   struct sensor_value *val)
{
	struct b91_battery_data *data = dev->data;

	switch (chan) {
	case SENSOR_CHAN_GAUGE_VOLTAGE:
		val->val1 = data->voltage_mv / 1000;
		val->val2 = (data->voltage_mv % 1000) * 1000;
		return 0;
	case SENSOR_CHAN_GAUGE_STATE_OF_CHARGE:
		val->val1 = data->state_of_charge;
		val->val2 = 0;
		return 0;
	default:
		return -ENOTSUP;
	}
}

static int b91_battery_init(const struct device *dev)
{
	const struct b91_battery_config *cfg = dev->config;

	adc_init();

	LOG_INF("Battery sensor: ADC ch 0x%x, divider %d/%d, range %d-%d mV",
		cfg->adc_channel, cfg->div_num, cfg->div_den,
		cfg->empty_mv, cfg->full_mv);

	return 0;
}

static DEVICE_API(sensor, b91_battery_api) = {
	.sample_fetch = b91_battery_sample_fetch,
	.channel_get = b91_battery_channel_get,
};

#define B91_BATTERY_INIT(inst)                                              \
	static const struct b91_battery_config b91_bat_cfg_##inst = {           \
		.adc_channel = DT_INST_PROP(inst, adc_channel),                     \
		.div_num = DT_INST_PROP(inst, divider_numerator),                   \
		.div_den = DT_INST_PROP(inst, divider_denominator),                 \
		.full_mv = DT_INST_PROP(inst, full_mv),                             \
		.empty_mv = DT_INST_PROP(inst, empty_mv),                           \
	};                                                                      \
	static struct b91_battery_data b91_bat_data_##inst;                     \
	DEVICE_DT_INST_DEFINE(inst, b91_battery_init, NULL,                     \
			      &b91_bat_data_##inst, &b91_bat_cfg_##inst,        \
			      POST_KERNEL,                                      \
			      CONFIG_SENSOR_INIT_PRIORITY,                      \
			      &b91_battery_api);

DT_INST_FOREACH_STATUS_OKAY(B91_BATTERY_INIT)
