/*
 * Samsung SoC DP (Display Port) interface driver.
 *
 * Copyright (C) 2012 Samsung Electronics Co., Ltd.
 * Author: Jingoo Han <jg1.han@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/pm_runtime.h>

#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif

#include <video/s5p-dp.h>

#include <plat/cpu.h>

#include "s5p-dp-core.h"

static int s5p_dp_init_dp(struct s5p_dp_device *dp)
{
	s5p_dp_reset(dp);

	/* SW defined function Normal operation */
	s5p_dp_enable_sw_function(dp);

	if (!soc_is_exynos5250())
		s5p_dp_config_interrupt(dp);

	s5p_dp_init_analog_func(dp);

	s5p_dp_init_hpd(dp);
	s5p_dp_init_aux(dp);

	return 0;
}

static int s5p_dp_detect_hpd(struct s5p_dp_device *dp)
{
	int timeout_loop = 0;

	s5p_dp_init_hpd(dp);

	udelay(200);

	while (s5p_dp_get_plug_in_status(dp) != 0) {
		timeout_loop++;
		if (DP_TIMEOUT_LOOP_COUNT < timeout_loop) {
			dev_err(dp->dev, "failed to get hpd plug status\n");
			return -ETIMEDOUT;
		}
		udelay(10);
	}

	return 0;
}

static unsigned char s5p_dp_calc_edid_check_sum(unsigned char *edid_data)
{
	int i;
	unsigned char sum = 0;

	for (i = 0; i < EDID_BLOCK_LENGTH; i++)
		sum = sum + edid_data[i];

	return sum;
}

static int s5p_dp_read_edid(struct s5p_dp_device *dp)
{
	unsigned char edid[EDID_BLOCK_LENGTH * 2];
	unsigned int extend_block = 0;
	unsigned char sum;
	unsigned char test_vector;
	int retval;

	/*
	 * EDID device address is 0x50.
	 * However, if necessary, you must have set upper address
	 * into E-EDID in I2C device, 0x30.
	 */

	/* Read Extension Flag, Number of 128-byte EDID extension blocks */
	s5p_dp_read_byte_from_i2c(dp, I2C_EDID_DEVICE_ADDR,
				EDID_EXTENSION_FLAG,
				&extend_block);

	if (extend_block > 0) {
		dev_dbg(dp->dev, "EDID data includes a single extension!\n");

		/* Read EDID data */
		retval = s5p_dp_read_bytes_from_i2c(dp, I2C_EDID_DEVICE_ADDR,
						EDID_HEADER_PATTERN,
						EDID_BLOCK_LENGTH,
						&edid[EDID_HEADER_PATTERN]);
		if (retval != 0) {
			dev_err(dp->dev, "EDID Read failed!\n");
			return -EIO;
		}
		sum = s5p_dp_calc_edid_check_sum(edid);
		if (sum != 0) {
			dev_err(dp->dev, "EDID bad checksum!\n");
			return -EIO;
		}

		/* Read additional EDID data */
		retval = s5p_dp_read_bytes_from_i2c(dp,
				I2C_EDID_DEVICE_ADDR,
				EDID_BLOCK_LENGTH,
				EDID_BLOCK_LENGTH,
				&edid[EDID_BLOCK_LENGTH]);
		if (retval != 0) {
			dev_err(dp->dev, "EDID Read failed!\n");
			return -EIO;
		}
		sum = s5p_dp_calc_edid_check_sum(&edid[EDID_BLOCK_LENGTH]);
		if (sum != 0) {
			dev_err(dp->dev, "EDID bad checksum!\n");
			return -EIO;
		}

		s5p_dp_read_byte_from_dpcd(dp, DPCD_ADDR_TEST_REQUEST,
					&test_vector);
		if (test_vector & DPCD_TEST_EDID_READ) {
			s5p_dp_write_byte_to_dpcd(dp,
				DPCD_ADDR_TEST_EDID_CHECKSUM,
				edid[EDID_BLOCK_LENGTH + EDID_CHECKSUM]);
			s5p_dp_write_byte_to_dpcd(dp,
				DPCD_ADDR_TEST_RESPONSE,
				DPCD_TEST_EDID_CHECKSUM_WRITE);
		}
	} else {
		dev_info(dp->dev, "EDID data does not include any extensions.\n");

		/* Read EDID data */
		retval = s5p_dp_read_bytes_from_i2c(dp,
				I2C_EDID_DEVICE_ADDR,
				EDID_HEADER_PATTERN,
				EDID_BLOCK_LENGTH,
				&edid[EDID_HEADER_PATTERN]);
		if (retval != 0) {
			dev_err(dp->dev, "EDID Read failed!\n");
			return -EIO;
		}
		sum = s5p_dp_calc_edid_check_sum(edid);
		if (sum != 0) {
			dev_err(dp->dev, "EDID bad checksum!\n");
			return -EIO;
		}

		s5p_dp_read_byte_from_dpcd(dp,
			DPCD_ADDR_TEST_REQUEST,
			&test_vector);
		if (test_vector & DPCD_TEST_EDID_READ) {
			s5p_dp_write_byte_to_dpcd(dp,
				DPCD_ADDR_TEST_EDID_CHECKSUM,
				edid[EDID_CHECKSUM]);
			s5p_dp_write_byte_to_dpcd(dp,
				DPCD_ADDR_TEST_RESPONSE,
				DPCD_TEST_EDID_CHECKSUM_WRITE);
		}
	}

	dev_err(dp->dev, "EDID Read success!\n");
	return 0;
}

