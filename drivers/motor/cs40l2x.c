/*
 * cs40l2x.c -- CS40L20/CS40L25/CS40L25A/CS40L25B Haptics Driver
 *
 * Copyright 2018 Cirrus Logic, Inc.
 *
 * Author: Jeff LaBundy <jeff.labundy@cirrus.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#ifdef CONFIG_CS40L2X_SAMSUNG_FEATURE
#define pr_fmt(fmt) "[VIB] " fmt
#endif
#define DEBUG_VIB 0

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <linux/regulator/consumer.h>
#include <linux/regmap.h>
#include <linux/sysfs.h>
#include <linux/firmware.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/platform_data/cs40l2x.h>
#ifdef CONFIG_FOLDER_HALL
#include <linux/hall.h>
#endif

#include "cs40l2x-private.h"

#ifdef CONFIG_ANDROID_TIMED_OUTPUT
#include "timed_output.h"
#else
#include <linux/leds.h>
#endif /* CONFIG_ANDROID_TIMED_OUTPUT */

struct cs40l2x_private {
	struct device *dev;
	struct regmap *regmap;
	struct regulator_bulk_data supplies[2];
	unsigned int num_supplies;
	unsigned int devid;
	unsigned int revid;
	struct work_struct vibe_start_work;
	struct work_struct vibe_pbq_work;
	struct work_struct vibe_stop_work;
	struct work_struct vibe_mode_work;
	struct workqueue_struct *vibe_workqueue;
	struct mutex lock;
	unsigned int cp_trigger_index;
	unsigned int cp_trailer_index;
	unsigned int num_waves;
	unsigned int wt_limit_xm;
	unsigned int wt_limit_ym;
	bool exc_available;
	struct cs40l2x_dblk_desc pre_dblks[CS40L2X_MAX_A2H_LEVELS];
	struct cs40l2x_dblk_desc a2h_dblks[CS40L2X_MAX_A2H_LEVELS];
	unsigned int num_a2h_levels;
	int a2h_level;
	bool vibe_init_success;
	bool vibe_mode;
	struct gpio_desc *reset_gpio;
#ifdef CONFIG_MOTOR_DRV_CS40L2X_PMIC_RESET
	struct regulator *reset_vldo;
#endif
	struct cs40l2x_platform_data pdata;
	unsigned int num_algos;
	struct cs40l2x_algo_info algo_info[CS40L2X_NUM_ALGOS_MAX + 1];
	struct list_head coeff_desc_head;
	unsigned int num_coeff_files;
	unsigned int diag_state;
	unsigned int diag_dig_scale;
	unsigned int f0_measured;
	unsigned int redc_measured;
	unsigned int q_measured;
#ifdef CONFIG_CS40L2X_SAMSUNG_FEATURE
	unsigned int intensity;
	unsigned int force_touch_intensity;
#endif
	struct cs40l2x_pbq_pair pbq_pairs[CS40L2X_PBQ_DEPTH_MAX];
	struct hrtimer pbq_timer;
	unsigned int pbq_depth;
	unsigned int pbq_index;
	unsigned int pbq_state;
	unsigned int pbq_cp_dig_scale;
	int pbq_repeat;
	int pbq_remain;
	struct cs40l2x_wseq_pair wseq_table[CS40L2X_WSEQ_LENGTH_MAX];
	unsigned int wseq_length;
	unsigned int event_control;
	unsigned int hw_err_mask;
	unsigned int peak_gpio1_enable;
	int vpp_measured;
	int ipp_measured;
	bool asp_available;
	bool asp_enable;
	struct hrtimer asp_timer;
	const struct cs40l2x_fw_desc *fw_desc;
	bool comp_enable_pend;
	bool comp_enable;
	bool comp_enable_redc;
	bool comp_enable_f0;
#ifdef CONFIG_ANDROID_TIMED_OUTPUT
	struct timed_output_dev timed_dev;
	struct hrtimer vibe_timer;
	int vibe_timeout;
#else
	struct led_classdev led_dev;
#endif /* CONFIG_ANDROID_TIMED_OUTPUT */
#ifdef CONFIG_FOLDER_HALL
	int flip_status;
	int flip_status_current;
	struct delayed_work switch_work;
	struct notifier_block hall_ic_nb;
#endif
};

static const char * const cs40l2x_supplies[] = {
	"VA",
	"VP",
};

static const char * const cs40l2x_part_nums[] = {
	"CS40L20",
	"CS40L25",
	"CS40L25A",
	"CS40L25B",
};

static const char * const cs40l2x_event_regs[] = {
	"GPIO1EVENT",
	"GPIO2EVENT",
	"GPIO3EVENT",
	"GPIO4EVENT",
	"GPIOPLAYBACKEVENT",
	"TRIGGERPLAYBACKEVENT",
	"RXREADYEVENT",
	"HARDWAREEVENT",
};

static const unsigned int cs40l2x_event_masks[] = {
	CS40L2X_EVENT_GPIO1_ENABLED,
	CS40L2X_EVENT_GPIO2_ENABLED,
	CS40L2X_EVENT_GPIO3_ENABLED,
	CS40L2X_EVENT_GPIO4_ENABLED,
	CS40L2X_EVENT_START_ENABLED | CS40L2X_EVENT_END_ENABLED,
	CS40L2X_EVENT_START_ENABLED | CS40L2X_EVENT_END_ENABLED,
	CS40L2X_EVENT_READY_ENABLED,
	CS40L2X_EVENT_HARDWARE_ENABLED,
};

#ifdef CONFIG_FOLDER_HALL
#define VIB_STATUS_FOLDING		1	//close
#define VIB_STATUS_UNFOLDING		0	//open

#define VIB_UNFOLDING_DIG_SCALE		50
#define VIB_FOLDING_DIG_SCALE		0
#define HALL_IC_PATH		"/sys/class/sec/sec_flip/flipStatus"

extern void hall_ic_register_notify(struct notifier_block *nb);
static void cs40l2x_switch_work(struct work_struct *work);
static int cs40l2x_hall_ic_notify(struct notifier_block *nb, unsigned long status, void *v);
#endif

static int cs40l2x_raw_write(struct cs40l2x_private *cs40l2x, unsigned int reg,
			const void *val, size_t val_len, size_t limit);
static int cs40l2x_ack_write(struct cs40l2x_private *cs40l2x, unsigned int reg,
			unsigned int val);

static int cs40l2x_basic_mode_exit(struct cs40l2x_private *cs40l2x);

static int cs40l2x_firmware_swap(struct cs40l2x_private *cs40l2x,
			unsigned int fw_id);

#ifdef CONFIG_MOTOR_DRV_CS40L2X_PMIC_RESET
static int regulator_vldo_control(struct cs40l2x_private *cs40l2x, int on) {
	struct regulator *vldo = cs40l2x->reset_vldo;
	int ret = 0, voltage = 0;
	
	if (IS_ERR(vldo)) {
		pr_err("%s: can't request VLDO power supply: %ld\n",
			__func__, PTR_ERR(vldo));
		return -EINVAL;
	}

	if(on) {
		if (regulator_is_enabled(vldo)) {
			pr_err("%s: power regulator is enabled\n", __func__);
			return 0;
		}
		else {
			ret = regulator_enable(vldo);
		}
			
	}
	else {
		if (regulator_is_enabled(vldo))
			ret = regulator_disable(vldo);
		else {
			pr_err("%s: power regulator is disabled\n", __func__);
			return 0;
		}
	}

	if (ret) {
		pr_err("%s: failed to set regulator cmd:%s, ret:%d\n", __func__, on ? "on" : "off", ret);
		return -EINVAL;
	} else {
		voltage = regulator_get_voltage(vldo);
		pr_info("%s: set cmd:%s voltage:%d\n", __func__, on ? "on" : "off", voltage);
	}

	return 0;
}

static int cs40l2x_reset_control(struct cs40l2x_private *cs40l2x, int on) {
	int ret = 0;

	if(cs40l2x == NULL || (on < 0 && on > 1)) {
		pr_err("%s: can't request reset control: cmd:%s\n", __func__, on ? "on" : "off");
		return -EINVAL;
	}

	if(cs40l2x->reset_gpio != NULL) {
		pr_info("%s: reset gpio control cmd:%s\n", __func__, on ? "on" : "off");
		gpiod_set_value_cansleep(cs40l2x->reset_gpio, on);
	}
	else if(cs40l2x->reset_vldo != NULL) {
		ret = regulator_vldo_control(cs40l2x, on);
	} else {
		pr_err("%s: reset pin has NULL pointer cmd:%s\n", __func__, on ? "on" : "off");
		return -EACCES;
	}

	return ret;
}
#endif
#if defined(CONFIG_SEC_FACTORY) && defined(CONFIG_MOTOR_DRV_CS40L2X_PMIC_RESET)
static int regulator_vldo_get_value(struct cs40l2x_private *cs40l2x) {

	if (IS_ERR_OR_NULL(cs40l2x->reset_vldo)) {
		pr_err("%s: can't request VLDO power supply: %ld\n",
			__func__, PTR_ERR(cs40l2x->reset_vldo));
		return -EINVAL;
	}

	return regulator_is_enabled(cs40l2x->reset_vldo);
}
#endif

static const struct cs40l2x_fw_desc *cs40l2x_firmware_match(
			struct cs40l2x_private *cs40l2x, unsigned int fw_id)
{
	int i;

	for (i = 0; i < CS40L2X_NUM_FW_FAMS; i++)
		if (cs40l2x_fw_fam[i].id == fw_id)
			return &cs40l2x_fw_fam[i];

	dev_err(cs40l2x->dev, "No matching firmware for ID 0x%06X\n", fw_id);

	return NULL;
}

static struct cs40l2x_private *cs40l2x_get_private(struct device *dev)
{
#ifdef CONFIG_ANDROID_TIMED_OUTPUT
	/* timed output device does not register under a parent device */
	return container_of(dev_get_drvdata(dev),
			struct cs40l2x_private, timed_dev);
#else
	return dev_get_drvdata(dev);
#endif /* CONFIG_ANDROID_TIMED_OUTPUT */
}

static ssize_t cs40l2x_cp_trigger_index_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	unsigned int index;

	mutex_lock(&cs40l2x->lock);
	index = cs40l2x->cp_trigger_index;
	mutex_unlock(&cs40l2x->lock);

	return snprintf(buf, PAGE_SIZE, "%d\n", index);
}

static ssize_t cs40l2x_cp_trigger_index_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int index;

	ret = kstrtou32(buf, 10, &index);
	if (ret)
		return -EINVAL;

#ifdef CONFIG_CS40L2X_SAMSUNG_FEATURE
	pr_info("%s index:%u (num_waves:%u)\n", __func__, index, cs40l2x->num_waves);	
#endif

	mutex_lock(&cs40l2x->lock);

	switch (index) {
	case CS40L2X_INDEX_QEST:
		if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_ORIG) {
			ret = -EPERM;
			break;
		}
		/* intentionally fall through */
	case CS40L2X_INDEX_DIAG:
		if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_REMAP)
			ret = cs40l2x_firmware_swap(cs40l2x,
					CS40L2X_FW_ID_CAL);
		break;
	case CS40L2X_INDEX_PEAK:
	case CS40L2X_INDEX_PBQ:
		if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_CAL)
			ret = cs40l2x_firmware_swap(cs40l2x,
					CS40L2X_FW_ID_REMAP);
		break;
	case CS40L2X_INDEX_IDLE:
		ret = -EINVAL;
		break;
	default:
		if ((index & CS40L2X_INDEX_MASK) >= cs40l2x->num_waves) {
			ret = -EINVAL;
			break;
		}

		if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_CAL)
			ret = cs40l2x_firmware_swap(cs40l2x,
					CS40L2X_FW_ID_REMAP);
	}
	if (ret)
		goto err_mutex;

	cs40l2x->cp_trigger_index = index;
	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_cp_trigger_queue_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	ssize_t len = 0;
	int i;

	if (cs40l2x->pbq_depth == 0)
		return -ENODATA;

	mutex_lock(&cs40l2x->lock);

	for (i = 0; i < cs40l2x->pbq_depth; i++) {
		switch (cs40l2x->pbq_pairs[i].tag) {
		case CS40L2X_PBQ_TAG_SILENCE:
			len += snprintf(buf + len, PAGE_SIZE - len, "%d",
					cs40l2x->pbq_pairs[i].mag);
			break;
		case CS40L2X_PBQ_TAG_START:
			len += snprintf(buf + len, PAGE_SIZE - len, "!!");
			break;
		case CS40L2X_PBQ_TAG_STOP:
			len += snprintf(buf + len, PAGE_SIZE - len, "%d!!",
					cs40l2x->pbq_pairs[i].repeat);
			break;
		default:
			len += snprintf(buf + len, PAGE_SIZE - len, "%d.%d",
					cs40l2x->pbq_pairs[i].tag,
					cs40l2x->pbq_pairs[i].mag);
		}

		if (i < (cs40l2x->pbq_depth - 1))
			len += snprintf(buf + len, PAGE_SIZE - len, ", ");
	}

	switch (cs40l2x->pbq_repeat) {
	case -1:
		len += snprintf(buf + len, PAGE_SIZE - len, ", ~\n");
		break;
	case 0:
		len += snprintf(buf + len, PAGE_SIZE - len, "\n");
		break;
	default:
		len += snprintf(buf + len, PAGE_SIZE - len, ", %d!\n",
				cs40l2x->pbq_repeat);
	}

	mutex_unlock(&cs40l2x->lock);

	return len;
}

static ssize_t cs40l2x_cp_trigger_queue_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	char *pbq_str_alloc, *pbq_str, *pbq_str_tok;
	char *pbq_seg_alloc, *pbq_seg, *pbq_seg_tok;
	size_t pbq_seg_len;
	unsigned int pbq_depth = 0;
	unsigned int val, num_empty;
	int pbq_marker = -1;
	int ret;

	pbq_str_alloc = kzalloc(count, GFP_KERNEL);
	if (!pbq_str_alloc)
		return -ENOMEM;

	pbq_seg_alloc = kzalloc(CS40L2X_PBQ_SEG_LEN_MAX + 1, GFP_KERNEL);
	if (!pbq_seg_alloc) {
		kfree(pbq_str_alloc);
		return -ENOMEM;
	}

	mutex_lock(&cs40l2x->lock);

	cs40l2x->pbq_depth = 0;
	cs40l2x->pbq_repeat = 0;

	pbq_str = pbq_str_alloc;
	strlcpy(pbq_str, buf, count);

#if DEBUG_VIB
	pr_info("%s pbq_str=%s\n", __func__, pbq_str);
#endif

	pbq_str_tok = strsep(&pbq_str, ",");

	while (pbq_str_tok) {
		pbq_seg = pbq_seg_alloc;
		pbq_seg_len = strlcpy(pbq_seg, strim(pbq_str_tok),
				CS40L2X_PBQ_SEG_LEN_MAX + 1);
		if (pbq_seg_len > CS40L2X_PBQ_SEG_LEN_MAX) {
			ret = -E2BIG;
			goto err_mutex;
		}

		/* waveform specifier */
		if (strnchr(pbq_seg, CS40L2X_PBQ_SEG_LEN_MAX, '.')) {
			/* index */
			pbq_seg_tok = strsep(&pbq_seg, ".");

			ret = kstrtou32(pbq_seg_tok, 10, &val);
			if (ret) {
				ret = -EINVAL;
				goto err_mutex;
			}
			if (val == 0 || val >= cs40l2x->num_waves) {
				ret = -EINVAL;
				goto err_mutex;
			}
			cs40l2x->pbq_pairs[pbq_depth].tag = val;

			/* scale */
			pbq_seg_tok = strsep(&pbq_seg, ".");

			ret = kstrtou32(pbq_seg_tok, 10, &val);
			if (ret) {
				ret = -EINVAL;
				goto err_mutex;
			}
			if (val == 0 || val > CS40L2X_PBQ_SCALE_MAX) {
				ret = -EINVAL;
				goto err_mutex;
			}
			cs40l2x->pbq_pairs[pbq_depth++].mag = val;

		/* repetition specifier */
		} else if (strnchr(pbq_seg, CS40L2X_PBQ_SEG_LEN_MAX, '!')) {
			val = 0;
			num_empty = 0;

			pbq_seg_tok = strsep(&pbq_seg, "!");

			while (pbq_seg_tok) {
				if (strnlen(pbq_seg_tok,
						CS40L2X_PBQ_SEG_LEN_MAX)) {
					ret = kstrtou32(pbq_seg_tok, 10, &val);
					if (ret) {
						ret = -EINVAL;
						goto err_mutex;
					}
					if (val > CS40L2X_PBQ_REPEAT_MAX) {
						ret = -EINVAL;
						goto err_mutex;
					}
				} else {
					num_empty++;
				}

				pbq_seg_tok = strsep(&pbq_seg, "!");
			}

			/* number of empty tokens reveals specifier type */
			switch (num_empty) {
			case 1:	/* outer loop: "n!" or "!n" */
				if (cs40l2x->pbq_repeat) {
					ret = -EINVAL;
					goto err_mutex;
				}
				cs40l2x->pbq_repeat = val;
				break;

			case 2:	/* inner loop stop: "n!!" or "!!n" */
				if (pbq_marker < 0) {
					ret = -EINVAL;
					goto err_mutex;
				}

				cs40l2x->pbq_pairs[pbq_depth].tag =
						CS40L2X_PBQ_TAG_STOP;
				cs40l2x->pbq_pairs[pbq_depth].mag = pbq_marker;
				cs40l2x->pbq_pairs[pbq_depth++].repeat = val;
				pbq_marker = -1;
				break;

			case 3:	/* inner loop start: "!!" */
				if (pbq_marker >= 0) {
					ret = -EINVAL;
					goto err_mutex;
				}

				cs40l2x->pbq_pairs[pbq_depth].tag =
						CS40L2X_PBQ_TAG_START;
				pbq_marker = pbq_depth++;
				break;

			default:
				ret = -EINVAL;
				goto err_mutex;
			}

		/* loop specifier */
		} else if (strnchr(pbq_seg, CS40L2X_PBQ_SEG_LEN_MAX, '~')) {
			if (cs40l2x->pbq_repeat) {
				ret = -EINVAL;
				goto err_mutex;
			}
			cs40l2x->pbq_repeat = -1;

		/* duration specifier */
		} else {
			cs40l2x->pbq_pairs[pbq_depth].tag =
					CS40L2X_PBQ_TAG_SILENCE;

			ret = kstrtou32(pbq_seg, 10, &val);
			if (ret) {
				ret = -EINVAL;
				goto err_mutex;
			}
			if (val > CS40L2X_PBQ_DELAY_MAX) {
				ret = -EINVAL;
				goto err_mutex;
			}
			cs40l2x->pbq_pairs[pbq_depth++].mag = val;
		}

		if (pbq_depth == CS40L2X_PBQ_DEPTH_MAX) {
			ret = -E2BIG;
			goto err_mutex;
		}

		pbq_str_tok = strsep(&pbq_str, ",");
	}

#ifdef CONFIG_CS40L2X_SAMSUNG_FEATURE
	if(count > DEBUG_PRINT_PLAYBACK_QUEUE) {
		strlcpy(pbq_str_alloc, buf, DEBUG_PRINT_PLAYBACK_QUEUE);
		pr_info("%s pbq_str=%s... (depth:%d)\n", __func__, pbq_str_alloc, pbq_depth);
	} else {
		strlcpy(pbq_str_alloc, buf, count);
		pr_info("%s pbq_str=%s (depth:%d)\n", __func__, pbq_str_alloc, pbq_depth);
	}
#endif
	cs40l2x->pbq_depth = pbq_depth;
	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	kfree(pbq_str_alloc);
	kfree(pbq_seg_alloc);

	return ret;
}

static unsigned int cs40l2x_dsp_reg(struct cs40l2x_private *cs40l2x,
			const char *coeff_name, const unsigned char block_type)
{
	struct cs40l2x_coeff_desc *coeff_desc;

	list_for_each_entry(coeff_desc, &cs40l2x->coeff_desc_head, list) {
		if (strcmp(coeff_desc->name, coeff_name))
			continue;
		if (coeff_desc->block_type != block_type)
			continue;

		return coeff_desc->reg;
	}

	/* return an identifiable register that is known to be read-only */
	return CS40L2X_DEVID;
}

static int cs40l2x_wseq_add_reg(struct cs40l2x_private *cs40l2x,
			unsigned int reg, unsigned int val)
{
	if (cs40l2x->wseq_length == CS40L2X_WSEQ_LENGTH_MAX)
		return -E2BIG;

	cs40l2x->wseq_table[cs40l2x->wseq_length].reg = reg;
	cs40l2x->wseq_table[cs40l2x->wseq_length++].val = val;

	return 0;
}

static int cs40l2x_wseq_add_seq(struct cs40l2x_private *cs40l2x,
			const struct reg_sequence *seq, unsigned int len)
{
	int ret, i;

	for (i = 0; i < len; i++) {
		ret = cs40l2x_wseq_add_reg(cs40l2x, seq[i].reg, seq[i].def);
		if (ret)
			return ret;
	}

	return 0;
}

static int cs40l2x_wseq_write(struct cs40l2x_private *cs40l2x, unsigned int pos,
			unsigned int reg, unsigned int val)
{
	unsigned int wseq_base = cs40l2x_dsp_reg(cs40l2x, "POWERONSEQUENCE",
			CS40L2X_XM_UNPACKED_TYPE);
	int ret;

	/* upper half */
	ret = regmap_write(cs40l2x->regmap,
			wseq_base + pos * CS40L2X_WSEQ_STRIDE,
			((reg & CS40L2X_WSEQ_REG_MASK1)
				<< CS40L2X_WSEQ_REG_SHIFTUP) |
					((val & CS40L2X_WSEQ_VAL_MASK1)
						>> CS40L2X_WSEQ_VAL_SHIFTDN));
	if (ret)
		return ret;

	/* lower half */
	return regmap_write(cs40l2x->regmap,
			wseq_base + pos * CS40L2X_WSEQ_STRIDE + 4,
			val & CS40L2X_WSEQ_VAL_MASK2);
}

static int cs40l2x_wseq_init(struct cs40l2x_private *cs40l2x)
{
	unsigned int wseq_base = cs40l2x_dsp_reg(cs40l2x, "POWERONSEQUENCE",
			CS40L2X_XM_UNPACKED_TYPE);
	int ret, i;

	for (i = 0; i < cs40l2x->wseq_length; i++) {
		ret = cs40l2x_wseq_write(cs40l2x, i,
				cs40l2x->wseq_table[i].reg,
				cs40l2x->wseq_table[i].val);
		if (ret)
			return ret;
	}

	return regmap_write(cs40l2x->regmap,
			wseq_base + cs40l2x->wseq_length * CS40L2X_WSEQ_STRIDE,
			CS40L2X_WSEQ_LIST_TERM);
}

static int cs40l2x_wseq_replace(struct cs40l2x_private *cs40l2x,
			unsigned int reg, unsigned int val)
{
	int i;

	for (i = 0; i < cs40l2x->wseq_length; i++)
		if (cs40l2x->wseq_table[i].reg == reg)
			break;

	if (i == cs40l2x->wseq_length)
		return -EINVAL;

	cs40l2x->wseq_table[i].val = val;

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP)
		return 0;

	return cs40l2x_wseq_write(cs40l2x, i, reg, val);
}

static int cs40l2x_user_ctrl_exec(struct cs40l2x_private *cs40l2x,
			unsigned int user_ctrl_cmd, unsigned int user_ctrl_data,
			unsigned int *user_ctrl_resp)
{
	struct regmap *regmap = cs40l2x->regmap;
	struct device *dev = cs40l2x->dev;
	unsigned int val;
	int ret, i;

	ret = regmap_write(regmap,
			cs40l2x_dsp_reg(cs40l2x, "USER_CONTROL_IPDATA",
					CS40L2X_XM_UNPACKED_TYPE),
			user_ctrl_data);
	if (ret) {
		dev_err(dev, "Failed to write user-control data\n");
		return ret;
	}

	ret = regmap_write(regmap, CS40L2X_MBOX_USER_CONTROL, user_ctrl_cmd);
	if (ret) {
		dev_err(dev, "Failed to write user-control command\n");
		return ret;
	}

	for (i = 0; i < CS40L2X_UCTRL_TIMEOUT_COUNT; i++) {
		ret = regmap_read(regmap, CS40L2X_MBOX_USER_CONTROL, &val);
		if (ret) {
			dev_err(dev, "Failed to read user-control command\n");
			return ret;
		}

		if (val != user_ctrl_cmd)
			break;

		usleep_range(1000, 1100);
	}

	if (i == CS40L2X_UCTRL_TIMEOUT_COUNT) {
		dev_err(dev, "Timed out waiting for user-control response\n");
		return -ETIME;
	}

	switch (val) {
	case CS40L2X_USER_CTRL_SUCCESS:
		break;
	case CS40L2X_USER_CTRL_UNKNOWN:
		dev_err(dev, "Unrecognized user-control command\n");
		return -EINVAL;
	case CS40L2X_USER_CTRL_INVALID:
		dev_err(dev, "Invalid user-control operation\n");
		return -EINVAL;
	case CS40L2X_USER_CTRL_ERROR:
		dev_err(dev, "User-control output error\n");
		return -EIO;
	default:
		dev_err(dev, "Unrecognized user-control response: %d\n", val);
		return -EINVAL;
	}

	if (!user_ctrl_resp)
		return 0;

	ret = regmap_read(regmap,
			cs40l2x_dsp_reg(cs40l2x, "USER_CONTROL_RESPONSE",
					CS40L2X_XM_UNPACKED_TYPE),
			&val);
	if (ret) {
		dev_err(dev, "Failed to read user-control response\n");
		return ret;
	}

	*user_ctrl_resp = val;

	return 0;
}

static ssize_t cs40l2x_cp_trigger_duration_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int index, val;

	mutex_lock(&cs40l2x->lock);

	index = cs40l2x->cp_trigger_index;

	switch (cs40l2x->fw_desc->id) {
	case CS40L2X_FW_ID_REMAP:
		if (index < CS40L2X_INDEX_CLICK_MIN
				|| index > CS40L2X_INDEX_CLICK_MAX) {
			ret = -EINVAL;
			goto err_mutex;
		}

		ret = cs40l2x_user_ctrl_exec(cs40l2x,
				CS40L2X_USER_CTRL_DURATION, index, &val);
		if (ret)
			goto err_mutex;
		break;
	case CS40L2X_FW_ID_CAL:
		if (index != CS40L2X_INDEX_QEST) {
			ret = -EINVAL;
			goto err_mutex;
		}

		if (cs40l2x->diag_state < CS40L2X_DIAG_STATE_DONE1) {
			ret = -ENODATA;
			goto err_mutex;
		}

		ret = regmap_read(cs40l2x->regmap,
				cs40l2x_dsp_reg(cs40l2x, "TONE_DURATION_MS",
						CS40L2X_XM_UNPACKED_TYPE),
				&val);
		if (ret)
			goto err_mutex;

		if (val == CS40L2X_TONE_DURATION_MS_NONE) {
			ret = -ENODATA;
			goto err_mutex;
		}

		val *= CS40L2X_QEST_SRATE;
		break;
	default:
		ret = -EPERM;
		goto err_mutex;
	}

	ret = snprintf(buf, PAGE_SIZE, "%d\n", val);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static int cs40l2x_hiber_cmd_send(struct cs40l2x_private *cs40l2x,
			unsigned int hiber_cmd)
{
	int ret, i;
	unsigned int val;

	switch (hiber_cmd) {
	case CS40L2X_POWERCONTROL_NONE:
	case CS40L2X_POWERCONTROL_HIBERNATE:
		return regmap_write(cs40l2x->regmap,
				CS40L2X_MBOX_POWERCONTROL,
				hiber_cmd);

	case CS40L2X_POWERCONTROL_WAKEUP:
		for (i = 0; i < CS40L2X_WAKEUP_RETRIES; i++) {
			/*
			 * the first several transactions are expected to be
			 * NAK'd, so retry multiple times in rapid succession
			 */
			ret = regmap_write(cs40l2x->regmap,
					CS40L2X_MBOX_POWERCONTROL,
					hiber_cmd);
			if (!ret)
				break;

			usleep_range(1000, 1100);
		}
		if (i == CS40L2X_WAKEUP_RETRIES)
			return -EIO;

		for (i = 0; i < CS40L2X_WAKEUP_RETRIES; i++) {
			usleep_range(5000, 5100);

			ret = regmap_read(cs40l2x->regmap,
					cs40l2x_dsp_reg(cs40l2x, "POWERSTATE",
						CS40L2X_XM_UNPACKED_TYPE),
					&val);
			if (ret)
				return ret;

			if (val != CS40L2X_POWERSTATE_HIBERNATE)
				return 0;
		}
		return -ETIME;

	case CS40L2X_POWERCONTROL_FRC_STDBY:
		ret = regmap_write(cs40l2x->regmap,
				CS40L2X_MBOX_POWERCONTROL,
				hiber_cmd);
		if (ret)
			return ret;

		for (i = 0; i < CS40L2X_WAKEUP_RETRIES; i++) {
			ret = regmap_read(cs40l2x->regmap,
					CS40L2X_MBOX_POWERCONTROL,
					&val);
			if (ret)
				return ret;

			if (val == CS40L2X_POWERCONTROL_NONE)
				return 0;

			usleep_range(1000, 1100);
		}
		return -ETIME;

	default:
		return -EINVAL;
	}
}

