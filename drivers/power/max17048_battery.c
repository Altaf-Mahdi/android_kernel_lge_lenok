/*
 *  max17048_battery.c
 *  fuel-gauge systems for lithium-ion (Li+) batteries
 *
 *  Copyright (C) 2012 Nvidia Cooperation
 *  Chandler Zhang <chazhang@nvidia.com>
 *  Syed Rafiuddin <srafiuddin@nvidia.com>
 *
 *  Copyright (C) 2013-2015 LGE Inc.
 *  ChoongRyeol Lee <choongryeol.lee@lge.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/power/max17048_battery.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/qpnp/qpnp-adc.h>
#include <linux/debugfs.h>
#include <linux/suspend.h>
#include <linux/wakelock.h>

#define MODE_REG      0x06
#define VCELL_REG     0x02
#define SOC_REG       0x04
#define VERSION_REG   0x08
#define HIBRT_REG     0x0A
#define CONFIG_REG    0x0C
#define VALRT_REG     0x14
#define CRATE_REG     0x16
#define VRESET_REG    0x18
#define STATUS_REG    0x1A

#define CFG_ALRT_MASK    0x0020
#define CFG_ATHD_MASK    0x001F
#define CFG_ALSC_MASK    0x0040
#define CFG_RCOMP_MASK    0xFF00
#define CFG_RCOMP_SHIFT    8
#define CFG_ALSC_SHIFT   6
#define STAT_RI_MASK     0x0100
#define STAT_CLEAR_MASK  0xFF00

#define MAX17048_VERSION_11    0x11
#define MAX17048_VERSION_12    0x12

#define NORMAL_POLL_MS 60000
#define MAX17048_DEFAULT_TEMP  250

struct max17048_chip {
	struct i2c_client *client;
	struct power_supply batt_psy;
	struct power_supply *battery;
	struct max17048_platform_data *pdata;
	struct dentry *dent;
	struct notifier_block pm_notifier;
	struct delayed_work monitor_work;
	int vcell;
	int soc;
	int capacity_level;
	struct wake_lock alert_lock;
	int alert_gpio;
	int alert_irq;
	int rcomp;
	int rcomp_co_hot;
	int rcomp_co_cold;
	int alert_threshold;
	int max_mvolt;
	int min_mvolt;
	int full_soc;
	int empty_soc;
	int batt_tech;
	int fcc_mah;
	int voltage;
	int lasttime_voltage;
	int lasttime_capacity_level;
	int chg_state;
	int batt_temp;
	int batt_health;
	int batt_current;
	bool ext_batt_psy;
#ifdef CONFIG_SENSORS_QPNP_ADC_CURRENT
	struct qpnp_iadc_chip *iadc_dev;
#endif
};

static struct max17048_chip *ref;
static int max17048_get_prop_current(struct max17048_chip *chip);
static int max17048_clear_interrupt(struct max17048_chip *chip);

struct debug_reg {
	char  *name;
	u8  reg;
};

#define MAX17048_DEBUG_REG(x) {#x, x##_REG}

static struct debug_reg max17048_debug_regs[] = {
	MAX17048_DEBUG_REG(MODE),
	MAX17048_DEBUG_REG(VCELL),
	MAX17048_DEBUG_REG(SOC),
	MAX17048_DEBUG_REG(VERSION),
	MAX17048_DEBUG_REG(HIBRT),
	MAX17048_DEBUG_REG(CONFIG),
	MAX17048_DEBUG_REG(VALRT),
	MAX17048_DEBUG_REG(CRATE),
	MAX17048_DEBUG_REG(VRESET),
	MAX17048_DEBUG_REG(STATUS),
};

static int bound_check(int max, int min, int val)
{
	val = max(min, val);
	val = min(max, val);
	return val;
}

static int max17048_write_word(struct i2c_client *client, int reg, u16 value)
{
	int ret;

	if (!client)
		return -1;

	ret = i2c_smbus_write_word_data(client, reg, swab16(value));
	if (ret < 0)
		dev_err(&client->dev, "%s(): Failed in writing register"
					"0x%02x err %d\n", __func__, reg, ret);

	return ret;
}

static int max17048_read_word(struct i2c_client *client, int reg)
{
	int ret;

	if (!client)
		return -1;

	ret = i2c_smbus_read_word_data(client, reg);
	if (ret < 0)
		dev_err(&client->dev, "%s(): Failed in reading register"
					"0x%02x err %d\n", __func__, reg, ret);
	else
		ret = (int)swab16((uint16_t)(ret & 0x0000ffff));

	return ret;
}

static int max17048_masked_write_word(struct i2c_client *client, int reg,
			       u16 mask, u16 val)
{
	s32 rc;
	u16 temp;

	temp = max17048_read_word(client, reg);
	if (temp < 0) {
		pr_err("max17048_read_word failed: reg=%03X, rc=%d\n",
				reg, temp);
		return temp;
	}

	if ((temp & mask) == (val & mask))
		return 0;

	temp &= ~mask;
	temp |= val & mask;
	rc = max17048_write_word(client, reg, temp);
	if (rc) {
		pr_err("max17048_write_word failed: reg=%03X, rc=%d\n",
				reg, rc);
		return rc;
	}

	return 0;
}

static int set_empty_soc(void *data, u64 val)
{
	int *empty_soc = data;

	if (val > 1000 || val < 0) {
		pr_err("max17048: Tried to set empty_soc to illegal value\n");
		return -1;
	}

	*empty_soc = (int)val;
	return 0;
}

static int get_empty_soc(void *data, u64 *val)
{
	int *empty_soc = data;

	*val = (u64)*empty_soc;

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(empty_soc_fops, get_empty_soc, set_empty_soc, "%llu\n");

static int set_reg(void *data, u64 val)
{
	u32 addr = (u32) data;
	int ret;
	struct i2c_client *client = ref->client;

	ret = max17048_write_word(client, addr, (u16) val);

	return ret;
}

static int get_reg(void *data, u64 *val)
{
	u32 addr = (u32) data;
	int ret;
	struct i2c_client *client = ref->client;

	ret = max17048_read_word(client, addr);
	if (ret < 0)
		return ret;

	*val = ret;

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(reg_fops, get_reg, set_reg, "0x%02llx\n");

/* Using Quickstart instead of reset for Power Test
*  DO NOT USE THIS COMMAND ANOTHER SCENE.
*/
static int max17048_set_reset(struct max17048_chip *chip)
{
	max17048_write_word(chip->client, MODE_REG, 0x4000);
	pr_info("%s: Reset (Quickstart)\n", __func__);
	return 0;
}