static int s5p_dp_handle_edid(struct s5p_dp_device *dp)
{
	u8 buf[12];
	int i;
	int retval;

	/* Read DPCD DPCD_ADDR_DPCD_REV~RECEIVE_PORT1_CAP_1 */
	s5p_dp_read_bytes_from_dpcd(dp,
		DPCD_ADDR_DPCD_REV,
		12, buf);

	/* Read EDID */
	for (i = 0; i < 3; i++) {
		retval = s5p_dp_read_edid(dp);
		if (retval == 0)
			break;
	}

	return retval;
}

static void s5p_dp_enable_rx_to_enhanced_mode(struct s5p_dp_device *dp,
						bool enable)
{
	u8 data;

	s5p_dp_read_byte_from_dpcd(dp, DPCD_ADDR_LANE_COUNT_SET, &data);

	if (enable)
		s5p_dp_write_byte_to_dpcd(dp, DPCD_ADDR_LANE_COUNT_SET,
			DPCD_ENHANCED_FRAME_EN |
			DPCD_LANE_COUNT_SET(data));
	else
		s5p_dp_write_byte_to_dpcd(dp, DPCD_ADDR_LANE_COUNT_SET,
			DPCD_LANE_COUNT_SET(data));
}

static int s5p_dp_is_enhanced_mode_available(struct s5p_dp_device *dp)
{
	u8 data;
	int retval;

	s5p_dp_read_byte_from_dpcd(dp, DPCD_ADDR_MAX_LANE_COUNT, &data);
	retval = DPCD_ENHANCED_FRAME_CAP(data);

	return retval;
}

static void s5p_dp_set_enhanced_mode(struct s5p_dp_device *dp)
{
	u8 data;

	data = s5p_dp_is_enhanced_mode_available(dp);
	s5p_dp_enable_rx_to_enhanced_mode(dp, data);
	s5p_dp_enable_enhanced_mode(dp, data);
}

static void s5p_dp_training_pattern_dis(struct s5p_dp_device *dp)
{
	s5p_dp_set_training_pattern(dp, DP_NONE);

	s5p_dp_write_byte_to_dpcd(dp,
		DPCD_ADDR_TRAINING_PATTERN_SET,
		DPCD_TRAINING_PATTERN_DISABLED);
}

static void s5p_dp_set_lane_lane_pre_emphasis(struct s5p_dp_device *dp,
					int pre_emphasis, int lane)
{
	switch (lane) {
	case 0:
		s5p_dp_set_lane0_pre_emphasis(dp, pre_emphasis);
		break;
	case 1:
		s5p_dp_set_lane1_pre_emphasis(dp, pre_emphasis);
		break;

	case 2:
		s5p_dp_set_lane2_pre_emphasis(dp, pre_emphasis);
		break;

	case 3:
		s5p_dp_set_lane3_pre_emphasis(dp, pre_emphasis);
		break;
	}
}

static void s5p_dp_link_start(struct s5p_dp_device *dp)
{
	u8 buf[5];
	int lane;
	int lane_count;

	lane_count = dp->link_train.lane_count;

	dp->link_train.lt_state = CLOCK_RECOVERY;
	dp->link_train.eq_loop = 0;

	for (lane = 0; lane < lane_count; lane++)
		dp->link_train.cr_loop[lane] = 0;

	/* Set sink to D0 (Sink Not Ready) mode. */
	s5p_dp_write_byte_to_dpcd(dp, DPCD_ADDR_SINK_POWER_STATE,
				DPCD_SET_POWER_STATE_D0);

	/* Set link rate and count as you want to establish*/
	s5p_dp_set_link_bandwidth(dp, dp->link_train.link_rate);
	s5p_dp_set_lane_count(dp, dp->link_train.lane_count);

	/* Setup RX configuration */
	buf[0] = dp->link_train.link_rate;
	buf[1] = dp->link_train.lane_count;
	s5p_dp_write_bytes_to_dpcd(dp, DPCD_ADDR_LINK_BW_SET,
				2, buf);

	/* Set TX pre-emphasis to minimum */
	for (lane = 0; lane < lane_count; lane++)
		s5p_dp_set_lane_lane_pre_emphasis(dp,
			PRE_EMPHASIS_LEVEL_0, lane);

	/* Set training pattern 1 */
	s5p_dp_set_training_pattern(dp, TRAINING_PTN1);

	/* Set RX training pattern */
	buf[0] = DPCD_SCRAMBLING_DISABLED |
		 DPCD_TRAINING_PATTERN_1;
	s5p_dp_write_byte_to_dpcd(dp,
		DPCD_ADDR_TRAINING_PATTERN_SET, buf[0]);

	for (lane = 0; lane < lane_count; lane++)
		buf[lane] = DPCD_PRE_EMPHASIS_PATTERN2_LEVEL0 |
			    DPCD_VOLTAGE_SWING_PATTERN1_LEVEL0;
	s5p_dp_write_bytes_to_dpcd(dp,
		DPCD_ADDR_TRAINING_PATTERN_SET,
		lane_count, buf);
}