static ssize_t cs40l2x_hiber_cmd_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int hiber_cmd;

	ret = kstrtou32(buf, 10, &hiber_cmd);
	if (ret)
		return -EINVAL;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = cs40l2x_hiber_cmd_send(cs40l2x, hiber_cmd);
	if (ret)
		goto err_mutex;

	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_hiber_timeout_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_read(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "FALSEI2CTIMEOUT",
					CS40L2X_XM_UNPACKED_TYPE), &val);
	if (ret)
		goto err_mutex;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", val);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_hiber_timeout_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val;

	ret = kstrtou32(buf, 10, &val);
	if (ret)
		return -EINVAL;

	if (val < CS40L2X_FALSEI2CTIMEOUT_MIN)
		return -EINVAL;

	if (val > CS40L2X_FALSEI2CTIMEOUT_MAX)
		return -EINVAL;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_write(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "FALSEI2CTIMEOUT",
					CS40L2X_XM_UNPACKED_TYPE), val);
	if (ret)
		goto err_mutex;

	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_gpio1_enable_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_CAL) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_read(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "GPIO_ENABLE",
					CS40L2X_XM_UNPACKED_TYPE), &val);
	if (ret)
		goto err_mutex;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", val);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_gpio1_enable_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val;

	ret = kstrtou32(buf, 10, &val);
	if (ret)
		return -EINVAL;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_CAL) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_write(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "GPIO_ENABLE",
					CS40L2X_XM_UNPACKED_TYPE),
			val ? CS40L2X_GPIO1_ENABLED : CS40L2X_GPIO1_DISABLED);
	if (ret)
		goto err_mutex;

	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_gpio1_rise_index_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int index;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_CAL) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_read(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "INDEXBUTTONPRESS",
					CS40L2X_XM_UNPACKED_TYPE)
						+ CS40L2X_INDEXBUTTONPRESS1,
			&index);
	if (ret)
		goto err_mutex;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", index);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_gpio1_rise_index_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int index;

	ret = kstrtou32(buf, 10, &index);
	if (ret)
		return -EINVAL;

#if DEBUG_VIB
	pr_info("%s index:%u (num_waves:%u)\n", __func__, index, cs40l2x->num_waves);
#endif

	mutex_lock(&cs40l2x->lock);

	if (index > (cs40l2x->num_waves - 1)) {
		ret = -EINVAL;
		goto err_mutex;
	}

	if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_CAL) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_write(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "INDEXBUTTONPRESS",
					CS40L2X_XM_UNPACKED_TYPE)
						+ CS40L2X_INDEXBUTTONPRESS1,
			index);
	if (ret)
		goto err_mutex;

	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_gpio1_fall_index_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int index;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_CAL) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_read(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "INDEXBUTTONRELEASE",
					CS40L2X_XM_UNPACKED_TYPE)
					+ CS40L2X_INDEXBUTTONRELEASE1,
					&index);
	if (ret)
		goto err_mutex;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", index);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_gpio1_fall_index_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int index;

	ret = kstrtou32(buf, 10, &index);
	if (ret)
		return -EINVAL;

#if DEBUG_VIB
	pr_info("%s index:%u (num_waves:%u)\n", __func__, index, cs40l2x->num_waves);
#endif

	mutex_lock(&cs40l2x->lock);

	if (index > (cs40l2x->num_waves - 1)) {
		ret = -EINVAL;
		goto err_mutex;
	}

	if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_CAL) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_write(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "INDEXBUTTONRELEASE",
					CS40L2X_XM_UNPACKED_TYPE)
						+ CS40L2X_INDEXBUTTONRELEASE1,
			index);
	if (ret)
		goto err_mutex;

	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_gpio1_fall_timeout_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_CAL) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_read(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "PRESS_RELEASE_TIMEOUT",
					CS40L2X_XM_UNPACKED_TYPE), &val);
	if (ret)
		goto err_mutex;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", val);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_gpio1_fall_timeout_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val;

	ret = kstrtou32(buf, 10, &val);
	if (ret)
		return -EINVAL;

	if (val > CS40L2X_PR_TIMEOUT_MAX)
		return -EINVAL;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_CAL) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_write(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "PRESS_RELEASE_TIMEOUT",
					CS40L2X_XM_UNPACKED_TYPE), val);
	if (ret)
		goto err_mutex;

	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_gpio2_rise_index_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int index;

	if (cs40l2x->devid != CS40L2X_DEVID_L25B)
		return -EPERM;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_read(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "INDEXBUTTONPRESS",
					CS40L2X_XM_UNPACKED_TYPE)
						+ CS40L2X_INDEXBUTTONPRESS2,
			&index);
	if (ret)
		goto err_mutex;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", index);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_gpio2_rise_index_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int index;

	if (cs40l2x->devid != CS40L2X_DEVID_L25B)
		return -EPERM;

	ret = kstrtou32(buf, 10, &index);
	if (ret)
		return -EINVAL;

	mutex_lock(&cs40l2x->lock);

	if (index > (cs40l2x->num_waves - 1)) {
		ret = -EINVAL;
		goto err_mutex;
	}

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_write(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "INDEXBUTTONPRESS",
					CS40L2X_XM_UNPACKED_TYPE)
						+ CS40L2X_INDEXBUTTONPRESS2,
			index);
	if (ret)
		goto err_mutex;

	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_gpio2_fall_index_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int index;

	if (cs40l2x->devid != CS40L2X_DEVID_L25B)
		return -EPERM;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_read(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "INDEXBUTTONRELEASE",
					CS40L2X_XM_UNPACKED_TYPE)
						+ CS40L2X_INDEXBUTTONRELEASE2,
			&index);
	if (ret)
		goto err_mutex;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", index);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_gpio2_fall_index_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int index;

	if (cs40l2x->devid != CS40L2X_DEVID_L25B)
		return -EPERM;

	ret = kstrtou32(buf, 10, &index);
	if (ret)
		return -EINVAL;

	mutex_lock(&cs40l2x->lock);

	if (index > (cs40l2x->num_waves - 1)) {
		ret = -EINVAL;
		goto err_mutex;
	}

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_write(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "INDEXBUTTONRELEASE",
					CS40L2X_XM_UNPACKED_TYPE)
						+ CS40L2X_INDEXBUTTONRELEASE2,
			index);
	if (ret)
		goto err_mutex;

	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_gpio3_rise_index_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int index;

	if (cs40l2x->devid != CS40L2X_DEVID_L25B)
		return -EPERM;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_read(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "INDEXBUTTONPRESS",
					CS40L2X_XM_UNPACKED_TYPE)
						+ CS40L2X_INDEXBUTTONPRESS3,
			&index);
	if (ret)
		goto err_mutex;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", index);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_gpio3_rise_index_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int index;

	if (cs40l2x->devid != CS40L2X_DEVID_L25B)
		return -EPERM;

	ret = kstrtou32(buf, 10, &index);
	if (ret)
		return -EINVAL;

	mutex_lock(&cs40l2x->lock);

	if (index > (cs40l2x->num_waves - 1)) {
		ret = -EINVAL;
		goto err_mutex;
	}

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_write(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "INDEXBUTTONPRESS",
					CS40L2X_XM_UNPACKED_TYPE)
						+ CS40L2X_INDEXBUTTONPRESS3,
			index);
	if (ret)
		goto err_mutex;

	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_gpio3_fall_index_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int index;

	if (cs40l2x->devid != CS40L2X_DEVID_L25B)
		return -EPERM;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_read(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "INDEXBUTTONRELEASE",
					CS40L2X_XM_UNPACKED_TYPE)
						+ CS40L2X_INDEXBUTTONRELEASE3,
			&index);
	if (ret)
		goto err_mutex;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", index);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_gpio3_fall_index_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int index;

	if (cs40l2x->devid != CS40L2X_DEVID_L25B)
		return -EPERM;

	ret = kstrtou32(buf, 10, &index);
	if (ret)
		return -EINVAL;

	mutex_lock(&cs40l2x->lock);

	if (index > (cs40l2x->num_waves - 1)) {
		ret = -EINVAL;
		goto err_mutex;
	}

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_write(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "INDEXBUTTONRELEASE",
					CS40L2X_XM_UNPACKED_TYPE)
						+ CS40L2X_INDEXBUTTONRELEASE3,
			index);
	if (ret)
		goto err_mutex;

	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_gpio4_rise_index_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int index;

	if (cs40l2x->devid != CS40L2X_DEVID_L25B)
		return -EPERM;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_read(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "INDEXBUTTONPRESS",
					CS40L2X_XM_UNPACKED_TYPE)
						+ CS40L2X_INDEXBUTTONPRESS4,
			&index);
	if (ret)
		goto err_mutex;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", index);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_gpio4_rise_index_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int index;

	if (cs40l2x->devid != CS40L2X_DEVID_L25B)
		return -EPERM;

	ret = kstrtou32(buf, 10, &index);
	if (ret)
		return -EINVAL;

	mutex_lock(&cs40l2x->lock);

	if (index > (cs40l2x->num_waves - 1)) {
		ret = -EINVAL;
		goto err_mutex;
	}

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_write(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "INDEXBUTTONPRESS",
					CS40L2X_XM_UNPACKED_TYPE)
						+ CS40L2X_INDEXBUTTONPRESS4,
			index);
	if (ret)
		goto err_mutex;

	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_gpio4_fall_index_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int index;

	if (cs40l2x->devid != CS40L2X_DEVID_L25B)
		return -EPERM;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_read(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "INDEXBUTTONRELEASE",
					CS40L2X_XM_UNPACKED_TYPE)
						+ CS40L2X_INDEXBUTTONRELEASE4,
			&index);
	if (ret)
		goto err_mutex;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", index);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_gpio4_fall_index_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int index;

	if (cs40l2x->devid != CS40L2X_DEVID_L25B)
		return -EPERM;

	ret = kstrtou32(buf, 10, &index);
	if (ret)
		return -EINVAL;

	mutex_lock(&cs40l2x->lock);

	if (index > (cs40l2x->num_waves - 1)) {
		ret = -EINVAL;
		goto err_mutex;
	}

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_write(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "INDEXBUTTONRELEASE",
					CS40L2X_XM_UNPACKED_TYPE)
						+ CS40L2X_INDEXBUTTONRELEASE4,
			index);
	if (ret)
		goto err_mutex;

	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_standby_timeout_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_CAL) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_read(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "EVENT_TIMEOUT",
					CS40L2X_XM_UNPACKED_TYPE), &val);
	if (ret)
		goto err_mutex;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", val);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_standby_timeout_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val;

	ret = kstrtou32(buf, 10, &val);
	if (ret)
		return -EINVAL;

	if (val > CS40L2X_EVENT_TIMEOUT_MAX)
		return -EINVAL;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_CAL) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_write(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "EVENT_TIMEOUT",
					CS40L2X_XM_UNPACKED_TYPE), val);
	if (ret)
		goto err_mutex;

	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_f0_measured_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->diag_state < CS40L2X_DIAG_STATE_DONE1) {
		ret = -ENODATA;
		goto err_mutex;
	}

	ret = snprintf(buf, PAGE_SIZE, "%d\n", cs40l2x->f0_measured);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_f0_stored_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_CAL) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_read(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "F0_STORED",
					CS40L2X_XM_UNPACKED_TYPE), &val);
	if (ret)
		goto err_mutex;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", val);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_f0_stored_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val;

	ret = kstrtou32(buf, 10, &val);
	if (ret)
		return -EINVAL;

	if (cs40l2x->pdata.f0_min > 0 && val < cs40l2x->pdata.f0_min)
		return -EINVAL;

	if (cs40l2x->pdata.f0_max > 0 && val > cs40l2x->pdata.f0_max)
		return -EINVAL;

#if DEBUG_VIB
	pr_info("%s val:%u\n", __func__, val);
#endif

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_CAL) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_write(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "F0_STORED",
					CS40L2X_XM_UNPACKED_TYPE), val);
	if (ret)
		goto err_mutex;

	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_f0_offset_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_read(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "F0_OFFSET",
					CS40L2X_XM_UNPACKED_TYPE), &val);
	if (ret)
		goto err_mutex;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", val);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_f0_offset_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val;

	ret = kstrtou32(buf, 10, &val);
	if (ret)
		return -EINVAL;

	if (val > CS40L2X_F0_OFFSET_POS_MAX && val < CS40L2X_F0_OFFSET_NEG_MIN)
		return -EINVAL;

	if (val > CS40L2X_F0_OFFSET_NEG_MAX)
		return -EINVAL;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_write(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "F0_OFFSET",
					CS40L2X_XM_UNPACKED_TYPE), val);
	if (ret)
		goto err_mutex;

	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_redc_measured_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->diag_state < CS40L2X_DIAG_STATE_DONE1) {
		ret = -ENODATA;
		goto err_mutex;
	}

	ret = snprintf(buf, PAGE_SIZE, "%d\n", cs40l2x->redc_measured);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_redc_stored_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_CAL) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_read(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "REDC_STORED",
					CS40L2X_XM_UNPACKED_TYPE), &val);
	if (ret)
		goto err_mutex;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", val);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_redc_stored_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val;

	ret = kstrtou32(buf, 10, &val);
	if (ret)
		return -EINVAL;

	if (cs40l2x->pdata.redc_min > 0 && val < cs40l2x->pdata.redc_min)
		return -EINVAL;

	if (cs40l2x->pdata.redc_max > 0 && val > cs40l2x->pdata.redc_max)
		return -EINVAL;

#if DEBUG_VIB
	pr_info("%s val:%u\n", __func__, val);
#endif

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_CAL) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_write(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "REDC_STORED",
					CS40L2X_XM_UNPACKED_TYPE), val);
	if (ret)
		goto err_mutex;

	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_q_measured_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_ORIG) {
		ret = -EPERM;
		goto err_mutex;
	}

	if (cs40l2x->diag_state < CS40L2X_DIAG_STATE_DONE2) {
		ret = -ENODATA;
		goto err_mutex;
	}

	ret = snprintf(buf, PAGE_SIZE, "%d\n", cs40l2x->q_measured);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_q_stored_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_read(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "Q_STORED",
					CS40L2X_XM_UNPACKED_TYPE), &val);
	if (ret)
		goto err_mutex;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", val);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_q_stored_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val;

	ret = kstrtou32(buf, 10, &val);
	if (ret)
		return -EINVAL;

	if (cs40l2x->pdata.q_min > 0 && val < cs40l2x->pdata.q_min)
		return -EINVAL;

	if (cs40l2x->pdata.q_max > 0 && val > cs40l2x->pdata.q_max)
		return -EINVAL;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_write(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "Q_STORED",
					CS40L2X_XM_UNPACKED_TYPE), val);
	if (ret)
		goto err_mutex;

	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_comp_enable_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_CAL) {
		ret = -EPERM;
		goto err_mutex;
	}

	if (cs40l2x->comp_enable_pend) {
		ret = -EIO;
		goto err_mutex;
	}

	ret = snprintf(buf, PAGE_SIZE, "%d\n", cs40l2x->comp_enable);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_comp_enable_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val;

	ret = kstrtou32(buf, 10, &val);
	if (ret)
		return -EINVAL;

#if DEBUG_VIB
	pr_info("%s val:%u\n", __func__, val);
#endif

	mutex_lock(&cs40l2x->lock);

	cs40l2x->comp_enable_pend = true;
	cs40l2x->comp_enable = val > 0;

	switch (cs40l2x->fw_desc->id) {
	case CS40L2X_FW_ID_ORIG:
		ret = regmap_write(cs40l2x->regmap,
				cs40l2x_dsp_reg(cs40l2x, "COMPENSATION_ENABLE",
						CS40L2X_XM_UNPACKED_TYPE),
				cs40l2x->comp_enable);
		break;
	case CS40L2X_FW_ID_REMAP:
		ret = regmap_write(cs40l2x->regmap,
				cs40l2x_dsp_reg(cs40l2x, "COMPENSATION_ENABLE",
						CS40L2X_XM_UNPACKED_TYPE),
				(cs40l2x->comp_enable
					& cs40l2x->comp_enable_redc)
					<< CS40L2X_COMP_EN_REDC_SHIFT |
				(cs40l2x->comp_enable
					& cs40l2x->comp_enable_f0)
					<< CS40L2X_COMP_EN_F0_SHIFT);
		break;
	default:
		ret = -EPERM;
	}

	if (ret)
		goto err_mutex;

	cs40l2x->comp_enable_pend = false;
	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_redc_comp_enable_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	if (cs40l2x->comp_enable_pend) {
		ret = -EIO;
		goto err_mutex;
	}

	ret = snprintf(buf, PAGE_SIZE, "%d\n", cs40l2x->comp_enable_redc);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_redc_comp_enable_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val;

	ret = kstrtou32(buf, 10, &val);
	if (ret)
		return -EINVAL;

	mutex_lock(&cs40l2x->lock);

	cs40l2x->comp_enable_pend = true;
	cs40l2x->comp_enable_redc = val > 0;

	switch (cs40l2x->fw_desc->id) {
	case CS40L2X_FW_ID_REMAP:
		ret = regmap_write(cs40l2x->regmap,
				cs40l2x_dsp_reg(cs40l2x, "COMPENSATION_ENABLE",
						CS40L2X_XM_UNPACKED_TYPE),
				(cs40l2x->comp_enable
					& cs40l2x->comp_enable_redc)
					<< CS40L2X_COMP_EN_REDC_SHIFT |
				(cs40l2x->comp_enable
					& cs40l2x->comp_enable_f0)
					<< CS40L2X_COMP_EN_F0_SHIFT);
		break;
	default:
		ret = -EPERM;
	}

	if (ret)
		goto err_mutex;

	cs40l2x->comp_enable_pend = false;
	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static int cs40l2x_dig_scale_get(struct cs40l2x_private *cs40l2x,
			unsigned int *dig_scale)
{
	int ret;
	unsigned int val;

	ret = regmap_read(cs40l2x->regmap, CS40L2X_AMP_DIG_VOL_CTRL, &val);
	if (ret)
		return ret;

	*dig_scale = (CS40L2X_DIG_SCALE_ZERO - ((val & CS40L2X_AMP_VOL_PCM_MASK)
			>> CS40L2X_AMP_VOL_PCM_SHIFT)) & CS40L2X_DIG_SCALE_MASK;

	return 0;
}

static int cs40l2x_dig_scale_set(struct cs40l2x_private *cs40l2x,
			unsigned int dig_scale)
{
	int ret;
	unsigned int val;

	if (dig_scale == CS40L2X_DIG_SCALE_RESET)
		return -EINVAL;

	ret = regmap_read(cs40l2x->regmap, CS40L2X_AMP_DIG_VOL_CTRL, &val);
	if (ret)
		return ret;

	val &= ~CS40L2X_AMP_VOL_PCM_MASK;
	val |= CS40L2X_AMP_VOL_PCM_MASK &
			(((CS40L2X_DIG_SCALE_ZERO - dig_scale)
			& CS40L2X_DIG_SCALE_MASK) << CS40L2X_AMP_VOL_PCM_SHIFT);

	ret = regmap_write(cs40l2x->regmap, CS40L2X_AMP_DIG_VOL_CTRL, val);
	if (ret)
		return ret;

	return cs40l2x_wseq_replace(cs40l2x, CS40L2X_AMP_DIG_VOL_CTRL, val);
}

#ifdef CIRRUS_VIB_DIG_SCALE_SUPPORT
static ssize_t cs40l2x_dig_scale_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int dig_scale;

	/*
	 * this operation is agnostic to the variable firmware ID and may
	 * therefore be performed without mutex protection
	 */
	ret = cs40l2x_dig_scale_get(cs40l2x, &dig_scale);
	if (ret)
		return ret;

	return snprintf(buf, PAGE_SIZE, "%d\n", dig_scale);
}

static ssize_t cs40l2x_dig_scale_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int dig_scale;

	ret = kstrtou32(buf, 10, &dig_scale);
	if (ret)
		return -EINVAL;

	if (dig_scale > CS40L2X_DIG_SCALE_MAX)
		return -EINVAL;

	mutex_lock(&cs40l2x->lock);
	/*
	 * this operation calls cs40l2x_wseq_replace which checks the variable
	 * firmware ID and must therefore be performed within mutex protection
	 */
	ret = cs40l2x_dig_scale_set(cs40l2x, dig_scale);
	if (ret)
		goto err_mutex;

	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static int cs40l2x_gpio1_dig_scale_get(struct cs40l2x_private *cs40l2x,
			unsigned int *dig_scale)
{
	int ret;
	unsigned int val;

	ret = regmap_read(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "GAIN_CONTROL",
					CS40L2X_XM_UNPACKED_TYPE), &val);
	if (ret)
		return ret;

	*dig_scale = (val & CS40L2X_GAIN_CTRL_GPIO_MASK)
			>> CS40L2X_GAIN_CTRL_GPIO_SHIFT;

	return 0;
}
#endif
#if defined(CONFIG_CS40L2X_SAMSUNG_FEATURE) || defined(CIRRUS_VIB_DIG_SCALE_SUPPORT)
static int cs40l2x_gpio1_dig_scale_set(struct cs40l2x_private *cs40l2x,
			unsigned int dig_scale)
{
	if (dig_scale == CS40L2X_DIG_SCALE_RESET)
		return -EINVAL;

#if DEBUG_VIB
	pr_info("%s %u\n", __func__, dig_scale);
#endif

	return regmap_update_bits(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "GAIN_CONTROL",
					CS40L2X_XM_UNPACKED_TYPE),
			CS40L2X_GAIN_CTRL_GPIO_MASK,
			dig_scale << CS40L2X_GAIN_CTRL_GPIO_SHIFT);
}
#endif
#ifdef CIRRUS_VIB_DIG_SCALE_SUPPORT
static ssize_t cs40l2x_gpio1_dig_scale_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int dig_scale;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_CAL) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = cs40l2x_gpio1_dig_scale_get(cs40l2x, &dig_scale);
	if (ret)
		goto err_mutex;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", dig_scale);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_gpio1_dig_scale_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int dig_scale;

	ret = kstrtou32(buf, 10, &dig_scale);
	if (ret)
		return -EINVAL;

	if (dig_scale > CS40L2X_DIG_SCALE_MAX)
		return -EINVAL;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_CAL) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = cs40l2x_gpio1_dig_scale_set(cs40l2x, dig_scale);
	if (ret)
		goto err_mutex;

	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}
#endif

static int cs40l2x_cp_dig_scale_get(struct cs40l2x_private *cs40l2x,
			unsigned int *dig_scale)
{
	int ret;
	unsigned int val;

	ret = regmap_read(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "GAIN_CONTROL",
					CS40L2X_XM_UNPACKED_TYPE), &val);
	if (ret)
		return ret;

	*dig_scale = (val & CS40L2X_GAIN_CTRL_TRIG_MASK)
			>> CS40L2X_GAIN_CTRL_TRIG_SHIFT;

	return 0;
}

static int cs40l2x_cp_dig_scale_set(struct cs40l2x_private *cs40l2x,
			unsigned int dig_scale)
{
	if (dig_scale == CS40L2X_DIG_SCALE_RESET)
		return -EINVAL;

#if DEBUG_VIB
	pr_info("%s %u\n", __func__, dig_scale);
#endif

	return regmap_update_bits(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "GAIN_CONTROL",
					CS40L2X_XM_UNPACKED_TYPE),
			CS40L2X_GAIN_CTRL_TRIG_MASK,
			dig_scale << CS40L2X_GAIN_CTRL_TRIG_SHIFT);
}

#ifdef CIRRUS_VIB_DIG_SCALE_SUPPORT
static ssize_t cs40l2x_cp_dig_scale_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int dig_scale;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_CAL) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = cs40l2x_cp_dig_scale_get(cs40l2x, &dig_scale);
	if (ret)
		goto err_mutex;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", dig_scale);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_cp_dig_scale_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int dig_scale;

	ret = kstrtou32(buf, 10, &dig_scale);
	if (ret)
		return -EINVAL;

	if (dig_scale > CS40L2X_DIG_SCALE_MAX)
		return -EINVAL;

#if DEBUG_VIB
	pr_info("%s %u\n", __func__, dig_scale);
#endif

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_CAL) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = cs40l2x_cp_dig_scale_set(cs40l2x, dig_scale);
	if (ret)
		goto err_mutex;

	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}
#endif

static ssize_t cs40l2x_heartbeat_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val;

	mutex_lock(&cs40l2x->lock);

	ret = regmap_read(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "HALO_HEARTBEAT",
					CS40L2X_XM_UNPACKED_TYPE), &val);
	if (ret)
		goto err_mutex;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", val);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_num_waves_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	unsigned int num_waves;

	mutex_lock(&cs40l2x->lock);
	num_waves = cs40l2x->num_waves;
	mutex_unlock(&cs40l2x->lock);

	return snprintf(buf, PAGE_SIZE, "%d\n", num_waves);
}
#ifdef CONFIG_CS40L2X_SAMSUNG_FEATURE
static ssize_t cs40l2x_intensity_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{

	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	return snprintf(buf, 20, "intensity: %u\n", cs40l2x->intensity);
}

static ssize_t cs40l2x_intensity_store(struct device *dev,
	struct device_attribute *devattr, const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret = 0;
	unsigned int intensity, dig_scale;

	ret = kstrtou32(buf, 10, &intensity);
	if (ret) {
		pr_err("fail to get intensity\n");
		return -EINVAL;
	}

	if(intensity > CS40L2X_INTENSITY_SCALE_MAX)
		return -EINVAL;

 	dig_scale = cs40l2x_pbq_dig_scale[intensity/100];
	pr_info("%s: %u (cp dig scale: %u)\n", __func__, intensity, dig_scale);

	mutex_lock(&cs40l2x->lock);
	ret = cs40l2x_cp_dig_scale_set(cs40l2x, dig_scale);
	mutex_unlock(&cs40l2x->lock);

	if (ret) {
		pr_err("Failed to write digital scale\n");
		return ret;
	}

	cs40l2x->intensity = intensity;
	return count;
}

static ssize_t cs40l2x_haptic_engine_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return 0;
}

static ssize_t cs40l2x_haptic_engine_store(struct device *dev,
	struct device_attribute *devattr, const char *buf, size_t count)
{
	int _data = 0;

	if (sscanf(buf, "%6d", &_data) == 1)
		pr_info("%s, packet size : [%d] \n", __func__, _data);
	else
		pr_err("fail to get haptic_engine\n");

	return count;
}

static ssize_t cs40l2x_force_touch_intensity_store(struct device *dev,
		struct device_attribute *devattr, const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret = 0;
	unsigned int intensity, dig_scale;

	ret = kstrtou32(buf, 10, &intensity);
	if (ret) {
		pr_err("fail to get intensity\n");
		return -EINVAL;
	}

	if(intensity > CS40L2X_INTENSITY_SCALE_MAX)
		return -EINVAL;

 	dig_scale = cs40l2x_pbq_dig_scale[intensity/100];
	pr_info("%s: %u (gpio1 dig scale: %u)\n", __func__, intensity, dig_scale);

	mutex_lock(&cs40l2x->lock);
	ret = cs40l2x_gpio1_dig_scale_set(cs40l2x, dig_scale);
	mutex_unlock(&cs40l2x->lock);

	if (ret) {
		pr_err("Failed to write digital scale\n");
		return ret;
	}

	cs40l2x->force_touch_intensity = intensity;
	return count;
}

static ssize_t cs40l2x_force_touch_intensity_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	return snprintf(buf, 30, "force_touch_intensity: %u\n", cs40l2x->force_touch_intensity);
}

static ssize_t cs40l2x_motor_type_show(struct device *dev, 
		struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);

	pr_info("%s: %s\n", __func__, cs40l2x->pdata.vib_type);
	return snprintf(buf, MAX_LEN_VIB_TYPE, "%s\n", cs40l2x->pdata.vib_type);
}
#endif