static int max17048_get_capacity_from_soc(struct max17048_chip *chip)
{
	u8 buf[2];
	int batt_soc = 0;

	buf[0] = (chip->soc & 0x0000FF00) >> 8;
	buf[1] = (chip->soc & 0x000000FF);

	pr_debug("%s: SOC raw = 0x%x%x\n", __func__, buf[0], buf[1]);

	batt_soc = (((int)buf[0]*256)+buf[1])*19531; /* 0.001953125 */
	batt_soc = (batt_soc - (chip->empty_soc * 1000000))
			/ ((chip->full_soc - chip->empty_soc) * 10000);

	batt_soc = bound_check(100, 0, batt_soc);

	return batt_soc;
}

static int max17048_get_vcell(struct max17048_chip *chip)
{
	int vcell;

	vcell = max17048_read_word(chip->client, VCELL_REG);
	if (vcell < 0) {
		pr_err("%s: err %d\n", __func__, vcell);
		return vcell;
	} else {
		chip->vcell = vcell >> 4;
		chip->voltage = (chip->vcell * 5) >> 2;
	}

	return 0;
}

static int max17048_get_soc(struct max17048_chip *chip)
{
	int soc;

	soc = max17048_read_word(chip->client, SOC_REG);
	if (soc < 0) {
		pr_err("%s: err %d\n", __func__, soc);
		return soc;
	} else {
		chip->soc = soc;
		chip->capacity_level =
			max17048_get_capacity_from_soc(chip);
	}

	return 0;
}