static unsigned char s5p_dp_get_lane_status(u8 link_status[6], int lane)
{
	int shift = (lane & 1) * 4;
	u8 link_value = link_status[lane>>1];

	return (link_value >> shift) & 0xf;
}

static int s5p_dp_clock_recovery_ok(u8 link_status[6], int lane_count)
{
	int lane;
	u8 lane_status;

	for (lane = 0; lane < lane_count; lane++) {
		lane_status = s5p_dp_get_lane_status(link_status, lane);
		if ((lane_status & DPCD_LANE_CR_DONE) == 0)
			return -EINVAL;
	}
	return 0;
}

static int s5p_dp_channel_eq_ok(u8 link_status[6], int lane_count)
{
	int lane;
	u8 lane_align;
	u8 lane_status;

	lane_align = link_status[2];
	if ((lane_align == DPCD_INTERLANE_ALIGN_DONE) == 0)
		return -EINVAL;

	for (lane = 0; lane < lane_count; lane++) {
		lane_status = s5p_dp_get_lane_status(link_status, lane);
		lane_status &= DPCD_CHANNEL_EQ_BITS;
		if (lane_status != DPCD_CHANNEL_EQ_BITS)
			return -EINVAL;
	}
	return 0;
}

static unsigned char s5p_dp_get_adjust_request_voltage(u8 adjust_request[2],
							int lane)
{
	int shift = (lane & 1) * 4;
	u8 link_value = adjust_request[lane>>1];

	return (link_value >> shift) & 0x3;
}

static unsigned char s5p_dp_get_adjust_request_pre_emphasis(
					u8 adjust_request[2],
					int lane)
{
	int shift = (lane & 1) * 4;
	u8 link_value = adjust_request[lane>>1];

	return ((link_value >> shift) & 0xc) >> 2;
}

static void s5p_dp_set_lane_link_training(struct s5p_dp_device *dp,
					u8 training_lane_set, int lane)
{
	switch (lane) {
	case 0:
		s5p_dp_set_lane0_link_training(dp, training_lane_set);
		break;
	case 1:
		s5p_dp_set_lane1_link_training(dp, training_lane_set);
		break;

	case 2:
		s5p_dp_set_lane2_link_training(dp, training_lane_set);
		break;

	case 3:
		s5p_dp_set_lane3_link_training(dp, training_lane_set);
		break;
	}
}

static unsigned int s5p_dp_get_lane_link_training(struct s5p_dp_device *dp,
						int lane)
{
	u32 reg;

	switch (lane) {
	case 0:
		reg = s5p_dp_get_lane0_link_training(dp);
		break;
	case 1:
		reg = s5p_dp_get_lane1_link_training(dp);
		break;
	case 2:
		reg = s5p_dp_get_lane2_link_training(dp);
		break;
	case 3:
		reg = s5p_dp_get_lane3_link_training(dp);
		break;
	}

	return reg;
}

static void s5p_dp_reduce_link_rate(struct s5p_dp_device *dp)
{
	if (dp->link_train.link_rate == LINK_RATE_2_70GBPS) {
		/* set to reduced bit rate */
		dp->link_train.link_rate = LINK_RATE_1_62GBPS;
		dev_err(dp->dev, "set to bandwidth %.2x\n",
			dp->link_train.link_rate);
		dp->link_train.lt_state = START;
	} else {
		s5p_dp_training_pattern_dis(dp);
		/* set enhanced mode if available */
		s5p_dp_set_enhanced_mode(dp);
		dp->link_train.lt_state = FAILED;
	}
}

static void s5p_dp_get_adjust_train(struct s5p_dp_device *dp,
				u8 adjust_request[2])
{
	int lane;
	int lane_count;
	u8 voltage_swing;
	u8 pre_emphasis;
	u8 training_lane;

	lane_count = dp->link_train.lane_count;
	for (lane = 0; lane < lane_count; lane++) {
		voltage_swing = s5p_dp_get_adjust_request_voltage(
						adjust_request, lane);
		pre_emphasis = s5p_dp_get_adjust_request_pre_emphasis(
						adjust_request, lane);
		training_lane = DPCD_VOLTAGE_SWING_SET(voltage_swing) |
				DPCD_PRE_EMPHASIS_SET(pre_emphasis);

		if (voltage_swing == VOLTAGE_LEVEL_3 ||
		   pre_emphasis == PRE_EMPHASIS_LEVEL_3) {
			training_lane |= DPCD_MAX_SWING_REACHED;
			training_lane |= DPCD_MAX_PRE_EMPHASIS_REACHED;
		}
		dp->link_train.training_lane[lane] = training_lane;
	}
}