#ifdef CONFIG_FOLDER_HALL
static int cs40l2x_set_flip_status(struct cs40l2x_private *cs40l2x, int status)
{
	unsigned int dig_scale = 0;
	int ret = 0;

	if (status == VIB_STATUS_UNFOLDING) {
		/* open(0) : set unfolding dig_scale */
		dig_scale = VIB_UNFOLDING_DIG_SCALE;
	} else if (status == VIB_STATUS_FOLDING){
		/* close(1) : set folding dig_scale */
		dig_scale = VIB_FOLDING_DIG_SCALE;
	} else {
		pr_err("%s: wrong status:%d\n", __func__, status);
		return -EINVAL;
	}

	if (dig_scale > CS40L2X_DIG_SCALE_MAX) {
		pr_info("%s: digital scale limit over: %d\n", __func__, dig_scale);
		return -EINVAL;
	}

	mutex_lock(&cs40l2x->lock);
	ret = cs40l2x_dig_scale_set(cs40l2x, dig_scale);
	mutex_unlock(&cs40l2x->lock);

	if (ret) {
		pr_info("%s: Failed to write digital scale\n", __func__);
		return -EIO;
	}
	pr_info("%s: set status:%s scale:%d\n", __func__, 
			status ? "close" : "open", dig_scale);

	return 0;
}

static void cs40l2x_switch_work(struct work_struct *work)
{
	struct cs40l2x_private *cs40l2x = container_of(work, 
					struct cs40l2x_private, switch_work.work);
	int ret = 0;
	
	pr_info("%s: before:%d current:%d\n", __func__, cs40l2x->flip_status, 
				cs40l2x->flip_status_current);

	if (cs40l2x->flip_status_current < VIB_STATUS_UNFOLDING &&
		cs40l2x->flip_status_current > VIB_STATUS_FOLDING) {
		pr_err("%s: get wrong status:%d\n", __func__,
				cs40l2x->flip_status_current);
		return;
	}
	if (cs40l2x->flip_status != cs40l2x->flip_status_current) {
		cs40l2x->flip_status = cs40l2x->flip_status_current;

		ret = cs40l2x_set_flip_status(cs40l2x, cs40l2x->flip_status);
		if (ret) {
			pr_err("%s: failed to work. status:%s\n", __func__,
					cs40l2x->flip_status ? "close" : "open");
			return;
		}
	}
}

static int cs40l2x_hall_ic_notify(struct notifier_block *nb,
			unsigned long status, void *v)
{
	struct cs40l2x_private *cs40l2x = container_of(nb, 
					struct cs40l2x_private, hall_ic_nb);

	cancel_delayed_work(&cs40l2x->switch_work);
	cs40l2x->flip_status_current = status;
	schedule_work(&cs40l2x->switch_work.work);

	pr_info("%s: get status:%d\n", __func__, status);
	
	return 0;
}
#endif

static ssize_t cs40l2x_fw_rev_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	unsigned int fw_rev;

	mutex_lock(&cs40l2x->lock);
	fw_rev = cs40l2x->algo_info[0].rev;
	mutex_unlock(&cs40l2x->lock);

	return snprintf(buf, PAGE_SIZE, "%u\n", fw_rev);
}

static ssize_t cs40l2x_vpp_measured_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);

	if (cs40l2x->vpp_measured < 0)
		return -ENODATA;

	return snprintf(buf, PAGE_SIZE, "%d\n", cs40l2x->vpp_measured);
}

static ssize_t cs40l2x_ipp_measured_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);

	if (cs40l2x->ipp_measured < 0)
		return -ENODATA;

	return snprintf(buf, PAGE_SIZE, "%d\n", cs40l2x->ipp_measured);
}

static ssize_t cs40l2x_vbatt_max_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_read(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "VPMONMAX",
					CS40L2X_XM_UNPACKED_TYPE), &val);
	if (ret)
		goto err_mutex;

	if (val == CS40L2X_VPMONMAX_RESET) {
		ret = -ENODATA;
		goto err_mutex;
	}

	ret = snprintf(buf, PAGE_SIZE, "%u\n", val);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_vbatt_max_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val;

	ret = kstrtou32(buf, 10, &val);
	if (ret)
		return -EINVAL;

	if (val)
		return -EINVAL;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_write(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "VPMONMAX",
					CS40L2X_XM_UNPACKED_TYPE),
			CS40L2X_VPMONMAX_RESET);
	if (ret)
		goto err_mutex;

	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_vbatt_min_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_read(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "VPMONMIN",
					CS40L2X_XM_UNPACKED_TYPE), &val);
	if (ret)
		goto err_mutex;

	if (val == CS40L2X_VPMONMIN_RESET) {
		ret = -ENODATA;
		goto err_mutex;
	}

	ret = snprintf(buf, PAGE_SIZE, "%u\n", val);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_vbatt_min_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val;

	ret = kstrtou32(buf, 10, &val);
	if (ret)
		return -EINVAL;

	if (val)
		return -EINVAL;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_write(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "VPMONMIN",
					CS40L2X_XM_UNPACKED_TYPE),
			CS40L2X_VPMONMIN_RESET);
	if (ret)
		goto err_mutex;

	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static int cs40l2x_refclk_switch(struct cs40l2x_private *cs40l2x,
			unsigned int refclk_freq)
{
	unsigned int refclk_sel, pll_config;
	int ret, i;

	refclk_sel = (refclk_freq == CS40L2X_PLL_REFCLK_FREQ_32K) ?
			CS40L2X_PLL_REFCLK_SEL_MCLK :
			CS40L2X_PLL_REFCLK_SEL_BCLK;

	for (i = 0; i < CS40L2X_NUM_REFCLKS; i++)
		if (cs40l2x_refclks[i].freq == refclk_freq)
			break;
	if (i == CS40L2X_NUM_REFCLKS)
		return -EINVAL;

	pll_config = ((1 << CS40L2X_PLL_REFCLK_EN_SHIFT)
				& CS40L2X_PLL_REFCLK_EN_MASK) |
			((i << CS40L2X_PLL_REFCLK_FREQ_SHIFT)
				& CS40L2X_PLL_REFCLK_FREQ_MASK) |
			((refclk_sel << CS40L2X_PLL_REFCLK_SEL_SHIFT)
				& CS40L2X_PLL_REFCLK_SEL_MASK);

	ret = cs40l2x_hiber_cmd_send(cs40l2x, CS40L2X_POWERCONTROL_FRC_STDBY);
	if (ret)
		return ret;

	ret = regmap_write(cs40l2x->regmap, CS40L2X_PLL_CLK_CTRL, pll_config);
	if (ret)
		return ret;

	ret = cs40l2x_wseq_replace(cs40l2x, CS40L2X_PLL_CLK_CTRL, pll_config);
	if (ret)
		return ret;

	return regmap_write(cs40l2x->regmap, CS40L2X_MBOX_POWERCONTROL,
			CS40L2X_POWERCONTROL_WAKEUP);
}

static int cs40l2x_asp_switch(struct cs40l2x_private *cs40l2x, bool enable)
{
	unsigned int val;
	int ret;

	if (!enable) {
		ret = cs40l2x_user_ctrl_exec(cs40l2x,
				CS40L2X_USER_CTRL_STOP, 0, NULL);
		if (ret)
			return ret;
	}

	ret = regmap_read(cs40l2x->regmap, CS40L2X_SP_ENABLES, &val);
	if (ret)
		return ret;

	val &= ~CS40L2X_ASP_RX1_EN_MASK;
	val |= (enable << CS40L2X_ASP_RX1_EN_SHIFT) & CS40L2X_ASP_RX1_EN_MASK;

	ret = regmap_write(cs40l2x->regmap, CS40L2X_SP_ENABLES, val);
	if (ret)
		return ret;

	ret = cs40l2x_wseq_replace(cs40l2x, CS40L2X_SP_ENABLES, val);
	if (ret)
		return ret;

	if (enable) {
		ret = cs40l2x_user_ctrl_exec(cs40l2x,
				CS40L2X_USER_CTRL_PLAY, 0, NULL);
		if (ret)
			return ret;
	}

	return 0;
}

static ssize_t cs40l2x_asp_enable_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	unsigned int fw_id;

	if (!cs40l2x->asp_available)
		return -EPERM;

	mutex_lock(&cs40l2x->lock);
	fw_id = cs40l2x->fw_desc->id;
	mutex_unlock(&cs40l2x->lock);

	if (fw_id != CS40L2X_FW_ID_REMAP)
		return -EPERM;

	return snprintf(buf, PAGE_SIZE, "%d\n", cs40l2x->asp_enable);
}

static ssize_t cs40l2x_asp_enable_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val, fw_id;

	if (!cs40l2x->asp_available)
		return -EPERM;

	mutex_lock(&cs40l2x->lock);
	fw_id = cs40l2x->fw_desc->id;
	mutex_unlock(&cs40l2x->lock);

	if (fw_id != CS40L2X_FW_ID_REMAP)
		return -EPERM;

	ret = kstrtou32(buf, 10, &val);
	if (ret)
		return -EINVAL;

	if (val > 0)
		cs40l2x->asp_enable = CS40L2X_ASP_ENABLED;
	else
		cs40l2x->asp_enable = CS40L2X_ASP_DISABLED;

	queue_work(cs40l2x->vibe_workqueue, &cs40l2x->vibe_mode_work);

	return count;
}

static ssize_t cs40l2x_asp_timeout_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	unsigned int fw_id;

	if (!cs40l2x->asp_available)
		return -EPERM;

	mutex_lock(&cs40l2x->lock);
	fw_id = cs40l2x->fw_desc->id;
	mutex_unlock(&cs40l2x->lock);

	if (fw_id != CS40L2X_FW_ID_REMAP)
		return -EPERM;

	return snprintf(buf, PAGE_SIZE, "%d\n", cs40l2x->pdata.asp_timeout);
}

static ssize_t cs40l2x_asp_timeout_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val, fw_id;

	if (!cs40l2x->asp_available)
		return -EPERM;

	mutex_lock(&cs40l2x->lock);
	fw_id = cs40l2x->fw_desc->id;
	mutex_unlock(&cs40l2x->lock);

	if (fw_id != CS40L2X_FW_ID_REMAP)
		return -EPERM;

	ret = kstrtou32(buf, 10, &val);
	if (ret)
		return -EINVAL;

	if (val > CS40L2X_ASP_TIMEOUT_MAX)
		return -EINVAL;

	cs40l2x->pdata.asp_timeout = val;

	return count;
}

static ssize_t cs40l2x_exc_enable_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_read(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "EX_PROTECT_ENABLED",
					CS40L2X_XM_UNPACKED_TYPE), &val);
	if (ret)
		goto err_mutex;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", val);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_exc_enable_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val;

	ret = kstrtou32(buf, 10, &val);
	if (ret)
		return -EINVAL;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP
			|| !cs40l2x->exc_available || cs40l2x->a2h_level) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_write(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "EX_PROTECT_ENABLED",
					CS40L2X_XM_UNPACKED_TYPE),
			val ? CS40L2X_EXC_ENABLED : CS40L2X_EXC_DISABLED);
	if (ret)
		goto err_mutex;

	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_a2h_level_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	if (cs40l2x->a2h_level < 0) {
		ret = -EIO;
		goto err_mutex;
	}

	ret = snprintf(buf, PAGE_SIZE, "%d\n", cs40l2x->a2h_level);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_a2h_level_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val, algo_state;

	ret = kstrtou32(buf, 10, &val);
	if (ret)
		return -EINVAL;

	mutex_lock(&cs40l2x->lock);

	if (val >= cs40l2x->num_a2h_levels) {
		ret = -EINVAL;
		goto err_mutex;
	}

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP
			|| !cs40l2x->exc_available) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_read(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "EX_PROTECT_ENABLED",
					CS40L2X_XM_UNPACKED_TYPE), &algo_state);
	if (ret)
		goto err_mutex;

	if (algo_state != CS40L2X_EXC_ENABLED) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = regmap_read(cs40l2x->regmap,
			cs40l2x_dsp_reg(cs40l2x, "PRE_FILTER_ENABLED",
					CS40L2X_YM_UNPACKED_TYPE), &algo_state);
	if (ret)
		goto err_mutex;

	if (algo_state != CS40L2X_PRE_ENABLED) {
		ret = -EPERM;
		goto err_mutex;
	}

	cs40l2x->a2h_level = -1;

	ret = cs40l2x_raw_write(cs40l2x, cs40l2x->pre_dblks[val].reg,
			cs40l2x->pre_dblks[val].data,
			cs40l2x->pre_dblks[val].length, CS40L2X_MAX_WLEN);
	if (ret)
		goto err_mutex;

	ret = cs40l2x_raw_write(cs40l2x, cs40l2x->a2h_dblks[val].reg,
			cs40l2x->a2h_dblks[val].data,
			cs40l2x->a2h_dblks[val].length, CS40L2X_MAX_WLEN);
	if (ret)
		goto err_mutex;

	cs40l2x->a2h_level = val;
	ret = count;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static ssize_t cs40l2x_num_a2h_levels_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP) {
		ret = -EPERM;
		goto err_mutex;
	}

	ret = snprintf(buf, PAGE_SIZE, "%d\n", cs40l2x->num_a2h_levels);

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}
#if defined(CONFIG_SEC_FACTORY)
static ssize_t cs40l2x_hw_reset_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
#ifndef CONFIG_MOTOR_DRV_CS40L2X_PMIC_RESET
	ret = gpiod_get_value_cansleep(cs40l2x->reset_gpio);
#else
	ret = regulator_vldo_get_value(cs40l2x);
#endif
	pr_info("%s: status:%d\n", __func__, ret);
	return snprintf(buf, PAGE_SIZE, "%d\n",
			gpiod_get_value_cansleep(cs40l2x->reset_gpio));
}

static ssize_t cs40l2x_hw_reset_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct cs40l2x_private *cs40l2x = cs40l2x_get_private(dev);
	int ret;
	unsigned int val, fw_id;
#ifdef CONFIG_CS40L2X_SAMSUNG_FEATURE
	pr_info("%s: enter revid:%d %d count:%d\n", __func__, 
			CS40L2X_REVID_B1, cs40l2x->revid, count);
#endif
	if (cs40l2x->revid < CS40L2X_REVID_B1)
		return -EPERM;

	ret = kstrtou32(buf, 10, &val);
	if (ret)
		return -EINVAL;

#ifdef CONFIG_CS40L2X_SAMSUNG_FEATURE
	pr_info("%s: start val:%d\n", __func__, val);
#endif
	mutex_lock(&cs40l2x->lock);

	if (val) {
		// Reset pin needs to work low to high for Motor IC reset
#ifdef CONFIG_CS40L2X_SAMSUNG_FEATURE
#ifndef CONFIG_MOTOR_DRV_CS40L2X_PMIC_RESET
		gpiod_set_value_cansleep(cs40l2x->reset_gpio, 0);
#else
		cs40l2x_reset_control(cs40l2x, 0);
#endif
		usleep_range(2000, 2100);
#endif

#ifndef CONFIG_MOTOR_DRV_CS40L2X_PMIC_RESET
		gpiod_set_value_cansleep(cs40l2x->reset_gpio, 1);
#else
		cs40l2x_reset_control(cs40l2x, 1);
#endif
		usleep_range(1000, 1100);

		fw_id = cs40l2x->fw_desc->id;
		cs40l2x->fw_desc = cs40l2x_firmware_match(cs40l2x,
				CS40L2X_FW_ID_B1ROM);
		if (!cs40l2x->fw_desc) {
			ret = -EINVAL;
			goto err_mutex;
		}

		ret = cs40l2x_firmware_swap(cs40l2x, fw_id);
		if (ret)
			goto err_mutex;
	} else {
#ifndef CONFIG_MOTOR_DRV_CS40L2X_PMIC_RESET
		gpiod_set_value_cansleep(cs40l2x->reset_gpio, 0);
#else
		cs40l2x_reset_control(cs40l2x, 0);
#endif
		usleep_range(2000, 2100);
	}

	ret = count;
#ifdef CONFIG_CS40L2X_SAMSUNG_FEATURE
	pr_info("%s: done val:%d ret:%d\n", __func__, val, ret);
#endif

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}
#endif
static DEVICE_ATTR(cp_trigger_index, 0660, cs40l2x_cp_trigger_index_show,
		cs40l2x_cp_trigger_index_store);
static DEVICE_ATTR(cp_trigger_queue, 0660, cs40l2x_cp_trigger_queue_show,
		cs40l2x_cp_trigger_queue_store);
static DEVICE_ATTR(cp_trigger_duration, 0660, cs40l2x_cp_trigger_duration_show,
		NULL);
static DEVICE_ATTR(hiber_cmd, 0660, NULL, cs40l2x_hiber_cmd_store);
static DEVICE_ATTR(hiber_timeout, 0660, cs40l2x_hiber_timeout_show,
		cs40l2x_hiber_timeout_store);
static DEVICE_ATTR(gpio1_enable, 0660, cs40l2x_gpio1_enable_show,
		cs40l2x_gpio1_enable_store);
static DEVICE_ATTR(gpio1_rise_index, 0660, cs40l2x_gpio1_rise_index_show,
		cs40l2x_gpio1_rise_index_store);
static DEVICE_ATTR(gpio1_fall_index, 0660, cs40l2x_gpio1_fall_index_show,
		cs40l2x_gpio1_fall_index_store);
static DEVICE_ATTR(gpio1_fall_timeout, 0660, cs40l2x_gpio1_fall_timeout_show,
		cs40l2x_gpio1_fall_timeout_store);
static DEVICE_ATTR(gpio2_rise_index, 0660, cs40l2x_gpio2_rise_index_show,
		cs40l2x_gpio2_rise_index_store);
static DEVICE_ATTR(gpio2_fall_index, 0660, cs40l2x_gpio2_fall_index_show,
		cs40l2x_gpio2_fall_index_store);
static DEVICE_ATTR(gpio3_rise_index, 0660, cs40l2x_gpio3_rise_index_show,
		cs40l2x_gpio3_rise_index_store);
static DEVICE_ATTR(gpio3_fall_index, 0660, cs40l2x_gpio3_fall_index_show,
		cs40l2x_gpio3_fall_index_store);
static DEVICE_ATTR(gpio4_rise_index, 0660, cs40l2x_gpio4_rise_index_show,
		cs40l2x_gpio4_rise_index_store);
static DEVICE_ATTR(gpio4_fall_index, 0660, cs40l2x_gpio4_fall_index_show,
		cs40l2x_gpio4_fall_index_store);
static DEVICE_ATTR(standby_timeout, 0660, cs40l2x_standby_timeout_show,
		cs40l2x_standby_timeout_store);
static DEVICE_ATTR(f0_measured, 0660, cs40l2x_f0_measured_show, NULL);
static DEVICE_ATTR(f0_stored, 0660, cs40l2x_f0_stored_show,
		cs40l2x_f0_stored_store);
static DEVICE_ATTR(f0_offset, 0660, cs40l2x_f0_offset_show,
		cs40l2x_f0_offset_store);
static DEVICE_ATTR(redc_measured, 0660, cs40l2x_redc_measured_show, NULL);
static DEVICE_ATTR(redc_stored, 0660, cs40l2x_redc_stored_show,
		cs40l2x_redc_stored_store);
static DEVICE_ATTR(q_measured, 0660, cs40l2x_q_measured_show, NULL);
static DEVICE_ATTR(q_stored, 0660, cs40l2x_q_stored_show,
		cs40l2x_q_stored_store);
static DEVICE_ATTR(comp_enable, 0660, cs40l2x_comp_enable_show,
		cs40l2x_comp_enable_store);
static DEVICE_ATTR(redc_comp_enable, 0660, cs40l2x_redc_comp_enable_show,
		cs40l2x_redc_comp_enable_store);
#ifdef CIRRUS_VIB_DIG_SCALE_SUPPORT
static DEVICE_ATTR(dig_scale, 0660, cs40l2x_dig_scale_show,
		cs40l2x_dig_scale_store);
static DEVICE_ATTR(gpio1_dig_scale, 0660, cs40l2x_gpio1_dig_scale_show,
		cs40l2x_gpio1_dig_scale_store);
static DEVICE_ATTR(cp_dig_scale, 0660, cs40l2x_cp_dig_scale_show,
		cs40l2x_cp_dig_scale_store);
#endif
static DEVICE_ATTR(heartbeat, 0660, cs40l2x_heartbeat_show, NULL);
static DEVICE_ATTR(num_waves, 0660, cs40l2x_num_waves_show, NULL);
#ifdef CONFIG_CS40L2X_SAMSUNG_FEATURE
static DEVICE_ATTR(intensity, 0660, cs40l2x_intensity_show, cs40l2x_intensity_store);
static DEVICE_ATTR(haptic_engine, 0660, cs40l2x_haptic_engine_show, cs40l2x_haptic_engine_store);
static DEVICE_ATTR(force_touch_intensity, 0660, cs40l2x_force_touch_intensity_show,
		cs40l2x_force_touch_intensity_store);
static DEVICE_ATTR(motor_type, 0660, cs40l2x_motor_type_show, NULL);
#endif
static DEVICE_ATTR(fw_rev, 0660, cs40l2x_fw_rev_show, NULL);
static DEVICE_ATTR(vpp_measured, 0660, cs40l2x_vpp_measured_show, NULL);
static DEVICE_ATTR(ipp_measured, 0660, cs40l2x_ipp_measured_show, NULL);
static DEVICE_ATTR(vbatt_max, 0660, cs40l2x_vbatt_max_show,
		cs40l2x_vbatt_max_store);
static DEVICE_ATTR(vbatt_min, 0660, cs40l2x_vbatt_min_show,
		cs40l2x_vbatt_min_store);
static DEVICE_ATTR(asp_enable, 0660, cs40l2x_asp_enable_show,
		cs40l2x_asp_enable_store);
static DEVICE_ATTR(asp_timeout, 0660, cs40l2x_asp_timeout_show,
		cs40l2x_asp_timeout_store);
static DEVICE_ATTR(exc_enable, 0660, cs40l2x_exc_enable_show,
		cs40l2x_exc_enable_store);
static DEVICE_ATTR(a2h_level, 0660, cs40l2x_a2h_level_show,
		cs40l2x_a2h_level_store);
static DEVICE_ATTR(num_a2h_levels, 0660, cs40l2x_num_a2h_levels_show, NULL);
#if defined(CONFIG_SEC_FACTORY)
static DEVICE_ATTR(hw_reset, 0660, cs40l2x_hw_reset_show,
		cs40l2x_hw_reset_store);
#endif

static struct attribute *cs40l2x_dev_attrs[] = {
	&dev_attr_cp_trigger_index.attr,
	&dev_attr_cp_trigger_queue.attr,
	&dev_attr_cp_trigger_duration.attr,
	&dev_attr_hiber_cmd.attr,
	&dev_attr_hiber_timeout.attr,
	&dev_attr_gpio1_enable.attr,
	&dev_attr_gpio1_rise_index.attr,
	&dev_attr_gpio1_fall_index.attr,
	&dev_attr_gpio1_fall_timeout.attr,
	&dev_attr_gpio2_rise_index.attr,
	&dev_attr_gpio2_fall_index.attr,
	&dev_attr_gpio3_rise_index.attr,
	&dev_attr_gpio3_fall_index.attr,
	&dev_attr_gpio4_rise_index.attr,
	&dev_attr_gpio4_fall_index.attr,
	&dev_attr_standby_timeout.attr,
	&dev_attr_f0_measured.attr,
	&dev_attr_f0_stored.attr,
	&dev_attr_f0_offset.attr,
	&dev_attr_redc_measured.attr,
	&dev_attr_redc_stored.attr,
	&dev_attr_q_measured.attr,
	&dev_attr_q_stored.attr,
	&dev_attr_comp_enable.attr,
	&dev_attr_redc_comp_enable.attr,
#ifdef CIRRUS_VIB_DIG_SCALE_SUPPORT
	&dev_attr_dig_scale.attr,
	&dev_attr_gpio1_dig_scale.attr,
	&dev_attr_cp_dig_scale.attr,
#endif
	&dev_attr_heartbeat.attr,
	&dev_attr_num_waves.attr,
#ifdef CONFIG_CS40L2X_SAMSUNG_FEATURE
	&dev_attr_intensity.attr,
	&dev_attr_haptic_engine.attr,
	&dev_attr_force_touch_intensity.attr,
	&dev_attr_motor_type.attr,
#endif
	&dev_attr_fw_rev.attr,
	&dev_attr_vpp_measured.attr,
	&dev_attr_ipp_measured.attr,
	&dev_attr_vbatt_max.attr,
	&dev_attr_vbatt_min.attr,
	&dev_attr_asp_enable.attr,
	&dev_attr_asp_timeout.attr,
	&dev_attr_exc_enable.attr,
	&dev_attr_a2h_level.attr,
	&dev_attr_num_a2h_levels.attr,
#ifdef CONFIG_SEC_FACTORY
	&dev_attr_hw_reset.attr,
#endif
	NULL,
};

static struct attribute_group cs40l2x_dev_attr_group = {
	.attrs = cs40l2x_dev_attrs,
};

static void cs40l2x_vibe_mode_worker(struct work_struct *work)
{
	struct cs40l2x_private *cs40l2x =
		container_of(work, struct cs40l2x_private, vibe_mode_work);
	struct regmap *regmap = cs40l2x->regmap;
	struct device *dev = cs40l2x->dev;
	unsigned int val;
	int ret;

	if (cs40l2x->devid != CS40L2X_DEVID_L25A)
		return;

	mutex_lock(&cs40l2x->lock);

	ret = regmap_read(regmap, cs40l2x_dsp_reg(cs40l2x, "STATUS",
			CS40L2X_XM_UNPACKED_TYPE), &val);
	if (ret) {
		dev_err(dev, "Failed to capture playback status\n");
		goto err_mutex;
	}

	if (val != CS40L2X_STATUS_IDLE)
		goto err_mutex;

	if (cs40l2x->vibe_mode == CS40L2X_VIBE_MODE_HAPTIC
			&& cs40l2x->asp_enable == CS40L2X_ASP_ENABLED) {
		/* switch to audio mode */
		ret = cs40l2x_refclk_switch(cs40l2x,
				cs40l2x->pdata.asp_bclk_freq);
		if (ret) {
			dev_err(dev, "Failed to switch to audio-rate REFCLK\n");
			goto err_mutex;
		}

		ret = cs40l2x_asp_switch(cs40l2x, CS40L2X_ASP_ENABLED);
		if (ret) {
			dev_err(dev, "Failed to enable ASP\n");
			goto err_mutex;
		}

		cs40l2x->vibe_mode = CS40L2X_VIBE_MODE_AUDIO;
	} else if (cs40l2x->vibe_mode == CS40L2X_VIBE_MODE_AUDIO
			&& cs40l2x->asp_enable == CS40L2X_ASP_ENABLED) {
		/* resume audio mode */
		ret = regmap_read(regmap,
				cs40l2x_dsp_reg(cs40l2x, "I2S_ENABLED",
						CS40L2X_XM_UNPACKED_TYPE),
				&val);
		if (ret) {
			dev_err(dev, "Failed to capture pause status\n");
			goto err_mutex;
		}

		if (val == CS40L2X_I2S_ENABLED)
			goto err_mutex;

		ret = cs40l2x_user_ctrl_exec(cs40l2x, CS40L2X_USER_CTRL_PLAY,
				0, NULL);
		if (ret)
			dev_err(dev, "Failed to resume playback\n");
	} else if (cs40l2x->vibe_mode == CS40L2X_VIBE_MODE_AUDIO
			&& cs40l2x->asp_enable == CS40L2X_ASP_DISABLED) {
		/* switch to haptic mode */
		ret = cs40l2x_asp_switch(cs40l2x, CS40L2X_ASP_DISABLED);
		if (ret) {
			dev_err(dev, "Failed to disable ASP\n");
			goto err_mutex;
		}

		ret = cs40l2x_refclk_switch(cs40l2x,
				CS40L2X_PLL_REFCLK_FREQ_32K);
		if (ret) {
			dev_err(dev, "Failed to switch to 32.768-kHz REFCLK\n");
			goto err_mutex;
		}

		cs40l2x->vibe_mode = CS40L2X_VIBE_MODE_HAPTIC;
	}

err_mutex:
	mutex_unlock(&cs40l2x->lock);
}

static enum hrtimer_restart cs40l2x_asp_timer(struct hrtimer *timer)
{
	struct cs40l2x_private *cs40l2x =
		container_of(timer, struct cs40l2x_private, asp_timer);

	queue_work(cs40l2x->vibe_workqueue, &cs40l2x->vibe_mode_work);

	return HRTIMER_NORESTART;
}

static int cs40l2x_stop_playback(struct cs40l2x_private *cs40l2x)
{
	int ret, i;

	for (i = 0; i < CS40L2X_ENDPLAYBACK_RETRIES; i++) {
		ret = regmap_write(cs40l2x->regmap,
				cs40l2x_dsp_reg(cs40l2x, "ENDPLAYBACK",
						CS40L2X_XM_UNPACKED_TYPE),
				CS40L2X_ENDPLAYBACK_REQ);
		if (!ret)
			return 0;

		usleep_range(10000, 10100);
	}

	dev_err(cs40l2x->dev, "Parking device in reset\n");
#ifndef CONFIG_MOTOR_DRV_CS40L2X_PMIC_RESET
	gpiod_set_value_cansleep(cs40l2x->reset_gpio, 0);
#else
	cs40l2x_reset_control(cs40l2x, 0);
#endif

	return -EIO;
}