static uint16_t max17048_get_version(struct max17048_chip *chip)
{
	return (uint16_t) max17048_read_word(chip->client, VERSION_REG);
}

static void max17048_set_rcomp(struct max17048_chip *chip)
{
	int ret;
	int scale_coeff;
	int rcomp;
	int temp = MAX17048_DEFAULT_TEMP;
	union power_supply_propval val = {0,};

	if (!chip->battery) {
		chip->battery = power_supply_get_by_name("battery");
		if (!chip->battery) {
			pr_err("%s: battery psy is not init yet!\n", __func__);
			return;
		}
	}

	ret = chip->battery->get_property(chip->battery,
			POWER_SUPPLY_PROP_TEMP, &val);
	if (ret < 0)
		pr_warn("%s: failed to get battery temp\n", __func__);
	else
		temp = val.intval;

	temp = temp / 10;

	if (temp > 20)
		scale_coeff = chip->rcomp_co_hot;
	else if (temp < 20)
		scale_coeff = chip->rcomp_co_cold;
	else
		scale_coeff = 0;

	rcomp = chip->rcomp * 1000 - (temp-20) * scale_coeff;
	rcomp = bound_check(255, 0, rcomp / 1000);

	pr_debug("%s: new RCOMP = 0x%02X\n", __func__, rcomp);

	rcomp = rcomp << CFG_RCOMP_SHIFT;

	ret = max17048_masked_write_word(chip->client,
			CONFIG_REG, CFG_RCOMP_MASK, rcomp);
	if (ret < 0)
		pr_err("%s: failed to set rcomp\n", __func__);
}

static void max17048_work(struct work_struct *work)
{
	struct max17048_chip *chip =
		container_of(work, struct max17048_chip, monitor_work.work);
	int ret = 0;

	wake_lock(&chip->alert_lock);

	pr_debug("%s.\n", __func__);

	max17048_get_prop_current(chip);
	max17048_set_rcomp(chip);
	max17048_get_vcell(chip);
	max17048_get_soc(chip);

	if (chip->voltage != chip->lasttime_voltage ||
		chip->capacity_level != chip->lasttime_capacity_level) {
		chip->lasttime_voltage = chip->voltage;
		chip->lasttime_capacity_level = chip->capacity_level;

		power_supply_changed(&chip->batt_psy);
	}

	ret = max17048_clear_interrupt(chip);
	if (ret < 0)
		pr_err("%s: failed to clear alert irq\n", __func__);

	pr_debug("%s: raw soc = 0x%04X raw vcell = 0x%04X\n",
			__func__, chip->soc, chip->vcell);
	pr_debug("%s: SOC = %d vbatt_mv = %d\n",
			__func__, chip->capacity_level, chip->voltage);

	wake_unlock(&chip->alert_lock);

	schedule_delayed_work(&chip->monitor_work,
			msecs_to_jiffies(NORMAL_POLL_MS));
}

static irqreturn_t max17048_interrupt_handler(int irq, void *data)
{
	struct max17048_chip *chip = data;

	pr_debug("%s: interupt occured\n", __func__);
	schedule_delayed_work(&chip->monitor_work, 0);

	return IRQ_HANDLED;
}

static int max17048_clear_interrupt(struct max17048_chip *chip)
{
	int ret;

	pr_debug("%s.\n", __func__);

	ret = max17048_masked_write_word(chip->client,
			CONFIG_REG, CFG_ALRT_MASK, 0);
	if (ret < 0) {
		pr_err("%s: failed to clear alert status bit\n", __func__);
		return ret;
	}

	ret = max17048_masked_write_word(chip->client,
			STATUS_REG, STAT_CLEAR_MASK, 0);
	if (ret < 0) {
		pr_err("%s: failed to clear status reg\n", __func__);
		return ret;
	}

	return 0;
}

static int max17048_set_athd_alert(struct max17048_chip *chip, int level)
{
	int ret;

	pr_debug("%s.\n", __func__);

	level = bound_check(32, 1, level);
	level = 32 - level;

	ret = max17048_masked_write_word(chip->client,
			CONFIG_REG, CFG_ATHD_MASK, level);
	if (ret < 0)
		pr_err("%s: failed to set athd alert\n", __func__);

	return ret;
}