static int s5p_dp_check_max_cr_loop(struct s5p_dp_device *dp, u8 voltage_swing)
{
	int lane;
	int lane_count;

	lane_count = dp->link_train.lane_count;
	for (lane = 0; lane < lane_count; lane++) {
		if (voltage_swing == VOLTAGE_LEVEL_3 ||
			dp->link_train.cr_loop[lane] == MAX_CR_LOOP)
			return -EINVAL;
	}
	return 0;
}

static int s5p_dp_process_clock_recovery(struct s5p_dp_device *dp)
{
	u8 data;
	u8 link_status[6];
	int lane;
	int lane_count;
	u8 buf[5];

	u8 adjust_request[2];
	u8 voltage_swing;
	u8 pre_emphasis;
	u8 training_lane;

	udelay(100);

	s5p_dp_read_bytes_from_dpcd(dp, DPCD_ADDR_LANE0_1_STATUS,
				6, link_status);
	lane_count = dp->link_train.lane_count;

	if (s5p_dp_clock_recovery_ok(link_status, lane_count) == 0) {
		/* set training pattern 2 for EQ */
		s5p_dp_set_training_pattern(dp, TRAINING_PTN2);

		adjust_request[0] = link_status[4];
		adjust_request[1] = link_status[5];

		s5p_dp_get_adjust_train(dp, adjust_request);

		buf[0] = DPCD_SCRAMBLING_DISABLED |
			 DPCD_TRAINING_PATTERN_2;
		s5p_dp_write_byte_to_dpcd(dp,
			DPCD_ADDR_TRAINING_LANE0_SET,
			buf[0]);

		for (lane = 0; lane < lane_count; lane++) {
			s5p_dp_set_lane_link_training(dp,
				dp->link_train.training_lane[lane],
				lane);
			buf[lane] = dp->link_train.training_lane[lane];
			s5p_dp_write_byte_to_dpcd(dp,
				DPCD_ADDR_TRAINING_LANE0_SET + lane,
				buf[lane]);
		}
		dp->link_train.lt_state = EQUALIZER_TRAINING;
	} else {
		s5p_dp_read_byte_from_dpcd(dp,
			DPCD_ADDR_ADJUST_REQUEST_LANE0_1,
			&data);
		adjust_request[0] = data;

		s5p_dp_read_byte_from_dpcd(dp,
			DPCD_ADDR_ADJUST_REQUEST_LANE2_3,
			&data);
		adjust_request[1] = data;

		for (lane = 0; lane < lane_count; lane++) {
			training_lane = s5p_dp_get_lane_link_training(dp, lane);

			voltage_swing = s5p_dp_get_adjust_request_voltage(
							adjust_request, lane);
			pre_emphasis = s5p_dp_get_adjust_request_pre_emphasis(
							adjust_request, lane);
			if ((DPCD_VOLTAGE_SWING_GET(training_lane) == voltage_swing) &&
			    (DPCD_PRE_EMPHASIS_GET(training_lane) == pre_emphasis))
				dp->link_train.cr_loop[lane]++;
			dp->link_train.training_lane[lane] = training_lane;
		}

		if (s5p_dp_check_max_cr_loop(dp, voltage_swing) != 0) {
			s5p_dp_reduce_link_rate(dp);
		} else {
			s5p_dp_get_adjust_train(dp, adjust_request);

			for (lane = 0; lane < lane_count; lane++) {
				s5p_dp_set_lane_link_training(dp,
					dp->link_train.training_lane[lane],
					lane);
				buf[lane] = dp->link_train.training_lane[lane];
				s5p_dp_write_byte_to_dpcd(dp,
					DPCD_ADDR_TRAINING_LANE0_SET + lane,
					buf[lane]);
			}
		}
	}

	return 0;
}