static int cs40l2x_pbq_cancel(struct cs40l2x_private *cs40l2x)
{
	int ret;

	cs40l2x->pbq_state = CS40L2X_PBQ_STATE_IDLE;
	cs40l2x->cp_trailer_index = CS40L2X_INDEX_IDLE;

	hrtimer_cancel(&cs40l2x->pbq_timer);

	ret = cs40l2x_stop_playback(cs40l2x);
	if (ret)
		return ret;

	ret = cs40l2x_cp_dig_scale_set(cs40l2x, cs40l2x->pbq_cp_dig_scale);
	if (ret)
		return ret;

	pm_relax(cs40l2x->dev);

	return 0;
}

static int cs40l2x_pbq_pair_launch(struct cs40l2x_private *cs40l2x)
{
	unsigned int tag, mag, cp_dig_scale;
	int ret, i;

	do {
		/* restart queue as necessary */
		if (cs40l2x->pbq_index == cs40l2x->pbq_depth) {
			cs40l2x->pbq_index = 0;
			for (i = 0; i < cs40l2x->pbq_depth; i++)
				cs40l2x->pbq_pairs[i].remain =
						cs40l2x->pbq_pairs[i].repeat;

			switch (cs40l2x->pbq_remain) {
			case -1:
				/* loop until stopped */
				break;
			case 0:
				/* queue is finished */
				return cs40l2x_pbq_cancel(cs40l2x);
			default:
				/* loop once more */
				cs40l2x->pbq_remain--;
			}
		}

		tag = cs40l2x->pbq_pairs[cs40l2x->pbq_index].tag;
		mag = cs40l2x->pbq_pairs[cs40l2x->pbq_index].mag;

		switch (tag) {
		case CS40L2X_PBQ_TAG_SILENCE:
			hrtimer_start(&cs40l2x->pbq_timer,
					ktime_set(mag / 1000,
							(mag % 1000) * 1000000),
					HRTIMER_MODE_REL);
			cs40l2x->pbq_state = CS40L2X_PBQ_STATE_SILENT;
			cs40l2x->pbq_index++;
			break;
		case CS40L2X_PBQ_TAG_START:
			cs40l2x->pbq_index++;
			break;
		case CS40L2X_PBQ_TAG_STOP:
			if (cs40l2x->pbq_pairs[cs40l2x->pbq_index].remain) {
				cs40l2x->pbq_pairs[cs40l2x->pbq_index].remain--;
				cs40l2x->pbq_index = mag;
			} else {
				cs40l2x->pbq_index++;
			}
			break;
		default:
			cp_dig_scale = cs40l2x->pbq_cp_dig_scale
					+ cs40l2x_pbq_dig_scale[mag];
			if (cp_dig_scale > CS40L2X_DIG_SCALE_MAX)
				cp_dig_scale = CS40L2X_DIG_SCALE_MAX;

			ret = cs40l2x_cp_dig_scale_set(cs40l2x, cp_dig_scale);
			if (ret)
				return ret;

			ret = regmap_write(cs40l2x->regmap,
					CS40L2X_MBOX_TRIGGERINDEX, tag);
			if (ret)
				return ret;

			cs40l2x->pbq_state = CS40L2X_PBQ_STATE_PLAYING;
			cs40l2x->pbq_index++;

			if (cs40l2x->event_control & CS40L2X_EVENT_END_ENABLED)
				continue;

			hrtimer_start(&cs40l2x->pbq_timer,
					ktime_set(0, CS40L2X_PBQ_POLL_NS),
					HRTIMER_MODE_REL);
		}

	} while (tag == CS40L2X_PBQ_TAG_START || tag == CS40L2X_PBQ_TAG_STOP);

	return 0;
}

static void cs40l2x_vibe_pbq_worker(struct work_struct *work)
{
	struct cs40l2x_private *cs40l2x =
		container_of(work, struct cs40l2x_private, vibe_pbq_work);
	struct regmap *regmap = cs40l2x->regmap;
	struct device *dev = cs40l2x->dev;
	unsigned int val;
	int ret;

	mutex_lock(&cs40l2x->lock);

	switch (cs40l2x->pbq_state) {
	case CS40L2X_PBQ_STATE_IDLE:
		/* queue may have been canceled */
		goto err_mutex;

	case CS40L2X_PBQ_STATE_PLAYING:
		if (cs40l2x->event_control & CS40L2X_EVENT_END_ENABLED)
			break;

		ret = regmap_read(regmap, cs40l2x_dsp_reg(cs40l2x, "STATUS",
				CS40L2X_XM_UNPACKED_TYPE), &val);
		if (ret) {
			dev_err(dev, "Failed to capture playback status\n");
			goto err_mutex;
		}

		if (val != CS40L2X_STATUS_IDLE) {
			hrtimer_start(&cs40l2x->pbq_timer,
					ktime_set(0, CS40L2X_PBQ_POLL_NS),
					HRTIMER_MODE_REL);
			goto err_mutex;
		}
		break;

	case CS40L2X_PBQ_STATE_SILENT:
		break;

	default:
		dev_err(dev, "Unexpected playback queue state: %d\n",
				cs40l2x->pbq_state);
		goto err_mutex;
	}

	ret = cs40l2x_pbq_pair_launch(cs40l2x);
	if (ret)
		dev_err(dev, "Failed to continue playback queue\n");

err_mutex:
	mutex_unlock(&cs40l2x->lock);
}

static enum hrtimer_restart cs40l2x_pbq_timer(struct hrtimer *timer)
{
	struct cs40l2x_private *cs40l2x =
		container_of(timer, struct cs40l2x_private, pbq_timer);

	queue_work(cs40l2x->vibe_workqueue, &cs40l2x->vibe_pbq_work);

	return HRTIMER_NORESTART;
}

static int cs40l2x_diag_enable(struct cs40l2x_private *cs40l2x,
			unsigned int val)
{
	struct regmap *regmap = cs40l2x->regmap;

	switch (cs40l2x->fw_desc->id) {
	case CS40L2X_FW_ID_ORIG:
		return regmap_write(regmap, CS40L2X_MBOX_STIMULUS_MODE, val);
	case CS40L2X_FW_ID_CAL:
		return regmap_write(regmap,
				cs40l2x_dsp_reg(cs40l2x, "F0_TRACKING_ENABLE",
						CS40L2X_XM_UNPACKED_TYPE), val);
	default:
		return -EPERM;
	}
}

static int cs40l2x_diag_capture(struct cs40l2x_private *cs40l2x)
{
	struct regmap *regmap = cs40l2x->regmap;
	unsigned int val;
	int ret;

	switch (cs40l2x->diag_state) {
	case CS40L2X_DIAG_STATE_RUN1:
		ret = regmap_read(regmap,
				cs40l2x_dsp_reg(cs40l2x, "F0",
						CS40L2X_XM_UNPACKED_TYPE),
				&cs40l2x->f0_measured);
		if (ret)
			return ret;

		ret = regmap_read(regmap,
				cs40l2x_dsp_reg(cs40l2x, "REDC",
						CS40L2X_XM_UNPACKED_TYPE),
				&cs40l2x->redc_measured);
		if (ret)
			return ret;

		cs40l2x->diag_state = CS40L2X_DIAG_STATE_DONE1;
		return 0;

	case CS40L2X_DIAG_STATE_RUN2:
		ret = regmap_read(regmap,
				cs40l2x_dsp_reg(cs40l2x, "F0_TRACKING_ENABLE",
						CS40L2X_XM_UNPACKED_TYPE),
				&val);
		if (ret)
			return ret;

		if (val != CS40L2X_F0_TRACKING_OFF)
			return -EBUSY;

		ret = regmap_read(regmap,
				cs40l2x_dsp_reg(cs40l2x, "Q_EST",
						CS40L2X_XM_UNPACKED_TYPE),
				&val);
		if (ret)
			return ret;

		if (val & CS40L2X_QEST_ERROR)
			return -EIO;

		cs40l2x->q_measured = val;
		cs40l2x->diag_state = CS40L2X_DIAG_STATE_DONE2;
		return 0;

	default:
		return -EINVAL;
	}
}

static int cs40l2x_peak_capture(struct cs40l2x_private *cs40l2x)
{
	struct regmap *regmap = cs40l2x->regmap;
	int vmon_max, vmon_min, imon_max, imon_min;
	int ret;

	/* VMON min and max are returned as 24-bit two's-complement values */
	ret = regmap_read(regmap, cs40l2x_dsp_reg(cs40l2x, "VMONMAX",
			CS40L2X_XM_UNPACKED_TYPE), &vmon_max);
	if (ret)
		return ret;
	if (vmon_max > CS40L2X_VMON_POSFS)
		vmon_max = ((vmon_max ^ CS40L2X_VMON_MASK) + 1) * -1;

	ret = regmap_read(regmap, cs40l2x_dsp_reg(cs40l2x, "VMONMIN",
			CS40L2X_XM_UNPACKED_TYPE), &vmon_min);
	if (ret)
		return ret;
	if (vmon_min > CS40L2X_VMON_POSFS)
		vmon_min = ((vmon_min ^ CS40L2X_VMON_MASK) + 1) * -1;

	/* IMON min and max are returned as 24-bit two's-complement values */
	ret = regmap_read(regmap, cs40l2x_dsp_reg(cs40l2x, "IMONMAX",
			CS40L2X_XM_UNPACKED_TYPE), &imon_max);
	if (ret)
		return ret;
	if (imon_max > CS40L2X_IMON_POSFS)
		imon_max = ((imon_max ^ CS40L2X_IMON_MASK) + 1) * -1;

	ret = regmap_read(regmap, cs40l2x_dsp_reg(cs40l2x, "IMONMIN",
			CS40L2X_XM_UNPACKED_TYPE), &imon_min);
	if (ret)
		return ret;
	if (imon_min > CS40L2X_IMON_POSFS)
		imon_min = ((imon_min ^ CS40L2X_IMON_MASK) + 1) * -1;

	cs40l2x->vpp_measured = vmon_max - vmon_min;
	cs40l2x->ipp_measured = imon_max - imon_min;

	return 0;
}

static void cs40l2x_vibe_start_worker(struct work_struct *work)
{
	struct cs40l2x_private *cs40l2x =
		container_of(work, struct cs40l2x_private, vibe_start_work);
	struct regmap *regmap = cs40l2x->regmap;
	struct device *dev = cs40l2x->dev;
	int ret, i;

	mutex_lock(&cs40l2x->lock);

	/* handle interruption of special cases */
	switch (cs40l2x->cp_trailer_index) {
	case CS40L2X_INDEX_QEST:
	case CS40L2X_INDEX_PEAK:
	case CS40L2X_INDEX_DIAG:
		dev_err(dev, "Ignored attempt to interrupt measurement\n");
		goto err_mutex;

	case CS40L2X_INDEX_PBQ:
		ret = cs40l2x_pbq_cancel(cs40l2x);
		if (ret) {
			dev_err(dev, "Failed to cancel playback queue\n");
			goto err_mutex;
		}
		break;
	}

	if (cs40l2x->cp_trigger_index == CS40L2X_INDEX_QEST
			&& cs40l2x->diag_state < CS40L2X_DIAG_STATE_DONE1) {
		dev_err(dev, "Diagnostics index (%d) not yet administered\n",
				CS40L2X_INDEX_DIAG);
		cs40l2x->cp_trailer_index = CS40L2X_INDEX_IDLE;
		goto err_mutex;
	} else {
		cs40l2x->cp_trailer_index = cs40l2x->cp_trigger_index;
	}

	switch (cs40l2x->cp_trailer_index) {
	case CS40L2X_INDEX_PBQ:
#ifdef CONFIG_CS40L2X_SAMSUNG_FEATURE
		pr_info("%s: Playing playback queue\n", __func__);
#endif
		pm_stay_awake(dev);
		break;
	case CS40L2X_INDEX_VIBE:
	case CS40L2X_INDEX_CONT_MIN ... CS40L2X_INDEX_CONT_MAX:
	case CS40L2X_INDEX_QEST:
	case CS40L2X_INDEX_PEAK:
	case CS40L2X_INDEX_DIAG:
		pm_stay_awake(dev);
#ifdef CONFIG_ANDROID_TIMED_OUTPUT
		hrtimer_start(&cs40l2x->vibe_timer,
				ktime_set(cs40l2x->vibe_timeout / 1000,
						(cs40l2x->vibe_timeout % 1000)
						* 1000000),
				HRTIMER_MODE_REL);
#endif /* CONFIG_ANDROID_TIMED_OUTPUT */
#ifdef CONFIG_CS40L2X_SAMSUNG_FEATURE
	default :
		pr_info("%s: Playing index %u (intensity: %u)\n",
			__func__, cs40l2x->cp_trailer_index, cs40l2x->intensity);
#endif
	}

	switch (cs40l2x->cp_trailer_index) {
	case CS40L2X_INDEX_PEAK:
		cs40l2x->vpp_measured = -1;
		cs40l2x->ipp_measured = -1;

		ret = regmap_read(regmap,
				cs40l2x_dsp_reg(cs40l2x, "GPIO_ENABLE",
						CS40L2X_XM_UNPACKED_TYPE),
				&cs40l2x->peak_gpio1_enable);
		if (ret) {
			dev_err(dev, "Failed to read GPIO1 configuration\n");
			goto err_mutex;
		}

		ret = regmap_write(regmap,
				cs40l2x_dsp_reg(cs40l2x, "GPIO_ENABLE",
						CS40L2X_XM_UNPACKED_TYPE),
				CS40L2X_GPIO1_DISABLED);
		if (ret) {
			dev_err(dev, "Failed to disable GPIO1\n");
			goto err_mutex;
		}

		ret = regmap_write(regmap, CS40L2X_MBOX_TRIGGER_MS,
				CS40L2X_INDEX_VIBE);
		if (ret) {
			dev_err(dev, "Failed to start playback\n");
			goto err_mutex;
		}

		msleep(CS40L2X_PEAK_DELAY_MS);

		ret = regmap_write(regmap, cs40l2x_dsp_reg(cs40l2x, "VMONMAX",
				CS40L2X_XM_UNPACKED_TYPE), CS40L2X_VMON_NEGFS);
		if (ret) {
			dev_err(dev, "Failed to reset maximum VMON\n");
			goto err_mutex;
		}

		ret = regmap_write(regmap, cs40l2x_dsp_reg(cs40l2x, "VMONMIN",
				CS40L2X_XM_UNPACKED_TYPE), CS40L2X_VMON_POSFS);
		if (ret) {
			dev_err(dev, "Failed to reset minimum VMON\n");
			goto err_mutex;
		}

		ret = regmap_write(regmap, cs40l2x_dsp_reg(cs40l2x, "IMONMAX",
				CS40L2X_XM_UNPACKED_TYPE), CS40L2X_IMON_NEGFS);
		if (ret) {
			dev_err(dev, "Failed to reset maximum IMON\n");
			goto err_mutex;
		}

		ret = regmap_write(regmap, cs40l2x_dsp_reg(cs40l2x, "IMONMIN",
				CS40L2X_XM_UNPACKED_TYPE), CS40L2X_IMON_POSFS);
		if (ret)
			dev_err(dev, "Failed to reset minimum IMON\n");
		break;

	case CS40L2X_INDEX_VIBE:
	case CS40L2X_INDEX_CONT_MIN ... CS40L2X_INDEX_CONT_MAX:
		ret = regmap_write(regmap, CS40L2X_MBOX_TRIGGER_MS,
				cs40l2x->cp_trailer_index & CS40L2X_INDEX_MASK);
		if (ret)
			dev_err(dev, "Failed to start playback\n");
		break;

	case CS40L2X_INDEX_CLICK_MIN ... CS40L2X_INDEX_CLICK_MAX:
		ret = regmap_write(regmap, CS40L2X_MBOX_TRIGGERINDEX,
				cs40l2x->cp_trailer_index);
		if (ret)
			dev_err(dev, "Failed to start playback\n");
		break;

	case CS40L2X_INDEX_PBQ:
		cs40l2x->pbq_cp_dig_scale = CS40L2X_DIG_SCALE_RESET;

		ret = cs40l2x_cp_dig_scale_get(cs40l2x,
				&cs40l2x->pbq_cp_dig_scale);
		if (ret) {
			dev_err(dev, "Failed to read digital scale\n");
			goto err_mutex;
		}

		cs40l2x->pbq_index = 0;
		cs40l2x->pbq_remain = cs40l2x->pbq_repeat;

		for (i = 0; i < cs40l2x->pbq_depth; i++)
			cs40l2x->pbq_pairs[i].remain =
					cs40l2x->pbq_pairs[i].repeat;

		ret = cs40l2x_pbq_pair_launch(cs40l2x);
		if (ret)
			dev_err(dev, "Failed to launch playback queue\n");
		break;

	case CS40L2X_INDEX_DIAG:
		cs40l2x->diag_state = CS40L2X_DIAG_STATE_INIT;
		cs40l2x->diag_dig_scale = CS40L2X_DIG_SCALE_RESET;

		ret = cs40l2x_dig_scale_get(cs40l2x, &cs40l2x->diag_dig_scale);
		if (ret) {
			dev_err(dev, "Failed to read digital scale\n");
			goto err_mutex;
		}

		ret = cs40l2x_dig_scale_set(cs40l2x, 0);
		if (ret) {
			dev_err(dev, "Failed to reset digital scale\n");
			goto err_mutex;
		}

		ret = regmap_write(regmap,
				cs40l2x_dsp_reg(cs40l2x, "CLOSED_LOOP",
					CS40L2X_XM_UNPACKED_TYPE), 0);
		if (ret) {
			dev_err(dev, "Failed to disable closed-loop mode\n");
			goto err_mutex;
		}

		ret = cs40l2x_diag_enable(cs40l2x, CS40L2X_F0_TRACKING_DIAG);
		if (ret) {
			dev_err(dev, "Failed to enable diagnostics tone\n");
			goto err_mutex;
		}

		msleep(CS40L2X_DIAG_STATE_DELAY_MS);

		ret = regmap_write(regmap,
				cs40l2x_dsp_reg(cs40l2x, "CLOSED_LOOP",
					CS40L2X_XM_UNPACKED_TYPE), 1);
		if (ret) {
			dev_err(dev, "Failed to enable closed-loop mode\n");
			goto err_mutex;
		}

		cs40l2x->diag_state = CS40L2X_DIAG_STATE_RUN1;
		break;

	case CS40L2X_INDEX_QEST:
		cs40l2x->diag_dig_scale = CS40L2X_DIG_SCALE_RESET;

		ret = cs40l2x_dig_scale_get(cs40l2x, &cs40l2x->diag_dig_scale);
		if (ret) {
			dev_err(dev, "Failed to read digital scale\n");
			goto err_mutex;
		}

		ret = cs40l2x_dig_scale_set(cs40l2x, 0);
		if (ret) {
			dev_err(dev, "Failed to reset digital scale\n");
			goto err_mutex;
		}

		ret = cs40l2x_diag_enable(cs40l2x, CS40L2X_F0_TRACKING_QEST);
		if (ret) {
			dev_err(dev, "Failed to enable diagnostics tone\n");
			goto err_mutex;
		}

		cs40l2x->diag_state = CS40L2X_DIAG_STATE_RUN2;
		break;

	default:
		dev_err(dev, "Invalid wavetable index\n");
	}

err_mutex:
	mutex_unlock(&cs40l2x->lock);
}

static void cs40l2x_vibe_stop_worker(struct work_struct *work)
{
	struct cs40l2x_private *cs40l2x =
		container_of(work, struct cs40l2x_private, vibe_stop_work);
	struct regmap *regmap = cs40l2x->regmap;
	struct device *dev = cs40l2x->dev;
	int ret;

	mutex_lock(&cs40l2x->lock);

	/* handle effects that straddle a firmware swap */
	if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_CAL) {
		switch (cs40l2x->cp_trailer_index) {
		case CS40L2X_INDEX_VIBE:
		case CS40L2X_INDEX_CONT_MIN ... CS40L2X_INDEX_CONT_MAX:
		case CS40L2X_INDEX_PEAK:
			pm_relax(dev);
			/* intentionally fall through */

		case CS40L2X_INDEX_PBQ:
		case CS40L2X_INDEX_CLICK_MIN ... CS40L2X_INDEX_CLICK_MAX:
			goto err_skip;
		}
	} else if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_REMAP) {
		switch (cs40l2x->cp_trailer_index) {
		case CS40L2X_INDEX_QEST:
		case CS40L2X_INDEX_DIAG:
			pm_relax(dev);
			goto err_skip;
		}
	}

	switch (cs40l2x->cp_trailer_index) {
	case CS40L2X_INDEX_PEAK:
		ret = cs40l2x_peak_capture(cs40l2x);
		if (ret)
			dev_err(dev, "Failed to capture peak-to-peak values\n");

		ret = regmap_write(regmap,
				cs40l2x_dsp_reg(cs40l2x, "GPIO_ENABLE",
						CS40L2X_XM_UNPACKED_TYPE),
				cs40l2x->peak_gpio1_enable);
		if (ret)
			dev_err(dev, "Failed to restore GPIO1 configuration\n");
		/* intentionally fall through */

	case CS40L2X_INDEX_VIBE:
	case CS40L2X_INDEX_CONT_MIN ... CS40L2X_INDEX_CONT_MAX:
		ret = cs40l2x_stop_playback(cs40l2x);
		if (ret)
			dev_err(dev, "Failed to stop playback\n");
		pm_relax(dev);
		break;

	case CS40L2X_INDEX_CLICK_MIN ... CS40L2X_INDEX_CLICK_MAX:
		ret = cs40l2x_stop_playback(cs40l2x);
		if (ret)
			dev_err(dev, "Failed to stop playback\n");
		break;

	case CS40L2X_INDEX_PBQ:
		ret = cs40l2x_pbq_cancel(cs40l2x);
		if (ret)
			dev_err(dev, "Failed to cancel playback queue\n");
		break;

	case CS40L2X_INDEX_DIAG:
	case CS40L2X_INDEX_QEST:
		ret = cs40l2x_diag_capture(cs40l2x);
		if (ret)
			dev_err(dev, "Failed to capture measurement(s): %d\n",
					ret);

		ret = cs40l2x_diag_enable(cs40l2x, CS40L2X_F0_TRACKING_OFF);
		if (ret)
			dev_err(dev, "Failed to disable diagnostics tone\n");

		ret = cs40l2x_dig_scale_set(cs40l2x, cs40l2x->diag_dig_scale);
		if (ret)
			dev_err(dev, "Failed to restore digital scale\n");
		pm_relax(dev);
		break;

	case CS40L2X_INDEX_IDLE:
		break;

	default:
		dev_err(dev, "Invalid wavetable index\n");
	}

err_skip:
	cs40l2x->cp_trailer_index = CS40L2X_INDEX_IDLE;

	mutex_unlock(&cs40l2x->lock);
}

#ifdef CONFIG_ANDROID_TIMED_OUTPUT
/* vibration callback for timed output device */
static void cs40l2x_vibe_enable(struct timed_output_dev *sdev, int timeout)
{
	struct cs40l2x_private *cs40l2x =
		container_of(sdev, struct cs40l2x_private, timed_dev);

#ifdef CONFIG_CS40L2X_SAMSUNG_FEATURE
	pr_info("%s: %dms\n", __func__, timeout);
#endif
#ifdef CONFIG_FOLDER_HALL
	if (cs40l2x->flip_status == -1) {
		hall_ic_request_notify(&cs40l2x->hall_ic_nb);
	}
#endif
	if (cs40l2x->vibe_init_success) {
		if (timeout > 0) {
			cs40l2x->vibe_timeout = timeout;
			queue_work(cs40l2x->vibe_workqueue, &cs40l2x->vibe_start_work);
		} else {
			hrtimer_cancel(&cs40l2x->vibe_timer);
			queue_work(cs40l2x->vibe_workqueue, &cs40l2x->vibe_stop_work);
		}
	} else {
		pr_info("%s vibe init state? %d\n", __func__, cs40l2x->vibe_init_success);
	}

}

static int cs40l2x_vibe_get_time(struct timed_output_dev *sdev)
{
	struct cs40l2x_private *cs40l2x =
		container_of(sdev, struct cs40l2x_private, timed_dev);
	int ret = 0;

	if (hrtimer_active(&cs40l2x->vibe_timer))
		ret = ktime_to_ms(hrtimer_get_remaining(&cs40l2x->vibe_timer));

	return ret;
}

static enum hrtimer_restart cs40l2x_vibe_timer(struct hrtimer *timer)
{
	struct cs40l2x_private *cs40l2x =
		container_of(timer, struct cs40l2x_private, vibe_timer);

	queue_work(cs40l2x->vibe_workqueue, &cs40l2x->vibe_stop_work);

	return HRTIMER_NORESTART;
}