static int max17048_set_alsc_alert(struct max17048_chip *chip, bool enable)
{
	int ret;
	u16 val;

	pr_debug("%s. with %d\n", __func__, enable);

	val = (u16)(!!enable << CFG_ALSC_SHIFT);

	ret = max17048_masked_write_word(chip->client,
			CONFIG_REG, CFG_ALSC_MASK, val);
	if (ret < 0)
		pr_err("%s: failed to set alsc alert\n", __func__);

	return ret;
}

ssize_t max17048_store_status(struct device *dev,
			  struct device_attribute *attr,
			  const char *buf,
			  size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct max17048_chip *chip = i2c_get_clientdata(client);

	if (!chip)
		return -ENODEV;

	if (strncmp(buf, "reset", 5) == 0) {
		max17048_set_reset(chip);
		schedule_delayed_work(&chip->monitor_work, 0);
	} else {
		return -EINVAL;
	}

	return count;
}
DEVICE_ATTR(fuelrst, 0220, NULL, max17048_store_status);

static int max17048_parse_dt(struct device *dev,
		struct max17048_chip *chip)
{
	struct device_node *dev_node = dev->of_node;
	int ret = 0;

	chip->ext_batt_psy = of_property_read_bool(dev_node,
			"max17048,ext-batt-psy");

	chip->alert_gpio = of_get_named_gpio(dev_node,
			"max17048,alert_gpio", 0);
	if (chip->alert_gpio < 0) {
		pr_err("failed to get stat-gpio.\n");
		ret = chip->alert_gpio;
		goto out;
	}

	ret = of_property_read_u32(dev_node, "max17048,rcomp",
				&chip->rcomp);
	if (ret) {
		pr_err("%s: failed to read rcomp\n", __func__);
		goto out;
	}

	ret = of_property_read_u32(dev_node, "max17048,rcomp-co-hot",
				&chip->rcomp_co_hot);
	if (ret) {
		pr_err("%s: failed to read rcomp_co_hot\n", __func__);
		goto out;
	}

	ret = of_property_read_u32(dev_node, "max17048,rcomp-co-cold",
				&chip->rcomp_co_cold);
	if (ret) {
		pr_err("%s: failed to read rcomp_co_cold\n", __func__);
		goto out;
	}

	ret = of_property_read_u32(dev_node, "max17048,alert_threshold",
				&chip->alert_threshold);
	if (ret) {
		pr_err("%s: failed to read alert_threshold\n", __func__);
		goto out;
	}

	ret = of_property_read_u32(dev_node, "max17048,max-mvolt",
				   &chip->max_mvolt);
	if (ret) {
		pr_err("%s: failed to read max voltage\n", __func__);
		goto out;
	}

	ret = of_property_read_u32(dev_node, "max17048,min-mvolt",
				   &chip->min_mvolt);
	if (ret) {
		pr_err("%s: failed to read min voltage\n", __func__);
		goto out;
	}

	ret = of_property_read_u32(dev_node, "max17048,full-soc",
				   &chip->full_soc);
	if (ret) {
		pr_err("%s: failed to read full soc\n", __func__);
		goto out;
	}

	ret = of_property_read_u32(dev_node, "max17048,empty-soc",
				   &chip->empty_soc);
	if (ret) {
		pr_err("%s: failed to read empty soc\n", __func__);
		goto out;
	}

	ret = of_property_read_u32(dev_node, "max17048,batt-tech",
				   &chip->batt_tech);
	if (ret) {
		pr_err("%s: failed to read batt technology\n", __func__);
		goto out;
	}

	ret = of_property_read_u32(dev_node, "max17048,fcc-mah",
				   &chip->fcc_mah);
	if (ret) {
		pr_err("%s: failed to read batt fcc\n", __func__);
		goto out;
	}