static int s5p_dp_process_equalizer_training(struct s5p_dp_device *dp)
{
	u8 link_status[6];
	int lane;
	int lane_count;
	u8 buf[5];
	u32 reg;

	u8 adjust_request[2];

	udelay(400);

	s5p_dp_read_bytes_from_dpcd(dp, DPCD_ADDR_LANE0_1_STATUS,
				6, link_status);
	lane_count = dp->link_train.lane_count;

	if (s5p_dp_clock_recovery_ok(link_status, lane_count) == 0) {
		adjust_request[0] = link_status[4];
		adjust_request[1] = link_status[5];

		if (s5p_dp_channel_eq_ok(link_status, lane_count) == 0) {
			/* traing pattern Set to Normal */
			s5p_dp_training_pattern_dis(dp);

			dev_info(dp->dev, "Link Training success!\n");

			s5p_dp_get_link_bandwidth(dp, &reg);
			dp->link_train.link_rate = reg;
			dev_dbg(dp->dev, "final bandwidth = %.2x\n",
				dp->link_train.link_rate);

			s5p_dp_get_lane_count(dp, &reg);
			dp->link_train.lane_count = reg;
			dev_dbg(dp->dev, "final lane count = %.2x\n",
				dp->link_train.lane_count);
			/* set enhanced mode if available */
			s5p_dp_set_enhanced_mode(dp);

			dp->link_train.lt_state = FINISHED;
		} else {
			/* not all locked */
			dp->link_train.eq_loop++;

			if (dp->link_train.eq_loop > MAX_EQ_LOOP) {
				s5p_dp_reduce_link_rate(dp);
			} else {
				s5p_dp_get_adjust_train(dp, adjust_request);

				for (lane = 0; lane < lane_count; lane++) {
					s5p_dp_set_lane_link_training(dp,
						dp->link_train.training_lane[lane],
						lane);
					buf[lane] = dp->link_train.training_lane[lane];
					s5p_dp_write_byte_to_dpcd(dp,
						DPCD_ADDR_TRAINING_LANE0_SET + lane,
						buf[lane]);
				}
			}
		}
	} else {
		s5p_dp_reduce_link_rate(dp);
	}

	return 0;
}

static void s5p_dp_get_max_rx_bandwidth(struct s5p_dp_device *dp,
			u8 *bandwidth)
{
	u8 data;

	/*
	 * For DP rev.1.1, Maximum link rate of Main Link lanes
	 * 0x06 = 1.62 Gbps, 0x0a = 2.7 Gbps
	 */
	s5p_dp_read_byte_from_dpcd(dp, DPCD_ADDR_MAX_LINK_RATE, &data);
	*bandwidth = data;
}

static void s5p_dp_get_max_rx_lane_count(struct s5p_dp_device *dp,
			u8 *lane_count)
{
	u8 data;

	/*
	 * For DP rev.1.1, Maximum number of Main Link lanes
	 * 0x01 = 1 lane, 0x02 = 2 lanes, 0x04 = 4 lanes
	 */
	s5p_dp_read_byte_from_dpcd(dp, DPCD_ADDR_MAX_LANE_COUNT, &data);
	*lane_count = DPCD_MAX_LANE_COUNT(data);
}

static void s5p_dp_init_training(struct s5p_dp_device *dp,
			enum link_lane_count_type max_lane,
			enum link_rate_type max_rate)
{
	/*
	 * MACRO_RST must be applied after the PLL_LOCK to avoid
	 * the DP inter pair skew issue for at least 10 us
	 */
	s5p_dp_reset_macro(dp);

	/* Initialize by reading RX's DPCD */
	s5p_dp_get_max_rx_bandwidth(dp, &dp->link_train.link_rate);
	s5p_dp_get_max_rx_lane_count(dp, &dp->link_train.lane_count);

	if ((dp->link_train.link_rate != LINK_RATE_1_62GBPS) &&
	   (dp->link_train.link_rate != LINK_RATE_2_70GBPS)) {
		dev_err(dp->dev, "Rx Max Link Rate is abnormal :%x !\n",
			dp->link_train.link_rate);
		dp->link_train.link_rate = LINK_RATE_1_62GBPS;
	}

	if (dp->link_train.lane_count == 0) {
		dev_err(dp->dev, "Rx Max Lane count is abnormal :%x !\n",
			dp->link_train.lane_count);
		dp->link_train.lane_count = (u8)LANE_COUNT1;
	}

	/* Setup TX lane count & rate */
	if (dp->link_train.lane_count > max_lane)
		dp->link_train.lane_count = max_lane;
	if (dp->link_train.link_rate > max_rate)
		dp->link_train.link_rate = max_rate;

	/* All DP analog module power up */
	s5p_dp_set_analog_power_down(dp, POWER_ALL, 0);
}

static int s5p_dp_sw_link_training(struct s5p_dp_device *dp)
{
	int retval = 0;
	int training_finished;

	/* Turn off unnecessary lane */
	if (dp->link_train.lane_count == 1)
		s5p_dp_set_analog_power_down(dp, CH1_BLOCK, 1);

	training_finished = 0;

	dp->link_train.lt_state = START;

	/* Process here */
	while (!training_finished) {
		switch (dp->link_train.lt_state) {
		case START:
			s5p_dp_link_start(dp);
			break;
		case CLOCK_RECOVERY:
			s5p_dp_process_clock_recovery(dp);
			break;
		case EQUALIZER_TRAINING:
			s5p_dp_process_equalizer_training(dp);
			break;
		case FINISHED:
			training_finished = 1;
			break;
		case FAILED:
			return -EREMOTEIO;
		}
	}

	return retval;
}