static void cs40l2x_create_timed_output(struct cs40l2x_private *cs40l2x)
{
	int ret;
	struct timed_output_dev *timed_dev = &cs40l2x->timed_dev;
	struct hrtimer *vibe_timer = &cs40l2x->vibe_timer;
	struct device *dev = cs40l2x->dev;

	timed_dev->name = CS40L2X_DEVICE_NAME;
	timed_dev->enable = cs40l2x_vibe_enable;
	timed_dev->get_time = cs40l2x_vibe_get_time;

	ret = timed_output_dev_register(timed_dev);
	if (ret) {
		dev_err(dev, "Failed to register timed output device: %d\n",
			ret);
		return;
	}

	hrtimer_init(vibe_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	vibe_timer->function = cs40l2x_vibe_timer;

	ret = sysfs_create_group(&cs40l2x->timed_dev.dev->kobj,
			&cs40l2x_dev_attr_group);
	if (ret) {
		dev_err(dev, "Failed to create sysfs group: %d\n", ret);
		return;
	}
}
#else
/* vibration callback for LED device */
static void cs40l2x_vibe_brightness_set(struct led_classdev *led_cdev,
		enum led_brightness brightness)
{
	struct cs40l2x_private *cs40l2x =
		container_of(led_cdev, struct cs40l2x_private, led_dev);

	switch (brightness) {
	case LED_OFF:
		queue_work(cs40l2x->vibe_workqueue, &cs40l2x->vibe_stop_work);
		break;
	default:
		queue_work(cs40l2x->vibe_workqueue, &cs40l2x->vibe_start_work);
	}
}

static void cs40l2x_create_led(struct cs40l2x_private *cs40l2x)
{
	int ret;
	struct led_classdev *led_dev = &cs40l2x->led_dev;
	struct device *dev = cs40l2x->dev;

	led_dev->name = CS40L2X_DEVICE_NAME;
	led_dev->max_brightness = LED_FULL;
	led_dev->brightness_set = cs40l2x_vibe_brightness_set;
	led_dev->default_trigger = "transient";

	ret = led_classdev_register(dev, led_dev);
	if (ret) {
		dev_err(dev, "Failed to register LED device: %d\n", ret);
		return;
	}

	ret = sysfs_create_group(&cs40l2x->dev->kobj, &cs40l2x_dev_attr_group);
	if (ret) {
		dev_err(dev, "Failed to create sysfs group: %d\n", ret);
		return;
	}
}
#endif /* CONFIG_ANDROID_TIMED_OUTPUT */

static void cs40l2x_vibe_init(struct cs40l2x_private *cs40l2x)
{
	struct hrtimer *pbq_timer = &cs40l2x->pbq_timer;
	struct hrtimer *asp_timer = &cs40l2x->asp_timer;
	int ret;
#ifndef CONFIG_CS40L2X_SAMSUNG_FEATURE
#ifdef CONFIG_ANDROID_TIMED_OUTPUT
	cs40l2x_create_timed_output(cs40l2x);
#else
	cs40l2x_create_led(cs40l2x);
#endif /* CONFIG_ANDROID_TIMED_OUTPUT */
#endif
	cs40l2x->vibe_workqueue =
		alloc_ordered_workqueue("vibe_workqueue", WQ_HIGHPRI);
	if (!cs40l2x->vibe_workqueue) {
		dev_err(cs40l2x->dev, "Failed to allocate workqueue\n");
		return;
	}

	INIT_WORK(&cs40l2x->vibe_start_work, cs40l2x_vibe_start_worker);
	INIT_WORK(&cs40l2x->vibe_pbq_work, cs40l2x_vibe_pbq_worker);
	INIT_WORK(&cs40l2x->vibe_stop_work, cs40l2x_vibe_stop_worker);
	INIT_WORK(&cs40l2x->vibe_mode_work, cs40l2x_vibe_mode_worker);

	hrtimer_init(pbq_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	pbq_timer->function = cs40l2x_pbq_timer;

	hrtimer_init(asp_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	asp_timer->function = cs40l2x_asp_timer;

	ret = device_init_wakeup(cs40l2x->dev, true);
	if (ret) {
		dev_err(cs40l2x->dev, "Failed to initialize wakeup source\n");
		return;
	}

	cs40l2x->vibe_init_success = true;

	dev_info(cs40l2x->dev, "Firmware revision %d.%d.%d\n",
			(cs40l2x->algo_info[0].rev & 0xFF0000) >> 16,
			(cs40l2x->algo_info[0].rev & 0xFF00) >> 8,
			cs40l2x->algo_info[0].rev & 0xFF);

	dev_info(cs40l2x->dev,
			"Max. wavetable size: %d bytes (XM), %d bytes (YM)\n",
			cs40l2x->wt_limit_xm / 4 * 3,
			cs40l2x->wt_limit_ym / 4 * 3);
}
#ifdef CONFIG_CS40L2X_SAMSUNG_FEATURE
static void cs40l2x_dev_node_init(struct cs40l2x_private *cs40l2x)
{
#ifdef CONFIG_ANDROID_TIMED_OUTPUT
	cs40l2x_create_timed_output(cs40l2x);
#else
	cs40l2x_create_led(cs40l2x);
#endif
}

static void cs40l2x_dev_node_remove(struct cs40l2x_private *cs40l2x)
{
#ifdef CONFIG_ANDROID_TIMED_OUTPUT
		sysfs_remove_group(&cs40l2x->timed_dev.dev->kobj,
				&cs40l2x_dev_attr_group);

		timed_output_dev_unregister(&cs40l2x->timed_dev);
#else
		sysfs_remove_group(&cs40l2x->dev->kobj,
				&cs40l2x_dev_attr_group);

		led_classdev_unregister(&cs40l2x->led_dev);
#endif
#ifdef CONFIG_MOTOR_DRV_CS40L2X_PMIC_RESET
	if (cs40l2x->reset_vldo != NULL) {
		regulator_put(cs40l2x->reset_vldo);
	}
	cs40l2x->reset_vldo == NULL;
#endif
}
#endif
static int cs40l2x_coeff_init(struct cs40l2x_private *cs40l2x)
{
	struct regmap *regmap = cs40l2x->regmap;
	struct device *dev = cs40l2x->dev;
	struct cs40l2x_coeff_desc *coeff_desc;
	unsigned int reg = CS40L2X_XM_FW_ID;
	unsigned int val;
	int ret, i;

	ret = regmap_read(regmap, CS40L2X_XM_NUM_ALGOS, &val);
	if (ret) {
		dev_err(dev, "Failed to read number of algorithms\n");
		return ret;
	}

	if (val > CS40L2X_NUM_ALGOS_MAX) {
		dev_err(dev, "Invalid number of algorithms\n");
		return -EINVAL;
	}
	cs40l2x->num_algos = val + 1;

	for (i = 0; i < cs40l2x->num_algos; i++) {
		ret = regmap_read(regmap,
				reg + CS40L2X_ALGO_ID_OFFSET,
				&cs40l2x->algo_info[i].id);
		if (ret) {
			dev_err(dev, "Failed to read algo. %d ID\n", i);
			return ret;
		}

		ret = regmap_read(regmap,
				reg + CS40L2X_ALGO_REV_OFFSET,
				&cs40l2x->algo_info[i].rev);
		if (ret) {
			dev_err(dev, "Failed to read algo. %d revision\n", i);
			return ret;
		}

		ret = regmap_read(regmap,
				reg + CS40L2X_ALGO_XM_BASE_OFFSET,
				&cs40l2x->algo_info[i].xm_base);
		if (ret) {
			dev_err(dev, "Failed to read algo. %d XM_BASE\n", i);
			return ret;
		}

		ret = regmap_read(regmap,
				reg + CS40L2X_ALGO_XM_SIZE_OFFSET,
				&cs40l2x->algo_info[i].xm_size);
		if (ret) {
			dev_err(dev, "Failed to read algo. %d XM_SIZE\n", i);
			return ret;
		}

		ret = regmap_read(regmap,
				reg + CS40L2X_ALGO_YM_BASE_OFFSET,
				&cs40l2x->algo_info[i].ym_base);
		if (ret) {
			dev_err(dev, "Failed to read algo. %d YM_BASE\n", i);
			return ret;
		}

		ret = regmap_read(regmap,
				reg + CS40L2X_ALGO_YM_SIZE_OFFSET,
				&cs40l2x->algo_info[i].ym_size);
		if (ret) {
			dev_err(dev, "Failed to read algo. %d YM_SIZE\n", i);
			return ret;
		}

		list_for_each_entry(coeff_desc,
			&cs40l2x->coeff_desc_head, list) {

			if (coeff_desc->parent_id != cs40l2x->algo_info[i].id)
				continue;

			switch (coeff_desc->block_type) {
			case CS40L2X_XM_UNPACKED_TYPE:
				coeff_desc->reg = CS40L2X_DSP1_XMEM_UNPACK24_0
					+ cs40l2x->algo_info[i].xm_base * 4
					+ coeff_desc->block_offset * 4;
				if (!strcmp(coeff_desc->name, "WAVETABLE"))
					cs40l2x->wt_limit_xm =
						(cs40l2x->algo_info[i].xm_size
						- coeff_desc->block_offset) * 4;
				break;
			case CS40L2X_YM_UNPACKED_TYPE:
				coeff_desc->reg = CS40L2X_DSP1_YMEM_UNPACK24_0
					+ cs40l2x->algo_info[i].ym_base * 4
					+ coeff_desc->block_offset * 4;
				if (!strcmp(coeff_desc->name, "WAVETABLEYM"))
					cs40l2x->wt_limit_ym =
						(cs40l2x->algo_info[i].ym_size
						- coeff_desc->block_offset) * 4;
				break;
			}

			dev_dbg(dev, "Found control %s at 0x%08X\n",
				coeff_desc->name, coeff_desc->reg);
		}

		/* system algo. contains one extra register (num. algos.) */
		if (i)
			reg += CS40L2X_ALGO_ENTRY_SIZE;
		else
			reg += (CS40L2X_ALGO_ENTRY_SIZE + 4);
	}

	ret = regmap_read(regmap, reg, &val);
	if (ret) {
		dev_err(dev, "Failed to read list terminator\n");
		return ret;
	}

	if (val != CS40L2X_ALGO_LIST_TERM) {
		dev_err(dev, "Invalid list terminator: 0x%X\n", val);
		return -EINVAL;
	}

	if (cs40l2x->algo_info[0].id != cs40l2x->fw_desc->id) {
		dev_err(dev, "Invalid firmware ID: 0x%06X\n",
				cs40l2x->algo_info[0].id);
		return -EINVAL;
	}

	if (cs40l2x->algo_info[0].rev < cs40l2x->fw_desc->min_rev) {
		dev_err(dev, "Invalid firmware revision: %d.%d.%d\n",
				(cs40l2x->algo_info[0].rev & 0xFF0000) >> 16,
				(cs40l2x->algo_info[0].rev & 0xFF00) >> 8,
				cs40l2x->algo_info[0].rev & 0xFF);
		return -EINVAL;
	}

	return 0;
}

static void cs40l2x_coeff_free(struct cs40l2x_private *cs40l2x)
{
	struct cs40l2x_coeff_desc *coeff_desc;

	while (!list_empty(&cs40l2x->coeff_desc_head)) {
		coeff_desc = list_first_entry(&cs40l2x->coeff_desc_head,
				struct cs40l2x_coeff_desc, list);
		list_del(&coeff_desc->list);
		kfree(coeff_desc);
	}
}

static int cs40l2x_hw_err_rls(struct cs40l2x_private *cs40l2x,
			unsigned int irq_mask)
{
	struct regmap *regmap = cs40l2x->regmap;
	struct device *dev = cs40l2x->dev;
	int ret, i;

	for (i = 0; i < CS40L2X_NUM_HW_ERRS; i++)
		if (cs40l2x_hw_errs[i].irq_mask == irq_mask)
			break;

	if (i == CS40L2X_NUM_HW_ERRS) {
		dev_err(dev, "Unrecognized hardware error\n");
		return -EINVAL;
	}

	dev_crit(dev, "Encountered %s error\n", cs40l2x_hw_errs[i].err_name);

	if (cs40l2x_hw_errs[i].bst_cycle) {
		ret = regmap_update_bits(regmap, CS40L2X_PWR_CTRL2,
				CS40L2X_BST_EN_MASK,
				CS40L2X_BST_DISABLED << CS40L2X_BST_EN_SHIFT);
		if (ret) {
			dev_err(dev, "Failed to disable boost converter\n");
			return ret;
		}
	}

	ret = regmap_write(regmap, CS40L2X_IRQ2_STATUS1, irq_mask);
	if (ret) {
		dev_err(dev, "Failed to acknowledge hardware error\n");
		return ret;
	}

	ret = regmap_write(regmap, CS40L2X_PROTECT_REL_ERR_IGN, 0);
	if (ret) {
		dev_err(dev, "Failed to cycle error release (step 1 of 3)\n");
		return ret;
	}

	ret = regmap_write(regmap, CS40L2X_PROTECT_REL_ERR_IGN,
			cs40l2x_hw_errs[i].rls_mask);
	if (ret) {
		dev_err(dev, "Failed to cycle error release (step 2 of 3)\n");
		return ret;
	}

	ret = regmap_write(regmap, CS40L2X_PROTECT_REL_ERR_IGN, 0);
	if (ret) {
		dev_err(dev, "Failed to cycle error release (step 3 of 3)\n");
		return ret;
	}

	if (cs40l2x_hw_errs[i].bst_cycle) {
		ret = regmap_update_bits(regmap, CS40L2X_PWR_CTRL2,
				CS40L2X_BST_EN_MASK,
				CS40L2X_BST_ENABLED << CS40L2X_BST_EN_SHIFT);
		if (ret) {
			dev_err(dev, "Failed to re-enable boost converter\n");
			return ret;
		}
	}

	return 0;
}

static int cs40l2x_hw_err_chk(struct cs40l2x_private *cs40l2x)
{
	int ret, i;
	unsigned int val;

	ret = regmap_read(cs40l2x->regmap, CS40L2X_IRQ2_STATUS1, &val);
	if (ret) {
		dev_err(cs40l2x->dev, "Failed to read hardware error status\n");
		return ret;
	}

	for (i = 0; i < CS40L2X_NUM_HW_ERRS; i++)
		if (val & cs40l2x_hw_errs[i].irq_mask) {
			ret = cs40l2x_hw_err_rls(cs40l2x,
					cs40l2x_hw_errs[i].irq_mask);
			if (ret)
				return ret;
		}

	return 0;
}

static const struct reg_sequence cs40l2x_irq2_masks[] = {
	{CS40L2X_IRQ2_MASK1,		0xFFFFFFFF},
	{CS40L2X_IRQ2_MASK2,		0xFFFFFFFF},
	{CS40L2X_IRQ2_MASK3,		0xFFFF87FF},
	{CS40L2X_IRQ2_MASK4,		0xFEFFFFFF},
};

static int cs40l2x_dsp_pre_config(struct cs40l2x_private *cs40l2x)
{
	struct regmap *regmap = cs40l2x->regmap;
	struct device *dev = cs40l2x->dev;
	unsigned int gpio_btndetect = CS40L2X_GPIO_BTNDETECT_GPIO1;
	unsigned int val;
	int ret, i;

	if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_CAL)
		return 0;

	if (cs40l2x->devid == CS40L2X_DEVID_L25B)
		gpio_btndetect |= (CS40L2X_GPIO_BTNDETECT_GPIO2
				| CS40L2X_GPIO_BTNDETECT_GPIO3
				| CS40L2X_GPIO_BTNDETECT_GPIO4);

	ret = regmap_write(regmap, cs40l2x_dsp_reg(cs40l2x, "GPIO_BUTTONDETECT",
			CS40L2X_XM_UNPACKED_TYPE), gpio_btndetect);
	if (ret) {
		dev_err(dev, "Failed to enable GPIO detection\n");
		return ret;
	}

	if (cs40l2x->pdata.gpio1_mode != CS40L2X_GPIO1_MODE_DEF_ON) {
		ret = regmap_write(regmap,
				cs40l2x_dsp_reg(cs40l2x, "GPIO_ENABLE",
						CS40L2X_XM_UNPACKED_TYPE),
				CS40L2X_GPIO1_DISABLED);
		if (ret) {
			dev_err(dev, "Failed to pre-configure GPIO1\n");
			return ret;
		}
	}

	if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_REMAP) {
		ret = cs40l2x_wseq_init(cs40l2x);
		if (ret) {
			dev_err(dev, "Failed to initialize write sequencer\n");
			return ret;
		}

		ret = regmap_write(regmap,
				cs40l2x_dsp_reg(cs40l2x, "EVENTCONTROL",
						CS40L2X_XM_UNPACKED_TYPE),
				cs40l2x->event_control);
		if (ret) {
			dev_err(dev, "Failed to configure event controls\n");
			return ret;
		}

		for (i = 0; i < ARRAY_SIZE(cs40l2x_irq2_masks); i++) {
			/* unmask hardware error interrupts */
			val = cs40l2x_irq2_masks[i].def;
			if (cs40l2x_irq2_masks[i].reg == CS40L2X_IRQ2_MASK1)
				val &= ~cs40l2x->hw_err_mask;

			/* upper half */
			ret = regmap_write(regmap,
					cs40l2x_dsp_reg(cs40l2x,
						"IRQMASKSEQUENCE",
						CS40L2X_XM_UNPACKED_TYPE)
						+ i * CS40L2X_IRQMASKSEQ_STRIDE,
					(val & CS40L2X_IRQMASKSEQ_MASK1)
						<< CS40L2X_IRQMASKSEQ_SHIFTUP);
			if (ret) {
				dev_err(dev,
					"Failed to write IRQMASKSEQ (upper)\n");
				return ret;
			}

			/* lower half */
			ret = regmap_write(regmap,
					cs40l2x_dsp_reg(cs40l2x,
						"IRQMASKSEQUENCE",
						CS40L2X_XM_UNPACKED_TYPE) + 4
						+ i * CS40L2X_IRQMASKSEQ_STRIDE,
					(val & CS40L2X_IRQMASKSEQ_MASK2)
						>> CS40L2X_IRQMASKSEQ_SHIFTDN);
			if (ret) {
				dev_err(dev,
					"Failed to write IRQMASKSEQ (lower)\n");
				return ret;
			}
		}

		ret = regmap_write(regmap,
				cs40l2x_dsp_reg(cs40l2x,
						"IRQMASKSEQUENCE_VALID",
						CS40L2X_XM_UNPACKED_TYPE), 1);
		if (ret) {
			dev_err(dev, "Failed to enable IRQMASKSEQ\n");
			return ret;
		}
	}

	return 0;
}

static const struct reg_sequence cs40l2x_dsp_errata[] = {
	{CS40L2X_DSP1_XM_ACCEL_PL0_PRI,	0x00000000},
	{CS40L2X_DSP1_YM_ACCEL_PL0_PRI,	0x00000000},
	{CS40L2X_DSP1_RX1_RATE,		0x00000001},
	{CS40L2X_DSP1_RX2_RATE,		0x00000001},
	{CS40L2X_DSP1_RX3_RATE,		0x00000001},
	{CS40L2X_DSP1_RX4_RATE,		0x00000001},
	{CS40L2X_DSP1_RX5_RATE,		0x00000001},
	{CS40L2X_DSP1_RX6_RATE,		0x00000001},
	{CS40L2X_DSP1_RX7_RATE,		0x00000001},
	{CS40L2X_DSP1_RX8_RATE,		0x00000001},
	{CS40L2X_DSP1_TX1_RATE,		0x00000001},
	{CS40L2X_DSP1_TX2_RATE,		0x00000001},
	{CS40L2X_DSP1_TX3_RATE,		0x00000001},
	{CS40L2X_DSP1_TX4_RATE,		0x00000001},
	{CS40L2X_DSP1_TX5_RATE,		0x00000001},
	{CS40L2X_DSP1_TX6_RATE,		0x00000001},
	{CS40L2X_DSP1_TX7_RATE,		0x00000001},
	{CS40L2X_DSP1_TX8_RATE,		0x00000001},
};

static int cs40l2x_dsp_start(struct cs40l2x_private *cs40l2x)
{
	struct regmap *regmap = cs40l2x->regmap;
	struct device *dev = cs40l2x->dev;
	unsigned int val;
	int dsp_timeout = CS40L2X_DSP_TIMEOUT_COUNT;
	int ret;

	ret = regmap_multi_reg_write(regmap, cs40l2x_dsp_errata,
			ARRAY_SIZE(cs40l2x_dsp_errata));
	if (ret) {
		dev_err(dev, "Failed to apply DSP-specific errata\n");
		return ret;
	}

	switch (cs40l2x->revid) {
	case CS40L2X_REVID_A0:
		ret = regmap_update_bits(regmap, CS40L2X_PWR_CTRL1,
				CS40L2X_GLOBAL_EN_MASK,
				1 << CS40L2X_GLOBAL_EN_SHIFT);
		if (ret) {
			dev_err(dev, "Failed to enable device\n");
			return ret;
		}
		break;

	default:
		ret = regmap_update_bits(regmap, CS40L2X_PWRMGT_CTL,
				CS40L2X_MEM_RDY_MASK,
				1 << CS40L2X_MEM_RDY_SHIFT);
		if (ret) {
			dev_err(dev, "Failed to set memory ready flag\n");
			return ret;
		}
	}

	ret = regmap_update_bits(regmap, CS40L2X_DSP1_CCM_CORE_CTRL,
			CS40L2X_DSP1_RESET_MASK | CS40L2X_DSP1_EN_MASK,
			(1 << CS40L2X_DSP1_RESET_SHIFT) |
				(1 << CS40L2X_DSP1_EN_SHIFT));
	if (ret) {
		dev_err(dev, "Failed to start DSP\n");
		return ret;
	}

	while (dsp_timeout > 0) {
		usleep_range(10000, 10100);

		ret = regmap_read(regmap, cs40l2x_dsp_reg(cs40l2x, "HALO_STATE",
				CS40L2X_XM_UNPACKED_TYPE), &val);
		if (ret) {
			dev_err(dev, "Failed to read DSP status\n");
			return ret;
		}

		if (val == cs40l2x->fw_desc->halo_state_run)
			break;

		dsp_timeout--;
	}

	if (dsp_timeout == 0) {
		dev_err(dev, "Timed out with DSP status = %d\n", val);
		return -ETIME;
	}

	return 0;
}

static int cs40l2x_dsp_post_config(struct cs40l2x_private *cs40l2x)
{
	struct regmap *regmap = cs40l2x->regmap;
	struct device *dev = cs40l2x->dev;
	int ret;

	if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_CAL)
		return 0;

	ret = regmap_write(regmap, cs40l2x_dsp_reg(cs40l2x, "TIMEOUT_MS",
			CS40L2X_XM_UNPACKED_TYPE), CS40L2X_TIMEOUT_MS_MAX);
	if (ret) {
		dev_err(dev, "Failed to extend playback timeout\n");
		return ret;
	}

	ret = regmap_read(regmap, cs40l2x_dsp_reg(cs40l2x, "NUMBEROFWAVES",
			CS40L2X_XM_UNPACKED_TYPE), &cs40l2x->num_waves);
	if (ret) {
		dev_err(dev, "Failed to count wavetable entries\n");
		return ret;
	}

	if (cs40l2x->num_waves == 0) {
		dev_err(dev, "Wavetable is empty\n");
		return -EINVAL;
	}

	switch (cs40l2x->fw_desc->id) {
	case CS40L2X_FW_ID_ORIG:
		ret = regmap_write(regmap,
				cs40l2x_dsp_reg(cs40l2x, "COMPENSATION_ENABLE",
						CS40L2X_XM_UNPACKED_TYPE),
				cs40l2x->comp_enable);
		break;
	case CS40L2X_FW_ID_REMAP:
		ret = regmap_write(regmap,
				cs40l2x_dsp_reg(cs40l2x, "COMPENSATION_ENABLE",
						CS40L2X_XM_UNPACKED_TYPE),
				(cs40l2x->comp_enable
					& cs40l2x->comp_enable_redc)
					<< CS40L2X_COMP_EN_REDC_SHIFT |
				(cs40l2x->comp_enable
					& cs40l2x->comp_enable_f0)
					<< CS40L2X_COMP_EN_F0_SHIFT);
		break;
	default:
		ret = -EPERM;
	}

	if (ret) {
		dev_err(dev, "Failed to configure click compensation\n");
		return ret;
	}

	if (cs40l2x->pdata.f0_default) {
		ret = regmap_write(regmap,
				cs40l2x_dsp_reg(cs40l2x, "F0_STORED",
						CS40L2X_XM_UNPACKED_TYPE),
				cs40l2x->pdata.f0_default);
		if (ret) {
			dev_err(dev, "Failed to write default f0\n");
			return ret;
		}
	}

	if (cs40l2x->pdata.redc_default) {
		ret = regmap_write(regmap,
				cs40l2x_dsp_reg(cs40l2x, "REDC_STORED",
						CS40L2X_XM_UNPACKED_TYPE),
				cs40l2x->pdata.redc_default);
		if (ret) {
			dev_err(dev, "Failed to write default ReDC\n");
			return ret;
		}
	}

	if (cs40l2x->pdata.q_default
			&& cs40l2x->fw_desc->id == CS40L2X_FW_ID_REMAP) {
		ret = regmap_write(regmap,
				cs40l2x_dsp_reg(cs40l2x, "Q_STORED",
						CS40L2X_XM_UNPACKED_TYPE),
				cs40l2x->pdata.q_default);
		if (ret) {
			dev_err(dev, "Failed to write default Q\n");
			return ret;
		}
	}

	if (cs40l2x->pdata.gpio1_rise_index > 0
			&& cs40l2x->pdata.gpio1_rise_index
				< cs40l2x->num_waves) {
		ret = regmap_write(regmap,
				cs40l2x_dsp_reg(cs40l2x, "INDEXBUTTONPRESS",
						CS40L2X_XM_UNPACKED_TYPE)
						+ CS40L2X_INDEXBUTTONPRESS1,
				cs40l2x->pdata.gpio1_rise_index);
		if (ret) {
			dev_err(dev,
				"Failed to write default gpio1_rise_index\n");
			return ret;
		}
	} else if (cs40l2x->pdata.gpio1_rise_index >= cs40l2x->num_waves) {
		dev_warn(dev, "Ignored default gpio1_rise_index\n");
	}

	if (cs40l2x->pdata.gpio1_fall_index > 0
			&& cs40l2x->pdata.gpio1_fall_index
				< cs40l2x->num_waves) {
		ret = regmap_write(regmap,
				cs40l2x_dsp_reg(cs40l2x, "INDEXBUTTONRELEASE",
						CS40L2X_XM_UNPACKED_TYPE)
						+ CS40L2X_INDEXBUTTONRELEASE1,
				cs40l2x->pdata.gpio1_fall_index);
		if (ret) {
			dev_err(dev,
				"Failed to write default gpio1_fall_index\n");
			return ret;
		}
	} else if (cs40l2x->pdata.gpio1_fall_index >= cs40l2x->num_waves) {
		dev_warn(dev, "Ignored default gpio1_fall_index\n");
	}

	if (cs40l2x->pdata.gpio1_fall_timeout > 0
			&& (cs40l2x->pdata.gpio1_fall_timeout
				& CS40L2X_PDATA_MASK)
					<= CS40L2X_PR_TIMEOUT_MAX) {
		ret = regmap_write(regmap,
				cs40l2x_dsp_reg(cs40l2x,
						"PRESS_RELEASE_TIMEOUT",
						CS40L2X_XM_UNPACKED_TYPE),
				cs40l2x->pdata.gpio1_fall_timeout
					& CS40L2X_PDATA_MASK);
		if (ret) {
			dev_err(dev,
				"Failed to write default gpio1_fall_timeout\n");
			return ret;
		}
	} else if ((cs40l2x->pdata.gpio1_fall_timeout
			& CS40L2X_PDATA_MASK) > CS40L2X_PR_TIMEOUT_MAX) {
		dev_warn(dev, "Ignored default gpio1_fall_timeout\n");
	}

	if (cs40l2x->pdata.gpio2_rise_index > 0
			&& cs40l2x->pdata.gpio2_rise_index < cs40l2x->num_waves
			&& cs40l2x->devid == CS40L2X_DEVID_L25B) {
		ret = regmap_write(regmap,
				cs40l2x_dsp_reg(cs40l2x, "INDEXBUTTONPRESS",
						CS40L2X_XM_UNPACKED_TYPE)
						+ CS40L2X_INDEXBUTTONPRESS2,
				cs40l2x->pdata.gpio2_rise_index);
		if (ret) {
			dev_err(dev,
				"Failed to write default gpio2_rise_index\n");
			return ret;
		}
	} else if ((cs40l2x->pdata.gpio2_rise_index >= cs40l2x->num_waves
			&& cs40l2x->devid == CS40L2X_DEVID_L25B)
			|| (cs40l2x->pdata.gpio2_rise_index > 0
				&& cs40l2x->devid != CS40L2X_DEVID_L25B)) {
		dev_warn(dev, "Ignored default gpio2_rise_index\n");
	}

	if (cs40l2x->pdata.gpio2_fall_index > 0
			&& cs40l2x->pdata.gpio2_fall_index < cs40l2x->num_waves
			&& cs40l2x->devid == CS40L2X_DEVID_L25B) {
		ret = regmap_write(regmap,
				cs40l2x_dsp_reg(cs40l2x, "INDEXBUTTONRELEASE",
						CS40L2X_XM_UNPACKED_TYPE)
						+ CS40L2X_INDEXBUTTONRELEASE2,
				cs40l2x->pdata.gpio2_fall_index);
		if (ret) {
			dev_err(dev,
				"Failed to write default gpio2_fall_index\n");
			return ret;
		}
	} else if ((cs40l2x->pdata.gpio2_fall_index >= cs40l2x->num_waves
			&& cs40l2x->devid == CS40L2X_DEVID_L25B)
			|| (cs40l2x->pdata.gpio2_fall_index > 0
				&& cs40l2x->devid != CS40L2X_DEVID_L25B)) {
		dev_warn(dev, "Ignored default gpio2_fall_index\n");
	}

	if (cs40l2x->pdata.gpio3_rise_index > 0
			&& cs40l2x->pdata.gpio3_rise_index < cs40l2x->num_waves
			&& cs40l2x->devid == CS40L2X_DEVID_L25B) {
		ret = regmap_write(regmap,
				cs40l2x_dsp_reg(cs40l2x, "INDEXBUTTONPRESS",
						CS40L2X_XM_UNPACKED_TYPE)
						+ CS40L2X_INDEXBUTTONPRESS3,
				cs40l2x->pdata.gpio3_rise_index);
		if (ret) {
			dev_err(dev,
				"Failed to write default gpio3_rise_index\n");
			return ret;
		}
	} else if ((cs40l2x->pdata.gpio3_rise_index >= cs40l2x->num_waves
			&& cs40l2x->devid == CS40L2X_DEVID_L25B)
			|| (cs40l2x->pdata.gpio3_rise_index > 0
				&& cs40l2x->devid != CS40L2X_DEVID_L25B)) {
		dev_warn(dev, "Ignored default gpio3_rise_index\n");
	}

	if (cs40l2x->pdata.gpio3_fall_index > 0
			&& cs40l2x->pdata.gpio3_fall_index < cs40l2x->num_waves
			&& cs40l2x->devid == CS40L2X_DEVID_L25B) {
		ret = regmap_write(regmap,
				cs40l2x_dsp_reg(cs40l2x, "INDEXBUTTONRELEASE",
						CS40L2X_XM_UNPACKED_TYPE)
						+ CS40L2X_INDEXBUTTONRELEASE3,
				cs40l2x->pdata.gpio3_fall_index);
		if (ret) {
			dev_err(dev,
				"Failed to write default gpio3_fall_index\n");
			return ret;
		}
	} else if ((cs40l2x->pdata.gpio3_fall_index >= cs40l2x->num_waves
			&& cs40l2x->devid == CS40L2X_DEVID_L25B)
			|| (cs40l2x->pdata.gpio3_fall_index > 0
				&& cs40l2x->devid != CS40L2X_DEVID_L25B)) {
		dev_warn(dev, "Ignored default gpio3_fall_index\n");
	}

	if (cs40l2x->pdata.gpio4_rise_index > 0
			&& cs40l2x->pdata.gpio4_rise_index < cs40l2x->num_waves
			&& cs40l2x->devid == CS40L2X_DEVID_L25B) {
		ret = regmap_write(regmap,
				cs40l2x_dsp_reg(cs40l2x, "INDEXBUTTONPRESS",
						CS40L2X_XM_UNPACKED_TYPE)
						+ CS40L2X_INDEXBUTTONPRESS4,
				cs40l2x->pdata.gpio4_rise_index);
		if (ret) {
			dev_err(dev,
				"Failed to write default gpio4_rise_index\n");
			return ret;
		}
	} else if ((cs40l2x->pdata.gpio4_rise_index >= cs40l2x->num_waves
			&& cs40l2x->devid == CS40L2X_DEVID_L25B)
			|| (cs40l2x->pdata.gpio4_rise_index > 0
				&& cs40l2x->devid != CS40L2X_DEVID_L25B)) {
		dev_warn(dev, "Ignored default gpio4_rise_index\n");
	}

	if (cs40l2x->pdata.gpio4_fall_index > 0
			&& cs40l2x->pdata.gpio4_fall_index < cs40l2x->num_waves
			&& cs40l2x->devid == CS40L2X_DEVID_L25B) {
		ret = regmap_write(regmap,
				cs40l2x_dsp_reg(cs40l2x, "INDEXBUTTONRELEASE",
						CS40L2X_XM_UNPACKED_TYPE)
						+ CS40L2X_INDEXBUTTONRELEASE4,
				cs40l2x->pdata.gpio4_fall_index);
		if (ret) {
			dev_err(dev,
				"Failed to write default gpio4_fall_index\n");
			return ret;
		}
	} else if ((cs40l2x->pdata.gpio4_fall_index >= cs40l2x->num_waves
			&& cs40l2x->devid == CS40L2X_DEVID_L25B)
			|| (cs40l2x->pdata.gpio4_fall_index > 0
				&& cs40l2x->devid != CS40L2X_DEVID_L25B)) {
		dev_warn(dev, "Ignored default gpio4_fall_index\n");
	}

	return cs40l2x_hw_err_chk(cs40l2x);
}