	pr_info("%s: rcomp = %d rcomp_co_hot = %d rcomp_co_cold = %d",
			__func__, chip->rcomp, chip->rcomp_co_hot,
			chip->rcomp_co_cold);
	pr_info("%s: alert_thres = %d full_soc = %d empty_soc = %d",
			__func__, chip->alert_threshold,
			chip->full_soc, chip->empty_soc);

out:
	return ret;
}

static int max17048_get_prop_vbatt_uv(struct max17048_chip *chip)
{
	max17048_get_vcell(chip);
	return chip->voltage * 1000;
}

static int max17048_get_prop_present(struct max17048_chip *chip)
{
	/*FIXME - need to implement */
	return true;
}

#ifdef CONFIG_SENSORS_QPNP_ADC_CURRENT
static bool qpnp_iadc_is_ready(struct max17048_chip *chip)
{
	struct i2c_client *client = chip->client;

	if (!client)
		return false;

	if (chip->iadc_dev)
		return true; /* ready */

	client = chip->client;
	chip->iadc_dev = qpnp_get_iadc(&client->dev, "chg");
	if (IS_ERR(chip->iadc_dev)) {
		chip->iadc_dev = NULL;
		return false;
	}

	return true;
}
static int qpnp_get_battery_current(struct max17048_chip *chip)
{
	struct qpnp_iadc_result i_result;
	int ret;

	if (!qpnp_iadc_is_ready(chip)) {
		pr_err("%s: qpnp iadc is not ready!\n", __func__);
		chip->batt_current = 0;
		return 0;
	}

	ret = qpnp_iadc_read(chip->iadc_dev, EXTERNAL_RSENSE, &i_result);
	if (ret) {
		pr_err("%s: failed to read iadc\n", __func__);
		chip->batt_current = 0;
		return ret;
	}

	/* positive polarity indicate net current entering the battery */
	chip->batt_current = i_result.result_ua;

	return 0;
}
#endif

static int max17048_get_prop_current(struct max17048_chip *chip)
{
#ifdef CONFIG_SENSORS_QPNP_ADC_CURRENT
	int ret;

	ret = qpnp_get_battery_current(chip);
	if (ret)
		pr_err("%s: failed to get batt current.\n", __func__);
#else
	pr_warn("%s: batt current is not supported!\n", __func__);
	chip->batt_current = 0;
#endif
	pr_debug("%s: ibatt_ua = %d\n", __func__, chip->batt_current);

	return chip->batt_current;
}

static enum power_supply_property max17048_battery_props[] = {

	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
};