static int s5p_dp_set_link_train(struct s5p_dp_device *dp,
				u32 count,
				u32 bwtype)
{
	int i;
	int retval;

	for (i = 0; i < DP_TIMEOUT_LOOP_COUNT; i++) {
		s5p_dp_init_training(dp, count, bwtype);
		retval = s5p_dp_sw_link_training(dp);
		if (retval == 0)
			break;

		udelay(100);
	}

	return retval;
}

static int s5p_dp_config_video(struct s5p_dp_device *dp,
			struct video_info *video_info)
{
	int retval = 0;
	int timeout_loop = 0;
	int done_count = 0;

	s5p_dp_config_video_slave_mode(dp, video_info);

	s5p_dp_set_video_color_format(dp, video_info->color_depth,
			video_info->color_space,
			video_info->dynamic_range,
			video_info->ycbcr_coeff);

	if (s5p_dp_get_pll_lock_status(dp) == PLL_UNLOCKED) {
		dev_err(dp->dev, "PLL is not locked yet.\n");
		return -EINVAL;
	}

	for (;;) {
		timeout_loop++;
		if (s5p_dp_is_slave_video_stream_clock_on(dp) == 0)
			break;
		if (DP_TIMEOUT_LOOP_COUNT < timeout_loop) {
			dev_err(dp->dev, "Timeout of video streamclk ok\n");
			return -ETIMEDOUT;
		}

		mdelay(100);
	}

	/* Set to use the register calculated M/N video */
	s5p_dp_set_video_cr_mn(dp, CALCULATED_M, 0, 0);

	/* For video bist, Video timing must be generated by register */
	s5p_dp_set_video_timing_mode(dp, VIDEO_TIMING_FROM_CAPTURE);

	/* Disable video mute */
	s5p_dp_enable_video_mute(dp, 0);

	/* Configure video slave mode */
	s5p_dp_enable_video_master(dp, 0);

	/* Enable video */
	s5p_dp_start_video(dp);

	timeout_loop = 0;

	for (;;) {
		timeout_loop++;
		if (s5p_dp_is_video_stream_on(dp) == 0) {
			done_count++;
			if (done_count > 10)
				break;
		} else if (done_count) {
			done_count = 0;
		}
		if (DP_TIMEOUT_LOOP_COUNT < timeout_loop) {
			dev_err(dp->dev, "Timeout of video streamclk ok\n");
			return -ETIMEDOUT;
		}

		mdelay(100);
	}

	if (retval != 0)
		dev_err(dp->dev, "Video stream is not detected!\n");

	return retval;
}

static void s5p_dp_enable_scramble(struct s5p_dp_device *dp, bool enable)
{
	u8 data;

	if (enable) {
		s5p_dp_enable_scrambling(dp);

		s5p_dp_read_byte_from_dpcd(dp,
			DPCD_ADDR_TRAINING_PATTERN_SET,
			&data);
		s5p_dp_write_byte_to_dpcd(dp,
			DPCD_ADDR_TRAINING_PATTERN_SET,
			(u8)(data & ~DPCD_SCRAMBLING_DISABLED));
	} else {
		s5p_dp_disable_scrambling(dp);

		s5p_dp_read_byte_from_dpcd(dp,
			DPCD_ADDR_TRAINING_PATTERN_SET,
			&data);
		s5p_dp_write_byte_to_dpcd(dp,
			DPCD_ADDR_TRAINING_PATTERN_SET,
			(u8)(data | DPCD_SCRAMBLING_DISABLED));
	}
}