static int cs40l2x_raw_write(struct cs40l2x_private *cs40l2x, unsigned int reg,
			const void *val, size_t val_len, size_t limit)
{
	int ret, i;

	/* split "val" into smaller writes not to exceed "limit" in length */
	for (i = 0; i < val_len; i += limit) {
		ret = regmap_raw_write(cs40l2x->regmap, (reg + i), (val + i),
				(val_len - i) > limit ? limit : (val_len - i));
		if (ret)
			break;
	}

	return ret;
}

static int cs40l2x_ack_write(struct cs40l2x_private *cs40l2x, unsigned int reg,
			unsigned int val)
{
	unsigned int ack_val;
	int ret, i;

	ret = regmap_write(cs40l2x->regmap, reg, val);
	if (ret)
		return ret;

	for (i = 0; i < CS40L2X_ACK_TIMEOUT_COUNT; i++) {
		usleep_range(10000, 10100);

		ret = regmap_read(cs40l2x->regmap, reg, &ack_val);
		if (ret)
			return ret;

		if (!ack_val)
			return 0;
	}

	return -ETIME;
}

static int cs40l2x_coeff_file_parse(struct cs40l2x_private *cs40l2x,
			const struct firmware *fw)
{
	struct device *dev = cs40l2x->dev;
	struct cs40l2x_dblk_desc dblk, *dblk_base;
	struct cs40l2x_dblk_desc pre_dblks[CS40L2X_MAX_A2H_LEVELS];
	struct cs40l2x_dblk_desc a2h_dblks[CS40L2X_MAX_A2H_LEVELS];
	unsigned int *dblk_index;
	unsigned int pre_index = 0;
	unsigned int a2h_index = 0;
	unsigned int pos = CS40L2X_WT_FILE_HEADER_SIZE;
	unsigned int block_offset, block_type, block_length;
	unsigned int algo_id, algo_rev, reg;
	int ret = -EINVAL;
	int i;

	if (memcmp(fw->data, "WMDR", 4)) {
		dev_err(dev, "Failed to recognize coefficient file\n");
		goto err_rls_fw;
	}

	if (fw->size % 4) {
		dev_err(dev, "Coefficient file is not word-aligned\n");
		goto err_rls_fw;
	}

	while (pos < fw->size) {
		block_offset = fw->data[pos]
				+ (fw->data[pos + 1] << 8);
		pos += CS40L2X_WT_DBLK_OFFSET_SIZE;

		block_type = fw->data[pos]
				+ (fw->data[pos + 1] << 8);
		pos += CS40L2X_WT_DBLK_TYPE_SIZE;

		algo_id = fw->data[pos]
				+ (fw->data[pos + 1] << 8)
				+ (fw->data[pos + 2] << 16)
				+ (fw->data[pos + 3] << 24);
		pos += CS40L2X_WT_ALGO_ID_SIZE;

		algo_rev = fw->data[pos]
				+ (fw->data[pos + 1] << 8)
				+ (fw->data[pos + 2] << 16)
				+ (fw->data[pos + 3] << 24);
		pos += CS40L2X_WT_ALGO_REV_SIZE;

		/* sample rate is not used here */
		pos += CS40L2X_WT_SAMPLE_RATE_SIZE;

		block_length = fw->data[pos]
				+ (fw->data[pos + 1] << 8)
				+ (fw->data[pos + 2] << 16)
				+ (fw->data[pos + 3] << 24);
		pos += CS40L2X_WT_DBLK_LENGTH_SIZE;

		if (block_type != CS40L2X_WMDR_NAME_TYPE) {
			for (i = 0; i < cs40l2x->num_algos; i++)
				if (algo_id == cs40l2x->algo_info[i].id)
					break;
			if (i == cs40l2x->num_algos) {
				dev_err(dev, "Invalid algo. ID: 0x%06X\n",
						algo_id);
				ret = -EINVAL;
				goto err_rls_fw;
			}

			if (((algo_rev >> 8) & CS40L2X_ALGO_REV_MASK)
					!= (cs40l2x->algo_info[i].rev
						& CS40L2X_ALGO_REV_MASK)) {
				dev_err(dev, "Invalid algo. rev.: %d.%d.%d\n",
						(algo_rev & 0xFF000000) >> 24,
						(algo_rev & 0xFF0000) >> 16,
						(algo_rev & 0xFF00) >> 8);
				ret = -EINVAL;
				goto err_rls_fw;
			}

			switch (algo_id) {
			case CS40L2X_ALGO_ID_PRE:
				dblk_index = &pre_index;
				dblk_base = pre_dblks;
				break;
			case CS40L2X_ALGO_ID_A2H:
				dblk_index = &a2h_index;
				dblk_base = a2h_dblks;
				break;
			case CS40L2X_ALGO_ID_EXC:
				cs40l2x->exc_available = true;
				/* intentionally fall through */
			default:
				dblk_index = NULL;
			}
		}

		switch (block_type) {
		case CS40L2X_WMDR_NAME_TYPE:
			reg = 0;
			dblk_index = NULL;
			break;
		case CS40L2X_XM_UNPACKED_TYPE:
			reg = CS40L2X_DSP1_XMEM_UNPACK24_0 + block_offset
					+ cs40l2x->algo_info[i].xm_base * 4;

			if (block_length > cs40l2x->wt_limit_xm
					&& reg == cs40l2x_dsp_reg(cs40l2x,
						"WAVETABLE",
						CS40L2X_XM_UNPACKED_TYPE)) {
				dev_err(dev,
					"Wavetable too large: %d bytes (XM)\n",
					block_length / 4 * 3);
				ret = -EINVAL;
				goto err_rls_fw;
			}
			break;
		case CS40L2X_YM_UNPACKED_TYPE:
			reg = CS40L2X_DSP1_YMEM_UNPACK24_0 + block_offset
					+ cs40l2x->algo_info[i].ym_base * 4;

			if (block_length > cs40l2x->wt_limit_ym
					&& reg == cs40l2x_dsp_reg(cs40l2x,
						"WAVETABLEYM",
						CS40L2X_YM_UNPACKED_TYPE)) {
				dev_err(dev,
					"Wavetable too large: %d bytes (YM)\n",
					block_length / 4 * 3);
				ret = -EINVAL;
				goto err_rls_fw;
			}
			break;
		default:
			dev_err(dev, "Unexpected block type: 0x%04X\n",
					block_type);
			ret = -EINVAL;
			goto err_rls_fw;
		}

		/* not all data blocks go directly to DSP */
		if (dblk_index) {
			if (*dblk_index >= CS40L2X_MAX_A2H_LEVELS) {
				dev_err(dev, "Too many A2H blocks\n");
				ret = -E2BIG;
				goto err_rls_fw;
			}

			dblk = *(dblk_base + *dblk_index);

			dblk.data = devm_kzalloc(dev, block_length, GFP_KERNEL);
			if (!dblk.data) {
				ret = -ENOMEM;
				goto err_rls_fw;
			}

			memcpy(dblk.data, &fw->data[pos], block_length);
			dblk.length = block_length;
			dblk.reg = reg;

			/* cancel register write for all but "off" level */
			if (*dblk_index)
				reg = 0;

			*(dblk_base + (*dblk_index)++) = dblk;
		}

		if (reg) {
			ret = cs40l2x_raw_write(cs40l2x, reg, &fw->data[pos],
					block_length, CS40L2X_MAX_WLEN);
			if (ret) {
				dev_err(dev, "Failed to write coefficients\n");
				goto err_rls_fw;
			}
		} else {
			ret = 0;
		}

		/* blocks are word-aligned */
		pos += (block_length + 3) & ~0x00000003;
	}

	if (a2h_index) {
		if (a2h_index != pre_index) {
			dev_err(dev, "Invalid number of A2H blocks\n");
			ret = -EINVAL;
			goto err_rls_fw;
		}

		for (i = 0; i < a2h_index; i++) {
			cs40l2x->pre_dblks[i] = pre_dblks[i];
			cs40l2x->a2h_dblks[i] = a2h_dblks[i];
		}
		cs40l2x->num_a2h_levels = a2h_index;
	} else {
		for (i = 0; i < pre_index; i++)
			devm_kfree(dev, pre_dblks[i].data);
	}

err_rls_fw:
	release_firmware(fw);

	return ret;
}

static void cs40l2x_coeff_file_load(const struct firmware *fw, void *context)
{
	struct cs40l2x_private *cs40l2x = (struct cs40l2x_private *)context;
	unsigned int num_coeff_files = 0;
	int ret = 0;

	mutex_lock(&cs40l2x->lock);

	if (fw)
		ret = cs40l2x_coeff_file_parse(cs40l2x, fw);

	if (!ret)
		num_coeff_files = ++(cs40l2x->num_coeff_files);

	mutex_unlock(&cs40l2x->lock);

	if (num_coeff_files == cs40l2x->fw_desc->num_coeff_files) {
		ret = cs40l2x_dsp_pre_config(cs40l2x);
		if (ret)
			return;

		ret = cs40l2x_dsp_start(cs40l2x);
		if (ret)
			return;

		ret = cs40l2x_dsp_post_config(cs40l2x);
		if (ret)
			return;

		cs40l2x_vibe_init(cs40l2x);
	}
}

static int cs40l2x_algo_parse(struct cs40l2x_private *cs40l2x,
		const unsigned char *data)
{
	struct cs40l2x_coeff_desc *coeff_desc;
	unsigned int pos = 0;
	unsigned int algo_id, algo_desc_length, coeff_count;
	unsigned int block_offset, block_type, block_length;
	unsigned char algo_name_length;
	int i;

	/* record algorithm ID */
	algo_id = *(data + pos)
			+ (*(data + pos + 1) << 8)
			+ (*(data + pos + 2) << 16)
			+ (*(data + pos + 3) << 24);
	pos += CS40L2X_ALGO_ID_SIZE;

	/* skip past algorithm name */
	algo_name_length = *(data + pos);
	pos += ((algo_name_length / 4) * 4) + 4;

	/* skip past algorithm description */
	algo_desc_length = *(data + pos)
			+ (*(data + pos + 1) << 8);
	pos += ((algo_desc_length / 4) * 4) + 4;

	/* record coefficient count */
	coeff_count = *(data + pos)
			+ (*(data + pos + 1) << 8)
			+ (*(data + pos + 2) << 16)
			+ (*(data + pos + 3) << 24);
	pos += CS40L2X_COEFF_COUNT_SIZE;

	for (i = 0; i < coeff_count; i++) {
		block_offset = *(data + pos)
				+ (*(data + pos + 1) << 8);
		pos += CS40L2X_COEFF_OFFSET_SIZE;

		block_type = *(data + pos)
				+ (*(data + pos + 1) << 8);
		pos += CS40L2X_COEFF_TYPE_SIZE;

		block_length = *(data + pos)
				+ (*(data + pos + 1) << 8)
				+ (*(data + pos + 2) << 16)
				+ (*(data + pos + 3) << 24);
		pos += CS40L2X_COEFF_LENGTH_SIZE;

		coeff_desc = kzalloc(sizeof(*coeff_desc), GFP_KERNEL);
		if (!coeff_desc)
			return -ENOMEM;

		coeff_desc->parent_id = algo_id;
		coeff_desc->block_offset = block_offset;
		coeff_desc->block_type = block_type;

		memcpy(coeff_desc->name, data + pos + 1, *(data + pos));
		coeff_desc->name[*(data + pos)] = '\0';

		list_add(&coeff_desc->list, &cs40l2x->coeff_desc_head);

		pos += block_length;
	}

	return 0;
}

static int cs40l2x_firmware_parse(struct cs40l2x_private *cs40l2x,
			const struct firmware *fw)
{
	struct device *dev = cs40l2x->dev;
	unsigned int pos = CS40L2X_FW_FILE_HEADER_SIZE;
	unsigned int block_offset, block_type, block_length;
	int ret = -EINVAL;

	if (memcmp(fw->data, "WMFW", 4)) {
		dev_err(dev, "Failed to recognize firmware file\n");
		goto err_rls_fw;
	}

	if (fw->size % 4) {
		dev_err(dev, "Firmware file is not word-aligned\n");
		goto err_rls_fw;
	}

	while (pos < fw->size) {
		block_offset = fw->data[pos]
				+ (fw->data[pos + 1] << 8)
				+ (fw->data[pos + 2] << 16);
		pos += CS40L2X_FW_DBLK_OFFSET_SIZE;

		block_type = fw->data[pos];
		pos += CS40L2X_FW_DBLK_TYPE_SIZE;

		block_length = fw->data[pos]
				+ (fw->data[pos + 1] << 8)
				+ (fw->data[pos + 2] << 16)
				+ (fw->data[pos + 3] << 24);
		pos += CS40L2X_FW_DBLK_LENGTH_SIZE;

		switch (block_type) {
		case CS40L2X_WMFW_INFO_TYPE:
			break;
		case CS40L2X_PM_PACKED_TYPE:
			ret = cs40l2x_raw_write(cs40l2x,
					CS40L2X_DSP1_PMEM_0
						+ block_offset * 5,
					&fw->data[pos], block_length,
					CS40L2X_MAX_WLEN);
			if (ret) {
				dev_err(dev,
					"Failed to write PM_PACKED memory\n");
				goto err_rls_fw;
			}
			break;
		case CS40L2X_XM_PACKED_TYPE:
			ret = cs40l2x_raw_write(cs40l2x,
					CS40L2X_DSP1_XMEM_PACK_0
						+ block_offset * 3,
					&fw->data[pos], block_length,
					CS40L2X_MAX_WLEN);
			if (ret) {
				dev_err(dev,
					"Failed to write XM_PACKED memory\n");
				goto err_rls_fw;
			}
			break;
		case CS40L2X_YM_PACKED_TYPE:
			ret = cs40l2x_raw_write(cs40l2x,
					CS40L2X_DSP1_YMEM_PACK_0
						+ block_offset * 3,
					&fw->data[pos], block_length,
					CS40L2X_MAX_WLEN);
			if (ret) {
				dev_err(dev,
					"Failed to write YM_PACKED memory\n");
				goto err_rls_fw;
			}
			break;
		case CS40L2X_ALGO_INFO_TYPE:
			ret = cs40l2x_algo_parse(cs40l2x, &fw->data[pos]);
			if (ret) {
				dev_err(dev,
					"Failed to parse algorithm: %d\n", ret);
				goto err_rls_fw;
			}
			break;
		default:
			dev_err(dev, "Unexpected block type: 0x%02X\n",
					block_type);
			ret = -EINVAL;
			goto err_rls_fw;
		}

		/* blocks are word-aligned */
		pos += (block_length + 3) & ~0x00000003;
	}

	ret = cs40l2x_coeff_init(cs40l2x);

err_rls_fw:
	release_firmware(fw);

	return ret;
}

static void cs40l2x_firmware_load(const struct firmware *fw, void *context)
{
	struct cs40l2x_private *cs40l2x = (struct cs40l2x_private *)context;
	struct device *dev = cs40l2x->dev;
	int ret, i;

	if (!fw) {
		dev_err(dev, "Failed to request firmware file\n");
		return;
	}

	ret = cs40l2x_firmware_parse(cs40l2x, fw);
	if (ret)
		return;

	for (i = 0; i < cs40l2x->fw_desc->num_coeff_files; i++)
		request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG,
				cs40l2x->fw_desc->coeff_files[i], dev,
				GFP_KERNEL, cs40l2x, cs40l2x_coeff_file_load);
}

static int cs40l2x_firmware_swap(struct cs40l2x_private *cs40l2x,
			unsigned int fw_id)
{
	const struct firmware *fw;
	struct regmap *regmap = cs40l2x->regmap;
	struct device *dev = cs40l2x->dev;
	int ret, i;

	if (cs40l2x->vibe_mode != CS40L2X_VIBE_MODE_HAPTIC)
		return -EPERM;

	switch (cs40l2x->fw_desc->id) {
	case CS40L2X_FW_ID_ORIG:
		return -EPERM;

	case CS40L2X_FW_ID_B1ROM:
		ret = cs40l2x_basic_mode_exit(cs40l2x);
		if (ret)
			return ret;
		break;

	case CS40L2X_FW_ID_REMAP:
		ret = regmap_write(regmap,
				cs40l2x_dsp_reg(cs40l2x, "EVENTCONTROL",
						CS40L2X_XM_UNPACKED_TYPE),
				CS40L2X_EVENT_DISABLED);
		if (ret) {
			dev_err(dev, "Failed to disable event controls\n");
			return ret;
		}

		ret = (cs40l2x->pbq_state != CS40L2X_PBQ_STATE_IDLE) ?
				cs40l2x_pbq_cancel(cs40l2x) :
				cs40l2x_stop_playback(cs40l2x);
		if (ret) {
			dev_err(dev, "Failed to stop playback\n");
			return ret;
		}

		ret = cs40l2x_hiber_cmd_send(cs40l2x,
				CS40L2X_POWERCONTROL_FRC_STDBY);
		if (ret) {
			dev_err(dev, "Failed to force standby\n");
			return ret;
		}
		break;

	case CS40L2X_FW_ID_CAL:
		ret = cs40l2x_diag_enable(cs40l2x, 0);
		if (ret) {
			dev_err(dev, "Failed to disable diagnostics tone\n");
			return ret;
		}

		ret = cs40l2x_ack_write(cs40l2x,
				cs40l2x_dsp_reg(cs40l2x, "SHUTDOWNREQUEST",
						CS40L2X_XM_UNPACKED_TYPE), 1);
		if (ret) {
			dev_err(dev, "Failed to administer shutdown request\n");
			return ret;
		}
		break;

	default:
		return -EINVAL;
	}

	ret = regmap_update_bits(regmap, CS40L2X_DSP1_CCM_CORE_CTRL,
			CS40L2X_DSP1_EN_MASK, (0 << CS40L2X_DSP1_EN_SHIFT));
	if (ret) {
		dev_err(dev, "Failed to stop DSP\n");
		return ret;
	}

	cs40l2x_coeff_free(cs40l2x);

	if (fw_id == CS40L2X_FW_ID_CAL)
		cs40l2x->diag_state = CS40L2X_DIAG_STATE_INIT;

	cs40l2x->exc_available = false;

	for (i = 0; i < cs40l2x->num_a2h_levels; i++) {
		devm_kfree(dev, cs40l2x->pre_dblks[i].data);
		devm_kfree(dev, cs40l2x->a2h_dblks[i].data);
	}
	cs40l2x->num_a2h_levels = 0;
	cs40l2x->a2h_level = 0;

	cs40l2x->fw_desc = cs40l2x_firmware_match(cs40l2x, fw_id);
	if (!cs40l2x->fw_desc)
		return -EINVAL;

	ret = request_firmware(&fw, cs40l2x->fw_desc->fw_file, dev);
	if (ret) {
		dev_err(dev, "Failed to request firmware file\n");
		return ret;
	}

	ret = cs40l2x_firmware_parse(cs40l2x, fw);
	if (ret)
		return ret;

	for (i = 0; i < cs40l2x->fw_desc->num_coeff_files; i++) {
		ret = request_firmware(&fw, cs40l2x->fw_desc->coeff_files[i],
				dev);
		if (ret)
			continue;

		ret = cs40l2x_coeff_file_parse(cs40l2x, fw);
		if (ret)
			return ret;
	}

	ret = cs40l2x_dsp_pre_config(cs40l2x);
	if (ret)
		return ret;

	ret = cs40l2x_dsp_start(cs40l2x);
	if (ret)
		return ret;

	return cs40l2x_dsp_post_config(cs40l2x);
}

static int cs40l2x_boost_config(struct cs40l2x_private *cs40l2x)
{
	struct regmap *regmap = cs40l2x->regmap;
	struct device *dev = cs40l2x->dev;
	unsigned int boost_ind = cs40l2x->pdata.boost_ind;
	unsigned int boost_cap = cs40l2x->pdata.boost_cap;
	unsigned int boost_ipk = cs40l2x->pdata.boost_ipk;
	unsigned int boost_ctl = cs40l2x->pdata.boost_ctl;
	unsigned int boost_ovp = cs40l2x->pdata.boost_ovp;
	unsigned int bst_lbst_val, bst_cbst_range;
	unsigned int bst_ipk_scaled, bst_ctl_scaled, bst_ovp_scaled;
	int ret;

	switch (boost_ind) {
	case 1000:	/* 1.0 uH */
		bst_lbst_val = 0;
		break;
	case 1200:	/* 1.2 uH */
		bst_lbst_val = 1;
		break;
	case 1500:	/* 1.5 uH */
		bst_lbst_val = 2;
		break;
	case 2200:	/* 2.2 uH */
		bst_lbst_val = 3;
		break;
	default:
		dev_err(dev, "Invalid boost inductor value: %d nH\n",
				boost_ind);
		return -EINVAL;
	}

	switch (boost_cap) {
	case 0 ... 19:
		bst_cbst_range = 0;
		break;
	case 20 ... 50:
		bst_cbst_range = 1;
		break;
	case 51 ... 100:
		bst_cbst_range = 2;
		break;
	case 101 ... 200:
		bst_cbst_range = 3;
		break;
	default:	/* 201 uF and greater */
		bst_cbst_range = 4;
	}

	ret = regmap_update_bits(regmap, CS40L2X_BSTCVRT_COEFF,
			CS40L2X_BST_K1_MASK,
			cs40l2x_bst_k1_table[bst_lbst_val][bst_cbst_range]
				<< CS40L2X_BST_K1_SHIFT);
	if (ret) {
		dev_err(dev, "Failed to write boost K1 coefficient\n");
		return ret;
	}

	ret = regmap_update_bits(regmap, CS40L2X_BSTCVRT_COEFF,
			CS40L2X_BST_K2_MASK,
			cs40l2x_bst_k2_table[bst_lbst_val][bst_cbst_range]
				<< CS40L2X_BST_K2_SHIFT);
	if (ret) {
		dev_err(dev, "Failed to write boost K2 coefficient\n");
		return ret;
	}

	ret = cs40l2x_wseq_add_reg(cs40l2x, CS40L2X_BSTCVRT_COEFF,
			(cs40l2x_bst_k2_table[bst_lbst_val][bst_cbst_range]
				<< CS40L2X_BST_K2_SHIFT) |
			(cs40l2x_bst_k1_table[bst_lbst_val][bst_cbst_range]
				<< CS40L2X_BST_K1_SHIFT));
	if (ret) {
		dev_err(dev, "Failed to sequence boost K1/K2 coefficients\n");
		return ret;
	}

	ret = regmap_update_bits(regmap, CS40L2X_BSTCVRT_SLOPE_LBST,
			CS40L2X_BST_SLOPE_MASK,
			cs40l2x_bst_slope_table[bst_lbst_val]
				<< CS40L2X_BST_SLOPE_SHIFT);
	if (ret) {
		dev_err(dev, "Failed to write boost slope coefficient\n");
		return ret;
	}

	ret = regmap_update_bits(regmap, CS40L2X_BSTCVRT_SLOPE_LBST,
			CS40L2X_BST_LBST_VAL_MASK,
			bst_lbst_val << CS40L2X_BST_LBST_VAL_SHIFT);
	if (ret) {
		dev_err(dev, "Failed to write boost inductor value\n");
		return ret;
	}

	ret = cs40l2x_wseq_add_reg(cs40l2x, CS40L2X_BSTCVRT_SLOPE_LBST,
			(cs40l2x_bst_slope_table[bst_lbst_val]
				<< CS40L2X_BST_SLOPE_SHIFT) |
			(bst_lbst_val << CS40L2X_BST_LBST_VAL_SHIFT));
	if (ret) {
		dev_err(dev, "Failed to sequence boost inductor value\n");
		return ret;
	}

	if ((boost_ipk < 1600) || (boost_ipk > 4500)) {
		dev_err(dev, "Invalid boost inductor peak current: %d mA\n",
				boost_ipk);
		return -EINVAL;
	}
	bst_ipk_scaled = ((boost_ipk - 1600) / 50) + 0x10;

	ret = regmap_update_bits(regmap, CS40L2X_BSTCVRT_PEAK_CUR,
			CS40L2X_BST_IPK_MASK,
			bst_ipk_scaled << CS40L2X_BST_IPK_SHIFT);
	if (ret) {
		dev_err(dev, "Failed to write boost inductor peak current\n");
		return ret;
	}

	ret = cs40l2x_wseq_add_reg(cs40l2x, CS40L2X_BSTCVRT_PEAK_CUR,
			bst_ipk_scaled << CS40L2X_BST_IPK_SHIFT);
	if (ret) {
		dev_err(dev,
			"Failed to sequence boost inductor peak current\n");
		return ret;
	}

	if (boost_ctl) {
		boost_ctl &= CS40L2X_PDATA_MASK;

		switch (boost_ctl) {
		case 0:
			bst_ctl_scaled = boost_ctl;
			break;
		case 2550 ... 11000:
			bst_ctl_scaled = ((boost_ctl - 2550) / 50) + 1;
			break;
		default:
			dev_err(dev, "Invalid VBST limit: %d mV\n", boost_ctl);
			return -EINVAL;
		}

		ret = regmap_update_bits(regmap, CS40L2X_BSTCVRT_VCTRL1,
				CS40L2X_BST_CTL_MASK,
				bst_ctl_scaled << CS40L2X_BST_CTL_SHIFT);
		if (ret) {
			dev_err(dev, "Failed to write VBST limit\n");
			return ret;
		}

		ret = cs40l2x_wseq_add_reg(cs40l2x, CS40L2X_BSTCVRT_VCTRL1,
				bst_ctl_scaled << CS40L2X_BST_CTL_SHIFT);
		if (ret) {
			dev_err(dev, "Failed to sequence VBST limit\n");
			return ret;
		}

		ret = regmap_update_bits(regmap, CS40L2X_BSTCVRT_VCTRL2,
				CS40L2X_BST_CTL_LIM_EN_MASK,
				1 << CS40L2X_BST_CTL_LIM_EN_SHIFT);
		if (ret) {
			dev_err(dev, "Failed to configure VBST control\n");
			return ret;
		}

		ret = cs40l2x_wseq_add_reg(cs40l2x, CS40L2X_BSTCVRT_VCTRL2,
				(1 << CS40L2X_BST_CTL_LIM_EN_SHIFT) |
				(CS40L2X_BST_CTL_SEL_CLASSH
					<< CS40L2X_BST_CTL_SEL_SHIFT));
		if (ret) {
			dev_err(dev, "Failed to sequence VBST control\n");
			return ret;
		}
	}

	switch (boost_ovp) {
	case 0:
		break;
	case 9000 ... 12875:
		bst_ovp_scaled = ((boost_ovp - 9000) / 125) * 2;

		ret = regmap_update_bits(regmap, CS40L2X_BSTCVRT_OVERVOLT_CTRL,
				CS40L2X_BST_OVP_THLD_MASK,
				bst_ovp_scaled << CS40L2X_BST_OVP_THLD_SHIFT);
		if (ret) {
			dev_err(dev, "Failed to write OVP threshold\n");
			return ret;
		}

		ret = cs40l2x_wseq_add_reg(cs40l2x,
				CS40L2X_BSTCVRT_OVERVOLT_CTRL,
				(1 << CS40L2X_BST_OVP_EN_SHIFT) |
				(bst_ovp_scaled << CS40L2X_BST_OVP_THLD_SHIFT));
		if (ret) {
			dev_err(dev, "Failed to sequence OVP threshold\n");
			return ret;
		}
		break;
	default:
		dev_err(dev, "Invalid OVP threshold: %d mV\n", boost_ovp);
		return -EINVAL;
	}

	return 0;
}