static int max17048_get_property(struct power_supply *psy,
			    enum power_supply_property psp,
			    union power_supply_propval *val)
{
	struct max17048_chip *chip = container_of(psy,
				struct max17048_chip, batt_psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = max17048_get_prop_present(chip);
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = chip->batt_tech;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = chip->max_mvolt * 1000;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		val->intval = chip->min_mvolt * 1000;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = max17048_get_prop_vbatt_uv(chip);
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = chip->capacity_level;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = max17048_get_prop_current(chip);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = chip->fcc_mah;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int max17048_create_debugfs_entries(struct max17048_chip *chip)
{
	int i;
	struct dentry *file;

	chip->dent = debugfs_create_dir("max17048", NULL);
	if (IS_ERR(chip->dent)) {
		pr_err("max17048 driver couldn't create debugfs dir\n");
		return -EFAULT;
	}

	for (i = 0 ; i < ARRAY_SIZE(max17048_debug_regs) ; i++) {
		char *name = max17048_debug_regs[i].name;
		u32 reg = max17048_debug_regs[i].reg;

		file = debugfs_create_file(name, 0644, chip->dent,
					(void *) reg, &reg_fops);
		if (IS_ERR(file)) {
			pr_err("debugfs_create_file %s failed.\n", name);
			return -EFAULT;
		}
	}

	file = debugfs_create_file("empty_soc", 0644, chip->dent,
			(void *) &(chip->empty_soc), &empty_soc_fops);
	if (IS_ERR(file)) {
		pr_err("debugfs_create_file empty_soc failed.\n");
		return -EFAULT;
	}

	return 0;
}

static int max17048_hw_init(struct max17048_chip *chip)
{
	int ret;

	ret = max17048_masked_write_word(chip->client,
			STATUS_REG, STAT_RI_MASK, 0);
	if (ret) {
		pr_err("%s: failed to clear ri bit\n", __func__);
		return ret;
	}

	ret = max17048_set_athd_alert(chip, chip->alert_threshold);
	if (ret) {
		pr_err("%s: failed to set athd alert threshold\n", __func__);
		return ret;
	}

	ret = max17048_set_alsc_alert(chip, true);
	if (ret) {
		pr_err("%s: failed to set alsc alert\n", __func__);
		return ret;
	}

	return 0;
}

static int max17048_pm_notifier(struct notifier_block *notifier,
			unsigned long pm_event, void *unused)
{
	struct max17048_chip *chip = container_of(notifier,
				struct max17048_chip, pm_notifier);

	switch (pm_event) {
	case PM_SUSPEND_PREPARE:
		max17048_set_alsc_alert(chip, false);
		cancel_delayed_work_sync(&chip->monitor_work);
		break;
	case PM_POST_SUSPEND:
		schedule_delayed_work(&chip->monitor_work,
					msecs_to_jiffies(200));
		max17048_set_alsc_alert(chip, true);
		break;
	default:
		break;
	}

	return NOTIFY_DONE;
}

static int max17048_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct max17048_chip *chip;
	int ret;
	uint16_t version;

	chip = kzalloc(sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	if (!i2c_check_functionality(client->adapter,
				I2C_FUNC_SMBUS_WORD_DATA)) {
		pr_err("%s: i2c_check_functionality fail\n", __func__);
		return -EIO;
	}

	chip->client = client;
	if (&client->dev.of_node) {
		ret = max17048_parse_dt(&client->dev, chip);
		if (ret) {
			pr_err("%s: failed to parse dt\n", __func__);
			goto  error;
		}
	} else {
		chip->pdata = client->dev.platform_data;
	}

	i2c_set_clientdata(client, chip);

	version = max17048_get_version(chip);
	dev_info(&client->dev, "MAX17048 Fuel-Gauge Ver 0x%x\n", version);
	if (version != MAX17048_VERSION_11 &&
	    version != MAX17048_VERSION_12) {
		pr_err("%s: Not supported version: 0x%x\n", __func__,
				version);
		ret = -ENODEV;
		goto error;
	}

	ref = chip;
	/*
	 * If ext_batt_psy is true, then an external device publishes
	 * a POWER_SUPPLY_TYPE_BATTERY, so this driver will publish its
	 * data via the POWER_SUPPLY_TYPE_BMS type
	 */
	if (chip->ext_batt_psy) {
		chip->batt_psy.name = "bms";
		chip->batt_psy.type = POWER_SUPPLY_TYPE_BMS;
	} else {
		chip->batt_psy.name = "battery";
		chip->batt_psy.type = POWER_SUPPLY_TYPE_BATTERY;
	}
	chip->batt_psy.get_property = max17048_get_property;
	chip->batt_psy.properties = max17048_battery_props;
	chip->batt_psy.num_properties = ARRAY_SIZE(max17048_battery_props);
	ret = power_supply_register(&client->dev, &chip->batt_psy);
	if (ret) {
		dev_err(&client->dev, "failed: power supply register\n");
		goto error;
	}

	INIT_DELAYED_WORK(&chip->monitor_work, max17048_work);
	wake_lock_init(&chip->alert_lock, WAKE_LOCK_SUSPEND,
			"max17048_alert");

	ret = gpio_request_one(chip->alert_gpio, GPIOF_DIR_IN,
				"max17048_alert");
	if (ret) {
		pr_err("%s: GPIO Request Failed : return %d\n",
				__func__, ret);
		goto err_gpio_request;
	}

	chip->alert_irq = gpio_to_irq(chip->alert_gpio);
	if (chip->alert_irq < 0) {
		pr_err("%s: failed to get alert irq\n", __func__);
		goto err_request_irq;
	}

	ret = request_threaded_irq(chip->alert_irq, NULL,
				max17048_interrupt_handler,
				IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				"MAX17048_Alert", chip);
	if (ret) {
		pr_err("%s: IRQ Request Failed : return %d\n",
				__func__, ret);
		goto err_request_irq;
	}

	ret = enable_irq_wake(chip->alert_irq);
	if (ret) {
		pr_err("%s: set irq to wakeup source failed.\n", __func__);
		goto err_request_wakeup_irq;
	}

	disable_irq(chip->alert_irq);

	ret = device_create_file(&client->dev, &dev_attr_fuelrst);
	if (ret) {
		pr_err("%s: fuelrst creation failed: %d\n", __func__, ret);
		ret = -ENODEV;
		goto err_create_file_fuelrst;
	}

	ret = max17048_create_debugfs_entries(chip);
	if (ret) {
		pr_err("max17048_create_debugfs_entries failed\n");
		goto err_create_debugfs;
	}

	ret = max17048_hw_init(chip);
	if (ret) {
		pr_err("%s: failed to init hw.\n", __func__);
		goto err_hw_init;
	}

	chip->pm_notifier.notifier_call = max17048_pm_notifier;
	ret = register_pm_notifier(&chip->pm_notifier);
	if (ret) {
		pr_err("%s: failed to register pm notifier\n", __func__);
		goto err_hw_init;
	}

	schedule_delayed_work(&chip->monitor_work, 0);
	enable_irq(chip->alert_irq);

	pr_info("%s: max17048 probed\n", __func__);
	return 0;

err_hw_init:
	debugfs_remove_recursive(chip->dent);
err_create_debugfs:
	device_remove_file(&client->dev, &dev_attr_fuelrst);
err_create_file_fuelrst:
	disable_irq_wake(chip->alert_irq);
err_request_wakeup_irq:
	free_irq(chip->alert_irq, NULL);
err_request_irq:
	gpio_free(chip->alert_gpio);
err_gpio_request:
	wake_lock_destroy(&chip->alert_lock);
	power_supply_unregister(&chip->batt_psy);
error:
	ref = NULL;
	kfree(chip);
	return ret;
}

static int max17048_remove(struct i2c_client *client)
{
	struct max17048_chip *chip = i2c_get_clientdata(client);

	unregister_pm_notifier(&chip->pm_notifier);
	debugfs_remove_recursive(chip->dent);
	device_remove_file(&client->dev, &dev_attr_fuelrst);
	disable_irq_wake(chip->alert_irq);
	free_irq(chip->alert_irq, NULL);
	gpio_free(chip->alert_gpio);
	wake_lock_destroy(&chip->alert_lock);
	power_supply_unregister(&chip->batt_psy);
	ref = NULL;
	kfree(chip);

	return 0;
}

static void max17048_shutdown(struct i2c_client *client)
{
	struct max17048_chip *chip = i2c_get_clientdata(client);
	int ret;

	ret = max17048_write_word(chip->client, HIBRT_REG, 0xFFFF);
	if (ret)
		pr_warn("%s: hibernate mode enable failed", __func__);

	ret = max17048_read_word(chip->client, HIBRT_REG);
	pr_debug("%s: HIBRT_REG: 0x%04X\n", __func__, ret);
}

static struct of_device_id max17048_match_table[] = {
	{ .compatible = "maxim,max17048", },
	{ },
};

static const struct i2c_device_id max17048_id[] = {
	{ "max17048", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, max17048_id);

static struct i2c_driver max17048_i2c_driver = {
	.driver	= {
		.name = "max17048",
		.owner = THIS_MODULE,
		.of_match_table = max17048_match_table,
	},
	.probe = max17048_probe,
	.remove = max17048_remove,
	.id_table = max17048_id,
	.shutdown  = max17048_shutdown,
};

static int __init max17048_init(void)
{
	return i2c_add_driver(&max17048_i2c_driver);
}
module_init(max17048_init);

static void __exit max17048_exit(void)
{
	i2c_del_driver(&max17048_i2c_driver);
}
module_exit(max17048_exit);

MODULE_DESCRIPTION("MAX17048 Fuel Gauge");
MODULE_LICENSE("GPL");