static irqreturn_t s5p_dp_irq_handler(int irq, void *arg)
{
	struct s5p_dp_device *dp = arg;

	dev_err(dp->dev, "s5p_dp_irq_handler\n");
	return IRQ_HANDLED;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void s5p_dp_early_suspend(struct early_suspend *handler)
{
	struct platform_device *pdev;
	struct s5p_dp_platdata *pdata;
	struct s5p_dp_device *dp;

	dp = container_of(handler, struct s5p_dp_device, early_suspend);
	pdev = to_platform_device(dp->dev);
	pdata = pdev->dev.platform_data;

	if (pdata->backlight_off)
		pdata->backlight_off();

	if (pdata && pdata->phy_exit)
		pdata->phy_exit();

	clk_disable(dp->clock);

	return;
}

static void s5p_dp_late_resume(struct early_suspend *handler)
{
	struct platform_device *pdev;
	struct s5p_dp_platdata *pdata;
	struct s5p_dp_device *dp;

	dp = container_of(handler, struct s5p_dp_device, early_suspend);
	pdev = to_platform_device(dp->dev);
	pdata = pdev->dev.platform_data;

	if (pdata && pdata->phy_init)
		pdata->phy_init();

	clk_enable(dp->clock);

	s5p_dp_init_dp(dp);

	if (!soc_is_exynos5250()) {
		s5p_dp_detect_hpd(dp);
		s5p_dp_handle_edid(dp);
	}

	s5p_dp_set_link_train(dp, dp->video_info->lane_count,
				dp->video_info->link_rate);

	if (soc_is_exynos5250()) {
		s5p_dp_enable_scramble(dp, 1);
		s5p_dp_enable_rx_to_enhanced_mode(dp, 1);
		s5p_dp_enable_enhanced_mode(dp, 1);
	} else {
		s5p_dp_enable_scramble(dp, 0);
		s5p_dp_enable_rx_to_enhanced_mode(dp, 0);
		s5p_dp_enable_enhanced_mode(dp, 0);
	}

	s5p_dp_set_lane_count(dp, dp->video_info->lane_count);
	s5p_dp_set_link_bandwidth(dp, dp->video_info->link_rate);

	s5p_dp_init_video(dp);
	s5p_dp_config_video(dp, dp->video_info);

	if (pdata->backlight_on)
		pdata->backlight_on();

	return;
}
#endif

static int s5p_dp_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct s5p_dp_device *dp;
	struct s5p_dp_platdata *pdata;

	int ret = 0;

	pdata = pdev->dev.platform_data;
	if (!pdata) {
		dev_err(&pdev->dev, "no platform data\n");
		return -EINVAL;
	}

	dp = kzalloc(sizeof(struct s5p_dp_device), GFP_KERNEL);
	if (!dp) {
		dev_err(&pdev->dev, "no memory for device data\n");
		return -ENOMEM;
	}

	dp->dev = &pdev->dev;

	dp->clock = clk_get(&pdev->dev, "dp");
	if (IS_ERR(dp->clock)) {
		dev_err(&pdev->dev, "failed to get clock\n");
		ret = PTR_ERR(dp->clock);
		goto err_dp;
	}

	clk_enable(dp->clock);

	pm_runtime_enable(dp->dev);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "failed to get registers\n");
		ret = -EINVAL;
		goto err_clock;
	}

	res = request_mem_region(res->start, resource_size(res),
				dev_name(&pdev->dev));
	if (!res) {
		dev_err(&pdev->dev, "failed to request registers region\n");
		ret = -EINVAL;
		goto err_clock;
	}

	dp->res = res;

	dp->reg_base = ioremap(res->start, resource_size(res));
	if (!dp->reg_base) {
		dev_err(&pdev->dev, "failed to ioremap\n");
		ret = -ENOMEM;
		goto err_req_region;
	}

	pm_runtime_get_sync(dp->dev);

#if 0 // to do check after wakeup
	dp->irq = platform_get_irq(pdev, 0);
	if (!dp->irq) {
		dev_err(&pdev->dev, "failed to get irq\n");
		ret = -ENODEV;
		goto err_ioremap;
	}

	ret = request_irq(dp->irq, s5p_dp_irq_handler, 0,
			"s5p-dp", dp);
	if (ret) {
		dev_err(&pdev->dev, "failed to request irq\n");
		goto err_ioremap;
	}
#endif
	dp->video_info = pdata->video_info;
	if (pdata->phy_init)
		pdata->phy_init();

	s5p_dp_init_dp(dp);

	if (!soc_is_exynos5250()) {
		ret = s5p_dp_detect_hpd(dp);
		if (ret) {
			dev_err(&pdev->dev, "unable to detect hpd\n");
			goto err_irq;
		}

		s5p_dp_handle_edid(dp);
	}

	ret = s5p_dp_set_link_train(dp, dp->video_info->lane_count,
				dp->video_info->link_rate);
	if (ret) {
		dev_err(&pdev->dev, "unable to do link train\n");
		goto err_irq;
	}

	if (soc_is_exynos5250()) {
		s5p_dp_enable_scramble(dp, 1);
		s5p_dp_enable_rx_to_enhanced_mode(dp, 1);
		s5p_dp_enable_enhanced_mode(dp, 1);
	} else {
		s5p_dp_enable_scramble(dp, 0);
		s5p_dp_enable_rx_to_enhanced_mode(dp, 0);
		s5p_dp_enable_enhanced_mode(dp, 0);
	}

	s5p_dp_set_lane_count(dp, dp->video_info->lane_count);
	s5p_dp_set_link_bandwidth(dp, dp->video_info->link_rate);

	s5p_dp_init_video(dp);
	ret = s5p_dp_config_video(dp, dp->video_info);
	if (ret) {
		dev_err(&pdev->dev, "unable to config video\n");
		goto err_irq;
	}

	if (pdata->backlight_on)
		pdata->backlight_on();

	platform_set_drvdata(pdev, dp);