static int cs40l2x_asp_config(struct cs40l2x_private *cs40l2x)
{
	struct regmap *regmap = cs40l2x->regmap;
	struct device *dev = cs40l2x->dev;
	unsigned int asp_bclk_freq = cs40l2x->pdata.asp_bclk_freq;
	bool asp_bclk_inv = cs40l2x->pdata.asp_bclk_inv;
	bool asp_fsync_inv = cs40l2x->pdata.asp_fsync_inv;
	unsigned int asp_fmt = cs40l2x->pdata.asp_fmt;
	unsigned int asp_slot_num = cs40l2x->pdata.asp_slot_num;
	unsigned int asp_slot_width = cs40l2x->pdata.asp_slot_width;
	unsigned int asp_samp_width = cs40l2x->pdata.asp_samp_width;
	unsigned int asp_frame_cfg = 0;
	int ret, i;

	if (asp_bclk_freq % asp_slot_width
			|| asp_slot_width < CS40L2X_ASP_RX_WIDTH_MIN
			|| asp_slot_width > CS40L2X_ASP_RX_WIDTH_MAX) {
		dev_err(dev, "Invalid ASP slot width: %d bits\n",
				asp_slot_width);
		return -EINVAL;
	}

	if (asp_samp_width > asp_slot_width
			|| asp_samp_width < CS40L2X_ASP_RX_WL_MIN
			|| asp_samp_width > CS40L2X_ASP_RX_WL_MAX) {
		dev_err(dev, "Invalid ASP sample width: %d bits\n",
				asp_samp_width);
		return -EINVAL;
	}

	if (asp_fmt) {
		asp_fmt &= CS40L2X_PDATA_MASK;

		switch (asp_fmt) {
		case CS40L2X_ASP_FMT_TDM1:
		case CS40L2X_ASP_FMT_I2S:
		case CS40L2X_ASP_FMT_TDM1R5:
			break;
		default:
			dev_err(dev, "Invalid ASP format: %d\n", asp_fmt);
			return -EINVAL;
		}
	} else {
		asp_fmt = CS40L2X_ASP_FMT_I2S;
	}

	if (asp_slot_num > CS40L2X_ASP_RX1_SLOT_MAX) {
		dev_err(dev, "Invalid ASP slot number: %d\n", asp_slot_num);
		return -EINVAL;
	}

	ret = cs40l2x_wseq_add_reg(cs40l2x, CS40L2X_SP_ENABLES, 0);
	if (ret) {
		dev_err(dev, "Failed to sequence ASP enable controls\n");
		return ret;
	}

	for (i = 0; i < CS40L2X_NUM_REFCLKS; i++)
		if (cs40l2x_refclks[i].freq == asp_bclk_freq)
			break;
	if (i == CS40L2X_NUM_REFCLKS) {
		dev_err(dev, "Invalid ASP_BCLK frequency: %d Hz\n",
				asp_bclk_freq);
		return -EINVAL;
	}

	ret = regmap_update_bits(regmap, CS40L2X_SP_RATE_CTRL,
			CS40L2X_ASP_BCLK_FREQ_MASK,
			i << CS40L2X_ASP_BCLK_FREQ_SHIFT);
	if (ret) {
		dev_err(dev, "Failed to write ASP_BCLK frequency\n");
		return ret;
	}

	ret = cs40l2x_wseq_add_reg(cs40l2x, CS40L2X_SP_RATE_CTRL,
			i << CS40L2X_ASP_BCLK_FREQ_SHIFT);
	if (ret) {
		dev_err(dev, "Failed to sequence ASP_BCLK frequency\n");
		return ret;
	}

	ret = regmap_write(regmap, CS40L2X_FS_MON_0, cs40l2x_refclks[i].coeff);
	if (ret) {
		dev_err(dev, "Failed to write ASP coefficients\n");
		return ret;
	}

	ret = cs40l2x_wseq_add_reg(cs40l2x, CS40L2X_FS_MON_0,
			cs40l2x_refclks[i].coeff);
	if (ret) {
		dev_err(dev, "Failed to sequence ASP coefficients\n");
		return ret;
	}

	asp_frame_cfg |= (asp_slot_width << CS40L2X_ASP_RX_WIDTH_SHIFT);
	asp_frame_cfg |= (asp_slot_width << CS40L2X_ASP_TX_WIDTH_SHIFT);
	asp_frame_cfg |= (asp_fmt << CS40L2X_ASP_FMT_SHIFT);
	asp_frame_cfg |= (asp_bclk_inv ? CS40L2X_ASP_BCLK_INV_MASK : 0);
	asp_frame_cfg |= (asp_fsync_inv ? CS40L2X_ASP_FSYNC_INV_MASK : 0);

	ret = regmap_write(regmap, CS40L2X_SP_FORMAT, asp_frame_cfg);
	if (ret) {
		dev_err(dev, "Failed to write ASP frame configuration\n");
		return ret;
	}

	ret = cs40l2x_wseq_add_reg(cs40l2x, CS40L2X_SP_FORMAT, asp_frame_cfg);
	if (ret) {
		dev_err(dev, "Failed to sequence ASP frame configuration\n");
		return ret;
	}

	ret = regmap_update_bits(regmap, CS40L2X_SP_FRAME_RX_SLOT,
			CS40L2X_ASP_RX1_SLOT_MASK,
			asp_slot_num << CS40L2X_ASP_RX1_SLOT_SHIFT);
	if (ret) {
		dev_err(dev, "Failed to write ASP slot number\n");
		return ret;
	}

	ret = cs40l2x_wseq_add_reg(cs40l2x, CS40L2X_SP_FRAME_RX_SLOT,
			asp_slot_num << CS40L2X_ASP_RX1_SLOT_SHIFT);
	if (ret) {
		dev_err(dev, "Failed to sequence ASP slot number\n");
		return ret;
	}

	ret = regmap_update_bits(regmap, CS40L2X_SP_RX_WL,
			CS40L2X_ASP_RX_WL_MASK,
			asp_samp_width << CS40L2X_ASP_RX_WL_SHIFT);
	if (ret) {
		dev_err(dev, "Failed to write ASP sample width\n");
		return ret;
	}

	ret = cs40l2x_wseq_add_reg(cs40l2x, CS40L2X_SP_RX_WL,
			asp_samp_width << CS40L2X_ASP_RX_WL_SHIFT);
	if (ret) {
		dev_err(dev, "Failed to sequence ASP sample width\n");
		return ret;
	}

	return 0;
}

static int cs40l2x_brownout_config(struct cs40l2x_private *cs40l2x)
{
	struct regmap *regmap = cs40l2x->regmap;
	struct device *dev = cs40l2x->dev;
	bool vpbr_enable = cs40l2x->pdata.vpbr_enable;
	bool vbbr_enable = cs40l2x->pdata.vbbr_enable;
	unsigned int vpbr_thld1 = cs40l2x->pdata.vpbr_thld1;
	unsigned int vbbr_thld1 = cs40l2x->pdata.vbbr_thld1;
	unsigned int vpbr_thld1_scaled, vbbr_thld1_scaled, val;
	int ret;

	if (!vpbr_enable && !vbbr_enable)
		return 0;

	ret = regmap_read(regmap, CS40L2X_PWR_CTRL3, &val);
	if (ret) {
		dev_err(dev, "Failed to read VPBR/VBBR enable controls\n");
		return ret;
	}

	val |= (vpbr_enable ? CS40L2X_VPBR_EN_MASK : 0);
	val |= (vbbr_enable ? CS40L2X_VBBR_EN_MASK : 0);

	ret = regmap_write(regmap, CS40L2X_PWR_CTRL3, val);
	if (ret) {
		dev_err(dev, "Failed to write VPBR/VBBR enable controls\n");
		return ret;
	}

	ret = cs40l2x_wseq_add_reg(cs40l2x, CS40L2X_PWR_CTRL3, val);
	if (ret) {
		dev_err(dev, "Failed to sequence VPBR/VBBR enable controls\n");
		return ret;
	}

	if (vpbr_thld1) {
		if ((vpbr_thld1 < 2497) || (vpbr_thld1 > 3874)) {
			dev_err(dev, "Invalid VPBR threshold: %d mV\n",
					vpbr_thld1);
			return -EINVAL;
		}
		vpbr_thld1_scaled = ((vpbr_thld1 - 2497) * 1000 / 47482) + 0x02;

		ret = regmap_read(regmap, CS40L2X_VPBR_CFG, &val);
		if (ret) {
			dev_err(dev, "Failed to read VPBR configuration\n");
			return ret;
		}

		val &= ~CS40L2X_VPBR_THLD1_MASK;
		val |= (vpbr_thld1_scaled << CS40L2X_VPBR_THLD1_SHIFT);

		ret = regmap_write(regmap, CS40L2X_VPBR_CFG, val);
		if (ret) {
			dev_err(dev, "Failed to write VPBR configuration\n");
			return ret;
		}

		ret = cs40l2x_wseq_add_reg(cs40l2x, CS40L2X_VPBR_CFG, val);
		if (ret) {
			dev_err(dev, "Failed to sequence VPBR configuration\n");
			return ret;
		}
	}

	if (vbbr_thld1) {
		if ((vbbr_thld1 < 109) || (vbbr_thld1 > 3445)) {
			dev_err(dev, "Invalid VBBR threshold: %d mV\n",
					vbbr_thld1);
			return -EINVAL;
		}
		vbbr_thld1_scaled = ((vbbr_thld1 - 109) * 1000 / 54688) + 0x02;

		ret = regmap_read(regmap, CS40L2X_VBBR_CFG, &val);
		if (ret) {
			dev_err(dev, "Failed to read VBBR configuration\n");
			return ret;
		}

		val &= ~CS40L2X_VBBR_THLD1_MASK;
		val |= (vbbr_thld1_scaled << CS40L2X_VBBR_THLD1_SHIFT);

		ret = regmap_write(regmap, CS40L2X_VBBR_CFG, val);
		if (ret) {
			dev_err(dev, "Failed to write VBBR configuration\n");
			return ret;
		}

		ret = cs40l2x_wseq_add_reg(cs40l2x, CS40L2X_VBBR_CFG, val);
		if (ret) {
			dev_err(dev, "Failed to sequence VBBR configuration\n");
			return ret;
		}
	}

	return 0;
}

static const struct reg_sequence cs40l2x_mpu_config[] = {
	{CS40L2X_DSP1_MPU_LOCK_CONFIG,	CS40L2X_MPU_UNLOCK_CODE1},
	{CS40L2X_DSP1_MPU_LOCK_CONFIG,	CS40L2X_MPU_UNLOCK_CODE2},
	{CS40L2X_DSP1_MPU_XM_ACCESS0,	0xFFFFFFFF},
	{CS40L2X_DSP1_MPU_YM_ACCESS0,	0xFFFFFFFF},
	{CS40L2X_DSP1_MPU_WNDW_ACCESS0,	0xFFFFFFFF},
	{CS40L2X_DSP1_MPU_XREG_ACCESS0,	0xFFFFFFFF},
	{CS40L2X_DSP1_MPU_YREG_ACCESS0,	0xFFFFFFFF},
	{CS40L2X_DSP1_MPU_WNDW_ACCESS1,	0xFFFFFFFF},
	{CS40L2X_DSP1_MPU_XREG_ACCESS1,	0xFFFFFFFF},
	{CS40L2X_DSP1_MPU_YREG_ACCESS1,	0xFFFFFFFF},
	{CS40L2X_DSP1_MPU_WNDW_ACCESS2,	0xFFFFFFFF},
	{CS40L2X_DSP1_MPU_XREG_ACCESS2,	0xFFFFFFFF},
	{CS40L2X_DSP1_MPU_YREG_ACCESS2,	0xFFFFFFFF},
	{CS40L2X_DSP1_MPU_WNDW_ACCESS3,	0xFFFFFFFF},
	{CS40L2X_DSP1_MPU_XREG_ACCESS3,	0xFFFFFFFF},
	{CS40L2X_DSP1_MPU_YREG_ACCESS3,	0xFFFFFFFF},
	{CS40L2X_DSP1_MPU_LOCK_CONFIG,	0x00000000}
};

static const struct reg_sequence cs40l2x_pcm_routing[] = {
	{CS40L2X_DAC_PCM1_SRC,		CS40L2X_DAC_PCM1_SRC_DSP1TX1},
	{CS40L2X_DSP1_RX1_SRC,		CS40L2X_DSP1_RXn_SRC_ASPRX1},
	{CS40L2X_DSP1_RX2_SRC,		CS40L2X_DSP1_RXn_SRC_VMON},
	{CS40L2X_DSP1_RX3_SRC,		CS40L2X_DSP1_RXn_SRC_IMON},
	{CS40L2X_DSP1_RX4_SRC,		CS40L2X_DSP1_RXn_SRC_VPMON},
};

static int cs40l2x_init(struct cs40l2x_private *cs40l2x)
{
	int ret;
	struct regmap *regmap = cs40l2x->regmap;
	struct device *dev = cs40l2x->dev;
	unsigned int wksrc_en = CS40L2X_WKSRC_EN_SDA | CS40L2X_WKSRC_EN_GPIO1;

	/* REFCLK configuration is handled by revision B1 ROM */
	if (cs40l2x->pdata.refclk_gpio2 &&
			(cs40l2x->revid < CS40L2X_REVID_B1)) {
		ret = regmap_update_bits(regmap, CS40L2X_GPIO_PAD_CONTROL,
				CS40L2X_GP2_CTRL_MASK,
				CS40L2X_GPx_CTRL_MCLK
					<< CS40L2X_GP2_CTRL_SHIFT);
		if (ret) {
			dev_err(dev, "Failed to select GPIO2 function\n");
			return ret;
		}

		ret = cs40l2x_wseq_add_reg(cs40l2x, CS40L2X_GPIO_PAD_CONTROL,
				((CS40L2X_GPx_CTRL_MCLK
					<< CS40L2X_GP2_CTRL_SHIFT)
					& CS40L2X_GP2_CTRL_MASK) |
				((CS40L2X_GPx_CTRL_GPIO
					<< CS40L2X_GP1_CTRL_SHIFT)
					& CS40L2X_GP1_CTRL_MASK));
		if (ret) {
			dev_err(dev,
				"Failed to sequence GPIO1/2 configuration\n");
			return ret;
		}

		ret = regmap_update_bits(regmap, CS40L2X_PLL_CLK_CTRL,
				CS40L2X_PLL_REFCLK_SEL_MASK,
				CS40L2X_PLL_REFCLK_SEL_MCLK
					<< CS40L2X_PLL_REFCLK_SEL_SHIFT);
		if (ret) {
			dev_err(dev, "Failed to select clock source\n");
			return ret;
		}

		ret = cs40l2x_wseq_add_reg(cs40l2x, CS40L2X_PLL_CLK_CTRL,
				((1 << CS40L2X_PLL_REFCLK_EN_SHIFT)
					& CS40L2X_PLL_REFCLK_EN_MASK) |
				((CS40L2X_PLL_REFCLK_SEL_MCLK
					<< CS40L2X_PLL_REFCLK_SEL_SHIFT)
					& CS40L2X_PLL_REFCLK_SEL_MASK));
		if (ret) {
			dev_err(dev,
				"Failed to sequence PLL configuration\n");
			return ret;
		}
	}

	ret = cs40l2x_boost_config(cs40l2x);
	if (ret)
		return ret;

	ret = regmap_multi_reg_write(regmap, cs40l2x_pcm_routing,
			ARRAY_SIZE(cs40l2x_pcm_routing));
	if (ret) {
		dev_err(dev, "Failed to configure PCM channel routing\n");
		return ret;
	}

	ret = cs40l2x_wseq_add_seq(cs40l2x, cs40l2x_pcm_routing,
			ARRAY_SIZE(cs40l2x_pcm_routing));
	if (ret) {
		dev_err(dev, "Failed to sequence PCM channel routing\n");
		return ret;
	}

	ret = cs40l2x_wseq_add_reg(cs40l2x, CS40L2X_AMP_DIG_VOL_CTRL,
			(1 << CS40L2X_AMP_HPF_PCM_EN_SHIFT)
				& CS40L2X_AMP_HPF_PCM_EN_MASK);
	if (ret) {
		dev_err(dev, "Failed to sequence amplifier volume control\n");
		return ret;
	}

	/* revisions A0 and B0 require MPU to be configured manually */
	if (cs40l2x->revid < CS40L2X_REVID_B1) {
		ret = regmap_multi_reg_write(regmap, cs40l2x_mpu_config,
				ARRAY_SIZE(cs40l2x_mpu_config));
		if (ret) {
			dev_err(dev, "Failed to configure MPU\n");
			return ret;
		}
	}

	/* hibernation is supported by revision B1 firmware only */
	if (cs40l2x->revid == CS40L2X_REVID_B1) {
		if (cs40l2x->devid == CS40L2X_DEVID_L25B)
			wksrc_en |= (CS40L2X_WKSRC_EN_GPIO2
					| CS40L2X_WKSRC_POL_GPIO4);

		ret = regmap_update_bits(regmap,
				CS40L2X_WAKESRC_CTL,
				CS40L2X_WKSRC_EN_MASK,
				wksrc_en << CS40L2X_WKSRC_EN_SHIFT);
		if (ret) {
			dev_err(dev, "Failed to enable wake sources\n");
			return ret;
		}

		ret = cs40l2x_wseq_add_reg(cs40l2x, CS40L2X_WAKESRC_CTL,
				((wksrc_en << CS40L2X_WKSRC_EN_SHIFT)
					& CS40L2X_WKSRC_EN_MASK) |
						((CS40L2X_WKSRC_POL_SDA
						<< CS40L2X_WKSRC_POL_SHIFT)
						& CS40L2X_WKSRC_POL_MASK));
		if (ret) {
			dev_err(dev, "Failed to sequence wake sources\n");
			return ret;
		}
	}

	if (cs40l2x->asp_available) {
		ret = cs40l2x_wseq_add_reg(cs40l2x, CS40L2X_PLL_CLK_CTRL,
				((1 << CS40L2X_PLL_REFCLK_EN_SHIFT)
					& CS40L2X_PLL_REFCLK_EN_MASK) |
				((CS40L2X_PLL_REFCLK_SEL_MCLK
					<< CS40L2X_PLL_REFCLK_SEL_SHIFT)
					& CS40L2X_PLL_REFCLK_SEL_MASK));
		if (ret) {
			dev_err(dev, "Failed to sequence PLL configuration\n");
			return ret;
		}

		ret = cs40l2x_asp_config(cs40l2x);
		if (ret)
			return ret;
	}

	return cs40l2x_brownout_config(cs40l2x);
}

static int cs40l2x_otp_unpack(struct cs40l2x_private *cs40l2x)
{
	struct regmap *regmap = cs40l2x->regmap;
	struct device *dev = cs40l2x->dev;
	struct cs40l2x_trim trim;
	unsigned char row_offset, col_offset;
	unsigned int val, otp_map;
	unsigned int *otp_mem;
	int ret, i;

	otp_mem = kmalloc_array(CS40L2X_NUM_OTP_WORDS,
				sizeof(*otp_mem), GFP_KERNEL);
	if (!otp_mem)
		return -ENOMEM;

	ret = regmap_read(regmap, CS40L2X_OTPID, &val);
	if (ret) {
		dev_err(dev, "Failed to read OTP ID\n");
		goto err_otp_unpack;
	}

	/* hard matching against known OTP IDs */
	for (i = 0; i < CS40L2X_NUM_OTP_MAPS; i++) {
		if (cs40l2x_otp_map[i].id == val) {
			otp_map = i;
			break;
		}
	}

	/* reject unrecognized IDs, including untrimmed devices (OTP ID = 0) */
	if (i == CS40L2X_NUM_OTP_MAPS) {
		dev_err(dev, "Unrecognized OTP ID: 0x%01X\n", val);
		ret = -ENODEV;
		goto err_otp_unpack;
	}

	dev_dbg(dev, "Found OTP ID: 0x%01X\n", val);

	ret = regmap_bulk_read(regmap, CS40L2X_OTP_MEM0, otp_mem,
			CS40L2X_NUM_OTP_WORDS);
	if (ret) {
		dev_err(dev, "Failed to read OTP contents\n");
		goto err_otp_unpack;
	}

	ret = regmap_write(regmap, CS40L2X_TEST_KEY_CTL,
			CS40L2X_TEST_KEY_UNLOCK_CODE1);
	if (ret) {
		dev_err(dev, "Failed to unlock test space (step 1 of 2)\n");
		goto err_otp_unpack;
	}

	ret = regmap_write(regmap, CS40L2X_TEST_KEY_CTL,
			CS40L2X_TEST_KEY_UNLOCK_CODE2);
	if (ret) {
		dev_err(dev, "Failed to unlock test space (step 2 of 2)\n");
		goto err_otp_unpack;
	}

	row_offset = cs40l2x_otp_map[otp_map].row_start;
	col_offset = cs40l2x_otp_map[otp_map].col_start;

	for (i = 0; i < cs40l2x_otp_map[otp_map].num_trims; i++) {
		trim = cs40l2x_otp_map[otp_map].trim_table[i];

		if (col_offset + trim.size - 1 > 31) {
			/* trim straddles word boundary */
			val = (otp_mem[row_offset] &
					GENMASK(31, col_offset)) >> col_offset;
			val |= (otp_mem[row_offset + 1] &
					GENMASK(col_offset + trim.size - 33, 0))
					<< (32 - col_offset);
		} else {
			/* trim does not straddle word boundary */
			val = (otp_mem[row_offset] &
					GENMASK(col_offset + trim.size - 1,
						col_offset)) >> col_offset;
		}

		/* advance column marker and wrap if necessary */
		col_offset += trim.size;
		if (col_offset > 31) {
			col_offset -= 32;
			row_offset++;
		}

		/* skip blank trims */
		if (trim.reg == 0)
			continue;

		ret = regmap_update_bits(regmap, trim.reg,
				GENMASK(trim.shift + trim.size - 1, trim.shift),
				val << trim.shift);
		if (ret) {
			dev_err(dev, "Failed to write trim %d\n", i + 1);
			goto err_otp_unpack;
		}

		dev_dbg(dev, "Trim %d: wrote 0x%X to 0x%08X bits [%d:%d]\n",
				i + 1, val, trim.reg,
				trim.shift + trim.size - 1, trim.shift);
	}

	ret = regmap_write(regmap, CS40L2X_TEST_KEY_CTL,
			CS40L2X_TEST_KEY_RELOCK_CODE1);
	if (ret) {
		dev_err(dev, "Failed to lock test space (step 1 of 2)\n");
		goto err_otp_unpack;
	}

	ret = regmap_write(regmap, CS40L2X_TEST_KEY_CTL,
			CS40L2X_TEST_KEY_RELOCK_CODE2);
	if (ret) {
		dev_err(dev, "Failed to lock test space (step 2 of 2)\n");
		goto err_otp_unpack;
	}

	ret = 0;

err_otp_unpack:
	kfree(otp_mem);

	return ret;
}

static int cs40l2x_handle_of_data(struct i2c_client *i2c_client,
		struct cs40l2x_platform_data *pdata)
{
	struct device_node *np = i2c_client->dev.of_node;
	struct device *dev = &i2c_client->dev;
	int ret;
	unsigned int out_val;
#ifdef CONFIG_CS40L2X_SAMSUNG_FEATURE
	const char *type;
#endif

	if (!np)
		return 0;

	ret = of_property_read_u32(np, "cirrus,boost-ind-nanohenry", &out_val);
	if (ret) {
		dev_err(dev, "Boost inductor value not specified\n");
		return -EINVAL;
	}
	pdata->boost_ind = out_val;

	ret = of_property_read_u32(np, "cirrus,boost-cap-microfarad", &out_val);
	if (ret) {
		dev_err(dev, "Boost capacitance not specified\n");
		return -EINVAL;
	}
	pdata->boost_cap = out_val;

	ret = of_property_read_u32(np, "cirrus,boost-ipk-milliamp", &out_val);
	if (ret) {
		dev_err(dev, "Boost inductor peak current not specified\n");
		return -EINVAL;
	}
	pdata->boost_ipk = out_val;

	ret = of_property_read_u32(np, "cirrus,boost-ctl-millivolt", &out_val);
	if (!ret)
		pdata->boost_ctl = out_val | CS40L2X_PDATA_PRESENT;

	ret = of_property_read_u32(np, "cirrus,boost-ovp-millivolt", &out_val);
	if (!ret)
		pdata->boost_ovp = out_val;

	pdata->refclk_gpio2 = of_property_read_bool(np, "cirrus,refclk-gpio2");

	ret = of_property_read_u32(np, "cirrus,f0-default", &out_val);
	if (!ret)
		pdata->f0_default = out_val;

	ret = of_property_read_u32(np, "cirrus,f0-min", &out_val);
	if (!ret)
		pdata->f0_min = out_val;

	ret = of_property_read_u32(np, "cirrus,f0-max", &out_val);
	if (!ret)
		pdata->f0_max = out_val;

	ret = of_property_read_u32(np, "cirrus,redc-default", &out_val);
	if (!ret)
		pdata->redc_default = out_val;

	ret = of_property_read_u32(np, "cirrus,redc-min", &out_val);
	if (!ret)
		pdata->redc_min = out_val;

	ret = of_property_read_u32(np, "cirrus,redc-max", &out_val);
	if (!ret)
		pdata->redc_max = out_val;

	ret = of_property_read_u32(np, "cirrus,q-default", &out_val);
	if (!ret)
		pdata->q_default = out_val;

	ret = of_property_read_u32(np, "cirrus,q-min", &out_val);
	if (!ret)
		pdata->q_min = out_val;

	ret = of_property_read_u32(np, "cirrus,q-max", &out_val);
	if (!ret)
		pdata->q_max = out_val;

	pdata->redc_comp_disable = of_property_read_bool(np,
			"cirrus,redc-comp-disable");

	pdata->comp_disable = of_property_read_bool(np, "cirrus,comp-disable");

	ret = of_property_read_u32(np, "cirrus,gpio1-rise-index", &out_val);
	if (!ret)
		pdata->gpio1_rise_index = out_val;

	ret = of_property_read_u32(np, "cirrus,gpio1-fall-index", &out_val);
	if (!ret)
		pdata->gpio1_fall_index = out_val;

	ret = of_property_read_u32(np, "cirrus,gpio1-fall-timeout", &out_val);
	if (!ret)
		pdata->gpio1_fall_timeout = out_val | CS40L2X_PDATA_PRESENT;

	ret = of_property_read_u32(np, "cirrus,gpio1-mode", &out_val);
	if (!ret) {
		if (out_val > CS40L2X_GPIO1_MODE_MAX)
			dev_warn(dev, "Ignored default gpio1_mode\n");
		else
			pdata->gpio1_mode = out_val;
	}

	ret = of_property_read_u32(np, "cirrus,gpio2-rise-index", &out_val);
	if (!ret)
		pdata->gpio2_rise_index = out_val;

	ret = of_property_read_u32(np, "cirrus,gpio2-fall-index", &out_val);
	if (!ret)
		pdata->gpio2_fall_index = out_val;

	ret = of_property_read_u32(np, "cirrus,gpio3-rise-index", &out_val);
	if (!ret)
		pdata->gpio3_rise_index = out_val;

	ret = of_property_read_u32(np, "cirrus,gpio3-fall-index", &out_val);
	if (!ret)
		pdata->gpio3_fall_index = out_val;

	ret = of_property_read_u32(np, "cirrus,gpio4-rise-index", &out_val);
	if (!ret)
		pdata->gpio4_rise_index = out_val;

	ret = of_property_read_u32(np, "cirrus,gpio4-fall-index", &out_val);
	if (!ret)
		pdata->gpio4_fall_index = out_val;

	pdata->hiber_enable = of_property_read_bool(np, "cirrus,hiber-enable");

	ret = of_property_read_u32(np, "cirrus,asp-bclk-freq-hz", &out_val);
	if (!ret)
		pdata->asp_bclk_freq = out_val;

	pdata->asp_bclk_inv = of_property_read_bool(np, "cirrus,asp-bclk-inv");

	pdata->asp_fsync_inv = of_property_read_bool(np,
			"cirrus,asp-fsync-inv");

	ret = of_property_read_u32(np, "cirrus,asp-fmt", &out_val);
	if (!ret)
		pdata->asp_fmt = out_val | CS40L2X_PDATA_PRESENT;

	ret = of_property_read_u32(np, "cirrus,asp-slot-num", &out_val);
	if (!ret)
		pdata->asp_slot_num = out_val;

	ret = of_property_read_u32(np, "cirrus,asp-slot-width", &out_val);
	if (!ret)
		pdata->asp_slot_width = out_val;

	ret = of_property_read_u32(np, "cirrus,asp-samp-width", &out_val);
	if (!ret)
		pdata->asp_samp_width = out_val;