#ifdef CONFIG_HAS_EARLYSUSPEND
	dp->early_suspend.suspend = s5p_dp_early_suspend;
	dp->early_suspend.resume = s5p_dp_late_resume;
	dp->early_suspend.level = EARLY_SUSPEND_LEVEL_DISABLE_FB - 1;
	register_early_suspend(&dp->early_suspend);
#endif
	return 0;

err_irq:
	free_irq(dp->irq, dp);
err_ioremap:
	iounmap(dp->reg_base);
err_req_region:
	release_mem_region(res->start, resource_size(res));
err_clock:
	clk_put(dp->clock);
err_dp:
	kfree(dp);

	return ret;
}

static int s5p_dp_remove(struct platform_device *pdev)
{
	struct s5p_dp_platdata *pdata = pdev->dev.platform_data;
	struct s5p_dp_device *dp = platform_get_drvdata(pdev);

#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&dp->early_suspend);
#endif

	if (pdata->backlight_off)
		pdata->backlight_off();

	if (pdata && pdata->phy_exit)
		pdata->phy_exit();

	free_irq(dp->irq, dp);
	iounmap(dp->reg_base);

	clk_disable(dp->clock);
	clk_put(dp->clock);

	release_mem_region(dp->res->start, resource_size(dp->res));

	pm_runtime_put_sync(dp->dev);
	pm_runtime_disable(dp->dev);

	kfree(dp);

	return 0;
}

#ifdef CONFIG_PM
#ifndef CONFIG_HAS_EARLYSUSPEND
static int s5p_dp_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct s5p_dp_platdata *pdata = pdev->dev.platform_data;
	struct s5p_dp_device *dp = platform_get_drvdata(pdev);

	if (pdata->backlight_off)
		pdata->backlight_off();

	if (pdata && pdata->phy_exit)
		pdata->phy_exit();

	clk_disable(dp->clock);
	pm_runtime_put_sync(dp->dev);

	return 0;
}

static int s5p_dp_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct s5p_dp_platdata *pdata = pdev->dev.platform_data;
	struct s5p_dp_device *dp = platform_get_drvdata(pdev);

	if (pdata && pdata->phy_init)
		pdata->phy_init();

	pm_runtime_get_sync(dp->dev);
	clk_enable(dp->clock);

	s5p_dp_init_dp(dp);

	if (!soc_is_exynos5250()) {
		s5p_dp_detect_hpd(dp);
		s5p_dp_handle_edid(dp);
	}

	s5p_dp_set_link_train(dp, dp->video_info->lane_count,
				dp->video_info->link_rate);

	if (soc_is_exynos5250()) {
		s5p_dp_enable_scramble(dp, 1);
		s5p_dp_enable_rx_to_enhanced_mode(dp, 1);
		s5p_dp_enable_enhanced_mode(dp, 1);
	} else {
		s5p_dp_enable_scramble(dp, 0);
		s5p_dp_enable_rx_to_enhanced_mode(dp, 0);
		s5p_dp_enable_enhanced_mode(dp, 0);
	}

	s5p_dp_set_lane_count(dp, dp->video_info->lane_count);
	s5p_dp_set_link_bandwidth(dp, dp->video_info->link_rate);

	s5p_dp_init_video(dp);
	s5p_dp_config_video(dp, dp->video_info);

	if (pdata->backlight_on)
		pdata->backlight_on();

	return 0;
}
#endif
static int s5p_dp_runtime_suspend(struct device *dev)
{
	return 0;
}

static int s5p_dp_runtime_resume(struct device *dev)
{
	return 0;
}
#else
#define s5p_dp_suspend NULL
#define s5p_dp_resume NULL
#define s5p_dp_runtime_suspend NULL
#define s5p_dp_runtime_resume NULL
#endif

static const struct dev_pm_ops s5p_dp_pm_ops = {
#ifndef CONFIG_HAS_EARLYSUSPEND
	.suspend		= s5p_dp_suspend,
	.resume			= s5p_dp_resume,
#endif
	.runtime_suspend	= s5p_dp_runtime_suspend,
	.runtime_resume		= s5p_dp_runtime_resume,
};

static struct platform_driver s5p_dp_driver = {
	.probe		= s5p_dp_probe,
	.remove		= s5p_dp_remove,
	.driver		= {
		.name	= "s5p-dp",
		.owner	= THIS_MODULE,
		.pm	= &s5p_dp_pm_ops,
	},
};

static int __init s5p_dp_init(void)
{
	return platform_driver_probe(&s5p_dp_driver, s5p_dp_probe);
}

static void __exit s5p_dp_exit(void)
{
	platform_driver_unregister(&s5p_dp_driver);
}

#ifdef CONFIG_FB_EXYNOS_FIMD_MC
late_initcall(s5p_dp_init);
#else
module_init(s5p_dp_init);
#endif
module_exit(s5p_dp_exit);

MODULE_AUTHOR("Jingoo Han <jg1.han@samsung.com>");
MODULE_DESCRIPTION("Samsung SoC DP Driver");
MODULE_LICENSE("GPL");