	ret = of_property_read_u32(np, "cirrus,asp-timeout", &out_val);
	if (!ret) {
		if (out_val > CS40L2X_ASP_TIMEOUT_MAX)
			dev_warn(dev, "Ignored default ASP timeout\n");
		else
			pdata->asp_timeout = out_val;
	}

	pdata->vpbr_enable = of_property_read_bool(np, "cirrus,vpbr-enable");

	pdata->vbbr_enable = of_property_read_bool(np, "cirrus,vbbr-enable");

	ret = of_property_read_u32(np, "cirrus,vpbr-thld1-millivolt", &out_val);
	if (!ret)
		pdata->vpbr_thld1 = out_val;

	ret = of_property_read_u32(np, "cirrus,vbbr-thld1-millivolt", &out_val);
	if (!ret)
		pdata->vbbr_thld1 = out_val;
#ifdef CONFIG_CS40L2X_SAMSUNG_FEATURE
	ret = of_property_read_string(np, "samsung,vib_type", &type);
	if (ret)
		pr_info("%s: motor type not specified\n", __func__);
	else
		snprintf(pdata->vib_type, sizeof(pdata->vib_type), "%s", type);
#endif
	return 0;
}

static const struct reg_sequence cs40l2x_basic_mode_revert[] = {
	{CS40L2X_PWR_CTRL1,		0x00000000},
	{CS40L2X_PWR_CTRL2,		0x00003321},
	{CS40L2X_LRCK_PAD_CONTROL,	0x00000007},
	{CS40L2X_SDIN_PAD_CONTROL,	0x00000007},
	{CS40L2X_GPIO_PAD_CONTROL,	0x00000000},
	{CS40L2X_AMP_DIG_VOL_CTRL,	0x00008000},
	{CS40L2X_IRQ2_MASK1,		0xFFFFFFFF},
	{CS40L2X_IRQ2_MASK2,		0xFFFFFFFF},
};

static int cs40l2x_basic_mode_exit(struct cs40l2x_private *cs40l2x)
{
	struct regmap *regmap = cs40l2x->regmap;
	struct device *dev = cs40l2x->dev;
	unsigned int val;
	int ret, i;

	for (i = 0; i < CS40L2X_BASIC_TIMEOUT_COUNT; i++) {
		ret = regmap_read(regmap, CS40L2X_BASIC_AMP_STATUS, &val);
		if (ret) {
			dev_err(dev, "Failed to read basic-mode boot status\n");
			return ret;
		}

		if (val & CS40L2X_BASIC_BOOT_DONE)
			break;

		usleep_range(10000, 10100);
	}

	if (i == CS40L2X_BASIC_TIMEOUT_COUNT) {
		dev_err(dev, "Timed out waiting for basic-mode boot\n");
		return -ETIME;
	}

	ret = cs40l2x_ack_write(cs40l2x, CS40L2X_BASIC_SHUTDOWNREQUEST, 1);
	if (ret) {
		dev_err(dev, "Failed to administer shutdown request\n");
		return ret;
	}

	ret = regmap_read(regmap, CS40L2X_BASIC_STATEMACHINE, &val);
	if (ret) {
		dev_err(dev, "Failed to read basic-mode state\n");
		return ret;
	}

	if (val != CS40L2X_BASIC_SHUTDOWN) {
		dev_err(dev, "Unexpected basic-mode state: 0x%02X\n", val);
		return -EBUSY;
	}

	ret = regmap_read(regmap, CS40L2X_BASIC_AMP_STATUS, &val);
	if (ret) {
		dev_err(dev, "Failed to read basic-mode error status\n");
		return ret;
	}

	if (val & CS40L2X_BASIC_OTP_ERROR) {
		dev_err(dev, "Encountered basic-mode OTP error\n");
		return -EIO;
	}

	if (val & CS40L2X_BASIC_AMP_ERROR) {
		ret = cs40l2x_hw_err_rls(cs40l2x, CS40L2X_AMP_ERR);
		if (ret)
			return ret;
	}

	if (val & CS40L2X_BASIC_TEMP_RISE_WARN) {
		ret = cs40l2x_hw_err_rls(cs40l2x, CS40L2X_TEMP_RISE_WARN);
		if (ret)
			return ret;
	}

	if (val & CS40L2X_BASIC_TEMP_ERROR) {
		ret = cs40l2x_hw_err_rls(cs40l2x, CS40L2X_TEMP_ERR);
		if (ret)
			return ret;
	}

	ret = regmap_multi_reg_write(regmap, cs40l2x_basic_mode_revert,
			ARRAY_SIZE(cs40l2x_basic_mode_revert));
	if (ret) {
		dev_err(dev, "Failed to revert basic-mode fields\n");
		return ret;
	}

	return 0;
}

static const struct reg_sequence cs40l2x_rev_a0_errata[] = {
	{CS40L2X_OTP_TRIM_30,		0x9091A1C8},
	{CS40L2X_PLL_LOOP_PARAM,	0x000C1837},
	{CS40L2X_PLL_MISC_CTRL,		0x03008E0E},
	{CS40L2X_BSTCVRT_DCM_CTRL,	0x00000051},
	{CS40L2X_CTRL_ASYNC1,		0x00000004},
	{CS40L2X_IRQ1_DB3,		0x00000000},
	{CS40L2X_IRQ2_DB3,		0x00000000},
	{CS40L2X_TEST_KEY_CTL,		CS40L2X_TEST_KEY_UNLOCK_CODE1},
	{CS40L2X_TEST_KEY_CTL,		CS40L2X_TEST_KEY_UNLOCK_CODE2},
	{CS40L2X_SPKMON_RESYNC,		0x00000000},
	{CS40L2X_TEMP_RESYNC,		0x00000000},
	{CS40L2X_TEST_KEY_CTL,		CS40L2X_TEST_KEY_RELOCK_CODE1},
	{CS40L2X_TEST_KEY_CTL,		CS40L2X_TEST_KEY_RELOCK_CODE2},
	{CS40L2X_VPVBST_FS_SEL,		0x00000000},
	{CS40L2X_ASP_CONTROL4,		0x01010000},
};

static const struct reg_sequence cs40l2x_rev_b0_errata[] = {
	{CS40L2X_PLL_LOOP_PARAM,	0x000C1837},
	{CS40L2X_PLL_MISC_CTRL,		0x03008E0E},
	{CS40L2X_TEST_KEY_CTL,		CS40L2X_TEST_KEY_UNLOCK_CODE1},
	{CS40L2X_TEST_KEY_CTL,		CS40L2X_TEST_KEY_UNLOCK_CODE2},
	{CS40L2X_OTP_TRIM_12,		0x002F0065},
	{CS40L2X_OTP_TRIM_13,		0x00002B4F},
	{CS40L2X_SPKMON_RESYNC,		0x00000000},
	{CS40L2X_TEMP_RESYNC,		0x00000000},
	{CS40L2X_TEST_KEY_CTL,		CS40L2X_TEST_KEY_RELOCK_CODE1},
	{CS40L2X_TEST_KEY_CTL,		CS40L2X_TEST_KEY_RELOCK_CODE2},
	{CS40L2X_VPVBST_FS_SEL,		0x00000000},
	{CS40L2X_ASP_CONTROL4,		0x01010000},
};

static int cs40l2x_part_num_resolve(struct cs40l2x_private *cs40l2x)
{
	struct regmap *regmap = cs40l2x->regmap;
	struct device *dev = cs40l2x->dev;
	unsigned int val, devid, revid;
	unsigned int part_num_index, fw_id;
	int otp_timeout = CS40L2X_OTP_TIMEOUT_COUNT;
	int ret;

	while (otp_timeout > 0) {
		usleep_range(10000, 10100);

		ret = regmap_read(regmap, CS40L2X_IRQ1_STATUS4, &val);
		if (ret) {
			dev_err(dev, "Failed to read OTP boot status\n");
			return ret;
		}

		if (val & CS40L2X_OTP_BOOT_DONE)
			break;

		otp_timeout--;
	}

	if (otp_timeout == 0) {
		dev_err(dev, "Timed out waiting for OTP boot\n");
		return -ETIME;
	}

	ret = regmap_read(regmap, CS40L2X_IRQ1_STATUS3, &val);
	if (ret) {
		dev_err(dev, "Failed to read OTP error status\n");
		return ret;
	}

	if (val & CS40L2X_OTP_BOOT_ERR) {
		dev_err(dev, "Encountered fatal OTP error\n");
		return -EIO;
	}

	ret = regmap_read(regmap, CS40L2X_DEVID, &devid);
	if (ret) {
		dev_err(dev, "Failed to read device ID\n");
		return ret;
	}

	ret = regmap_read(regmap, CS40L2X_REVID, &revid);
	if (ret) {
		dev_err(dev, "Failed to read revision ID\n");
		return ret;
	}

	switch (devid) {
	case CS40L2X_DEVID_L20:
		part_num_index = 0;
		fw_id = CS40L2X_FW_ID_ORIG;

		if (revid != CS40L2X_REVID_A0)
			goto err_revid;

		ret = regmap_register_patch(regmap, cs40l2x_rev_a0_errata,
				ARRAY_SIZE(cs40l2x_rev_a0_errata));
		if (ret) {
			dev_err(dev, "Failed to apply revision %02X errata\n",
					revid);
			return ret;
		}

		ret = cs40l2x_otp_unpack(cs40l2x);
		if (ret)
			return ret;
		break;
	case CS40L2X_DEVID_L25:
		part_num_index = 1;
		fw_id = CS40L2X_FW_ID_ORIG;

		if (revid != CS40L2X_REVID_B0)
			goto err_revid;

		ret = regmap_register_patch(regmap, cs40l2x_rev_b0_errata,
				ARRAY_SIZE(cs40l2x_rev_b0_errata));
		if (ret) {
			dev_err(dev, "Failed to apply revision %02X errata\n",
					revid);
			return ret;
		}

		ret = cs40l2x_wseq_add_seq(cs40l2x, cs40l2x_rev_b0_errata,
				ARRAY_SIZE(cs40l2x_rev_b0_errata));
		if (ret) {
			dev_err(dev,
				"Failed to sequence revision %02X errata\n",
				revid);
			return ret;
		}
		break;
	case CS40L2X_DEVID_L25A:
	case CS40L2X_DEVID_L25B:
		part_num_index = devid - CS40L2X_DEVID_L25A + 2;
		fw_id = CS40L2X_FW_ID_REMAP;

		if (revid < CS40L2X_REVID_B1)
			goto err_revid;

		ret = cs40l2x_basic_mode_exit(cs40l2x);
		if (ret)
			return ret;

		ret = regmap_register_patch(regmap, cs40l2x_rev_b0_errata,
				ARRAY_SIZE(cs40l2x_rev_b0_errata));
		if (ret) {
			dev_err(dev, "Failed to apply revision %02X errata\n",
					revid);
			return ret;
		}

		ret = cs40l2x_wseq_add_seq(cs40l2x, cs40l2x_rev_b0_errata,
				ARRAY_SIZE(cs40l2x_rev_b0_errata));
		if (ret) {
			dev_err(dev,
				"Failed to sequence revision %02X errata\n",
				revid);
			return ret;
		}
		break;
	default:
		dev_err(dev, "Unrecognized device ID: 0x%06X\n", devid);
		return -ENODEV;
	}

	cs40l2x->fw_desc = cs40l2x_firmware_match(cs40l2x, fw_id);
	if (!cs40l2x->fw_desc)
		return -EINVAL;

	dev_info(dev, "Cirrus Logic %s revision %02X\n",
			cs40l2x_part_nums[part_num_index], revid);
	cs40l2x->devid = devid;
	cs40l2x->revid = revid;

	return 0;
err_revid:
	dev_err(dev, "Unexpected revision ID for %s: %02X\n",
			cs40l2x_part_nums[part_num_index], revid);
	return -ENODEV;
}

static irqreturn_t cs40l2x_irq(int irq, void *data)
{
	struct cs40l2x_private *cs40l2x = (struct cs40l2x_private *)data;
	struct regmap *regmap = cs40l2x->regmap;
	struct device *dev = cs40l2x->dev;
	unsigned int asp_timeout = cs40l2x->pdata.asp_timeout;
	unsigned int val;
	int event_count = 0;
	int ret, i;
	irqreturn_t ret_irq = IRQ_NONE;

	if (!cs40l2x->vibe_init_success)
		return ret_irq;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->fw_desc->id != CS40L2X_FW_ID_REMAP)
		goto err_mutex;

	for (i = 0; i < ARRAY_SIZE(cs40l2x_event_regs); i++) {
		/* skip disabled event notifiers */
		if (!(cs40l2x->event_control & cs40l2x_event_masks[i]))
			continue;

		ret = regmap_read(regmap,
				cs40l2x_dsp_reg(cs40l2x,
						cs40l2x_event_regs[i],
						CS40L2X_XM_UNPACKED_TYPE),
				&val);
		if (ret) {
			dev_err(dev, "Failed to read %s\n",
					cs40l2x_event_regs[i]);
			goto err_mutex;
		}

		/* any event handling goes here */
		switch (val) {
		case CS40L2X_EVENT_CTRL_NONE:
			continue;
		case CS40L2X_EVENT_CTRL_HARDWARE:
			ret = cs40l2x_hw_err_chk(cs40l2x);
			if (ret)
				goto err_mutex;
			break;
		case CS40L2X_EVENT_CTRL_TRIG_STOP:
			queue_work(cs40l2x->vibe_workqueue,
					&cs40l2x->vibe_pbq_work);
			/* intentionally fall through */
		case CS40L2X_EVENT_CTRL_GPIO_STOP:
			if (asp_timeout > 0)
				hrtimer_start(&cs40l2x->asp_timer,
						ktime_set(asp_timeout / 1000,
							(asp_timeout % 1000)
								* 1000000),
						HRTIMER_MODE_REL);
			else
				queue_work(cs40l2x->vibe_workqueue,
						&cs40l2x->vibe_mode_work);
			/* intentionally fall through */
		case CS40L2X_EVENT_CTRL_GPIO1_FALL
			... CS40L2X_EVENT_CTRL_GPIO_START:
		case CS40L2X_EVENT_CTRL_READY:
		case CS40L2X_EVENT_CTRL_TRIG_SUSP
			... CS40L2X_EVENT_CTRL_TRIG_RESM:
			dev_dbg(dev, "Found notifier %d in %s\n",
					val, cs40l2x_event_regs[i]);
			break;
		default:
			dev_err(dev, "Unrecognized notifier %d in %s\n",
					val, cs40l2x_event_regs[i]);
			goto err_mutex;
		}

		ret = regmap_write(regmap,
				cs40l2x_dsp_reg(cs40l2x,
						cs40l2x_event_regs[i],
						CS40L2X_XM_UNPACKED_TYPE),
				CS40L2X_EVENT_CTRL_NONE);
		if (ret) {
			dev_err(dev, "Failed to acknowledge %s\n",
					cs40l2x_event_regs[i]);
			goto err_mutex;
		}

		ret = regmap_write(regmap, CS40L2X_MBOX_POWERCONTROL,
				CS40L2X_POWERCONTROL_WAKEUP);
		if (ret) {
			dev_err(dev, "Failed to free /ALERT output\n");
			goto err_mutex;
		}

		event_count++;
	}

	if (event_count > 0)
		ret_irq = IRQ_HANDLED;

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret_irq;
}

static struct regmap_config cs40l2x_regmap = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_BIG,
	.max_register = CS40L2X_LASTREG,
	.precious_reg = cs40l2x_precious_reg,
	.readable_reg = cs40l2x_readable_reg,
	.cache_type = REGCACHE_NONE,
};

static int cs40l2x_i2c_probe(struct i2c_client *i2c_client,
				const struct i2c_device_id *id)
{
	int ret, i;
	struct cs40l2x_private *cs40l2x;
	struct device *dev = &i2c_client->dev;
	struct cs40l2x_platform_data *pdata = dev_get_platdata(dev);

#ifdef CONFIG_CS40L2X_SAMSUNG_FEATURE
	pr_info("%s\n", __func__ );
#endif
	cs40l2x = devm_kzalloc(dev, sizeof(struct cs40l2x_private), GFP_KERNEL);
	if (!cs40l2x)
		return -ENOMEM;

	cs40l2x->dev = dev;
	dev_set_drvdata(dev, cs40l2x);
	i2c_set_clientdata(i2c_client, cs40l2x);

	mutex_init(&cs40l2x->lock);

	INIT_LIST_HEAD(&cs40l2x->coeff_desc_head);

	cs40l2x->regmap = devm_regmap_init_i2c(i2c_client, &cs40l2x_regmap);
	if (IS_ERR(cs40l2x->regmap)) {
		ret = PTR_ERR(cs40l2x->regmap);
		dev_err(dev, "Failed to allocate register map: %d\n", ret);
		return ret;
	}

	for (i = 0; i < ARRAY_SIZE(cs40l2x_supplies); i++)
		cs40l2x->supplies[i].supply = cs40l2x_supplies[i];

	cs40l2x->num_supplies = ARRAY_SIZE(cs40l2x_supplies);

	ret = devm_regulator_bulk_get(dev, cs40l2x->num_supplies,
			cs40l2x->supplies);
	if (ret) {
		dev_err(dev, "Failed to request core supplies: %d\n", ret);
		return ret;
	}

	if (pdata) {
		cs40l2x->pdata = *pdata;
	} else {
		pdata = devm_kzalloc(dev, sizeof(struct cs40l2x_platform_data),
				GFP_KERNEL);
		if (!pdata)
			return -ENOMEM;

		if (i2c_client->dev.of_node) {
			ret = cs40l2x_handle_of_data(i2c_client, pdata);
			if (ret)
				return ret;

		}
		cs40l2x->pdata = *pdata;
	}

	cs40l2x->cp_trailer_index = CS40L2X_INDEX_IDLE;

	cs40l2x->vpp_measured = -1;
	cs40l2x->ipp_measured = -1;

	cs40l2x->comp_enable = !pdata->comp_disable;
	cs40l2x->comp_enable_redc = !pdata->redc_comp_disable;
	cs40l2x->comp_enable_f0 = true;

	for (i = 0; i < CS40L2X_NUM_HW_ERRS; i++)
		cs40l2x->hw_err_mask |= cs40l2x_hw_errs[i].irq_mask;

	ret = regulator_bulk_enable(cs40l2x->num_supplies, cs40l2x->supplies);
	if (ret) {
		dev_err(dev, "Failed to enable core supplies: %d\n", ret);
		return ret;
	}

	cs40l2x->reset_gpio = devm_gpiod_get_optional(dev, "reset",
			GPIOD_OUT_LOW);
	if (IS_ERR(cs40l2x->reset_gpio)) {
#ifndef CONFIG_MOTOR_DRV_CS40L2X_PMIC_RESET
		ret = PTR_ERR(cs40l2x->reset_gpio);
		pr_err("Failed to get reset gpio: %d\n", ret);
		return ret;
#else
		pr_err("%s: DT has no reset gpio. Will try to get pmic vldo\n", __func__);
		cs40l2x->reset_gpio = NULL;
#endif
	}

#ifdef CONFIG_MOTOR_DRV_CS40L2X_PMIC_RESET
	if (cs40l2x->reset_gpio == NULL) {
		cs40l2x->reset_vldo = devm_regulator_get(dev, "samsung,reset-vldo");
		if (IS_ERR(cs40l2x->reset_vldo)) {
			pr_err("can't request VLDO power supply: %ld\n", __func__, 
				PTR_ERR(cs40l2x->reset_vldo));
			cs40l2x->reset_vldo = NULL;
			return ret;
		}
		cs40l2x_reset_control(cs40l2x, 0);
	}
#endif
	/* satisfy reset pulse width specification (with margin) */
	usleep_range(2000, 2100);
#ifndef CONFIG_MOTOR_DRV_CS40L2X_PMIC_RESET
	gpiod_set_value_cansleep(cs40l2x->reset_gpio, 1);
#else
	cs40l2x_reset_control(cs40l2x, 1);
#endif
	/* satisfy control port delay specification (with margin) */
	usleep_range(1000, 1100);

	ret = cs40l2x_part_num_resolve(cs40l2x);
	if (ret)
		goto err1;

	cs40l2x->asp_available = (cs40l2x->devid == CS40L2X_DEVID_L25A)
			&& pdata->asp_bclk_freq
			&& pdata->asp_slot_width
			&& pdata->asp_samp_width;

	if (cs40l2x->fw_desc->id == CS40L2X_FW_ID_REMAP && i2c_client->irq) {
		ret = devm_request_threaded_irq(dev, i2c_client->irq,
				NULL, cs40l2x_irq,
				IRQF_ONESHOT | IRQF_TRIGGER_LOW,
				i2c_client->name, cs40l2x);
		if (ret) {
			dev_err(dev, "Failed to request IRQ: %d\n", ret);
			goto err1;
		}

		cs40l2x->event_control = CS40L2X_EVENT_HARDWARE_ENABLED
				| CS40L2X_EVENT_END_ENABLED;
	} else {
		cs40l2x->event_control = CS40L2X_EVENT_DISABLED;
	}
#ifdef CONFIG_CS40L2X_SAMSUNG_FEATURE
	cs40l2x_dev_node_init(cs40l2x);
#endif
	ret = cs40l2x_init(cs40l2x);
	if (ret)
		goto err2;

	request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG,
			cs40l2x->fw_desc->fw_file, dev, GFP_KERNEL, cs40l2x,
			cs40l2x_firmware_load);

#ifdef CONFIG_FOLDER_HALL
	INIT_DELAYED_WORK(&cs40l2x->switch_work, cs40l2x_switch_work);

	/* Hall IC notify priority -> ftn -> register */
	cs40l2x->flip_status = -1;
	cs40l2x->flip_status_current = VIB_STATUS_UNFOLDING;	// default : unfolding (open, 0)
	cs40l2x->hall_ic_nb.priority = 1;
	cs40l2x->hall_ic_nb.notifier_call = cs40l2x_hall_ic_notify;
	hall_ic_register_notify(&cs40l2x->hall_ic_nb);
	
	pr_info("%s: registed hall ic notify\n", __func__);
#endif

	return 0;

err2:
	pr_info("%s err2\n", __func__ );
#ifdef CONFIG_CS40L2X_SAMSUNG_FEATURE
	cs40l2x_dev_node_remove(cs40l2x);
#endif
err1:
	pr_info("%s err1\n", __func__ );
#ifndef CONFIG_MOTOR_DRV_CS40L2X_PMIC_RESET
	gpiod_set_value_cansleep(cs40l2x->reset_gpio, 0);
#else
	cs40l2x_reset_control(cs40l2x, 0);
#endif
	regulator_bulk_disable(cs40l2x->num_supplies, cs40l2x->supplies);

	return ret;
}

static int cs40l2x_i2c_remove(struct i2c_client *i2c_client)
{
	struct cs40l2x_private *cs40l2x = i2c_get_clientdata(i2c_client);

	/* manually free irq ahead of destroying workqueue */
	if (cs40l2x->event_control != CS40L2X_EVENT_DISABLED)
		devm_free_irq(&i2c_client->dev, i2c_client->irq, cs40l2x);

	if (cs40l2x->vibe_init_success) {
#ifdef CONFIG_ANDROID_TIMED_OUTPUT
		hrtimer_cancel(&cs40l2x->vibe_timer);
#ifndef CONFIG_CS40L2X_SAMSUNG_FEATURE
		timed_output_dev_unregister(&cs40l2x->timed_dev);
#endif
		sysfs_remove_group(&cs40l2x->timed_dev.dev->kobj,
				&cs40l2x_dev_attr_group);
#ifdef CONFIG_CS40L2X_SAMSUNG_FEATURE
		timed_output_dev_unregister(&cs40l2x->timed_dev);
#endif
#else
#ifndef CONFIG_CS40L2X_SAMSUNG_FEATURE
		led_classdev_unregister(&cs40l2x->led_dev);
#endif
		sysfs_remove_group(&cs40l2x->dev->kobj,
				&cs40l2x_dev_attr_group);
#ifdef CONFIG_CS40L2X_SAMSUNG_FEATURE
		led_classdev_unregister(&cs40l2x->led_dev);
#endif
#endif /* CONFIG_ANDROID_TIMED_OUTPUT */

		hrtimer_cancel(&cs40l2x->pbq_timer);
		hrtimer_cancel(&cs40l2x->asp_timer);

		cancel_work_sync(&cs40l2x->vibe_start_work);
		cancel_work_sync(&cs40l2x->vibe_pbq_work);
		cancel_work_sync(&cs40l2x->vibe_stop_work);
		cancel_work_sync(&cs40l2x->vibe_mode_work);

		destroy_workqueue(cs40l2x->vibe_workqueue);

		device_init_wakeup(cs40l2x->dev, false);
	}

#ifndef CONFIG_MOTOR_DRV_CS40L2X_PMIC_RESET
	gpiod_set_value_cansleep(cs40l2x->reset_gpio, 0);
#else
	cs40l2x_reset_control(cs40l2x, 0);
#endif

	regulator_bulk_disable(cs40l2x->num_supplies, cs40l2x->supplies);

	cs40l2x_coeff_free(cs40l2x);

	mutex_destroy(&cs40l2x->lock);

	return 0;
}

static int __maybe_unused cs40l2x_suspend(struct device *dev)
{
	struct cs40l2x_private *cs40l2x = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->pdata.gpio1_mode == CS40L2X_GPIO1_MODE_AUTO
			&& cs40l2x->fw_desc->id != CS40L2X_FW_ID_CAL) {
		ret = regmap_write(cs40l2x->regmap,
				cs40l2x_dsp_reg(cs40l2x, "GPIO_ENABLE",
						CS40L2X_XM_UNPACKED_TYPE),
				CS40L2X_GPIO1_ENABLED);
		if (ret) {
			dev_err(dev, "Failed to enable GPIO1 upon suspend\n");
			goto err_mutex;
		}
	}

	if (cs40l2x->pdata.hiber_enable
			&& cs40l2x->fw_desc->id == CS40L2X_FW_ID_REMAP) {
		ret = cs40l2x_hiber_cmd_send(cs40l2x,
				CS40L2X_POWERCONTROL_HIBERNATE);
		if (ret)
			dev_err(dev, "Failed to hibernate upon suspend\n");
	}

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static int __maybe_unused cs40l2x_resume(struct device *dev)
{
	struct cs40l2x_private *cs40l2x = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&cs40l2x->lock);

	if (cs40l2x->pdata.gpio1_mode == CS40L2X_GPIO1_MODE_AUTO
			&& cs40l2x->fw_desc->id != CS40L2X_FW_ID_CAL) {
		ret = regmap_write(cs40l2x->regmap,
				cs40l2x_dsp_reg(cs40l2x, "GPIO_ENABLE",
						CS40L2X_XM_UNPACKED_TYPE),
				CS40L2X_GPIO1_DISABLED);
		if (ret) {
			dev_err(dev, "Failed to disable GPIO1 upon resume\n");
			goto err_mutex;
		}
	}

	if (cs40l2x->pdata.hiber_enable
			&& cs40l2x->fw_desc->id == CS40L2X_FW_ID_REMAP) {
		ret = cs40l2x_hiber_cmd_send(cs40l2x,
				CS40L2X_POWERCONTROL_WAKEUP);
		if (ret)
			dev_err(dev, "Failed to wake up upon resume\n");
	}

err_mutex:
	mutex_unlock(&cs40l2x->lock);

	return ret;
}

static SIMPLE_DEV_PM_OPS(cs40l2x_pm_ops, cs40l2x_suspend, cs40l2x_resume);

static const struct of_device_id cs40l2x_of_match[] = {
	{ .compatible = "cirrus,cs40l20" },
	{ .compatible = "cirrus,cs40l25" },
	{ .compatible = "cirrus,cs40l25a" },
	{ .compatible = "cirrus,cs40l25b" },
	{ }
};

MODULE_DEVICE_TABLE(of, cs40l2x_of_match);

static const struct i2c_device_id cs40l2x_id[] = {
	{ "cs40l20", 0 },
	{ "cs40l25", 1 },
	{ "cs40l25a", 2 },
	{ "cs40l25b", 3 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, cs40l2x_id);

static struct i2c_driver cs40l2x_i2c_driver = {
	.driver = {
		.name = "cs40l2x",
		.of_match_table = cs40l2x_of_match,
		.pm = &cs40l2x_pm_ops,
	},
	.id_table = cs40l2x_id,
	.probe = cs40l2x_i2c_probe,
	.remove = cs40l2x_i2c_remove,
};

module_i2c_driver(cs40l2x_i2c_driver);

MODULE_DESCRIPTION("CS40L20/CS40L25/CS40L25A/CS40L25B Haptics Driver");
MODULE_AUTHOR("Jeff LaBundy, Cirrus Logic Inc, <jeff.labundy@cirrus.com>");
MODULE_LICENSE("GPL");
