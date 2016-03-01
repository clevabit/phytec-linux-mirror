/*
 * Driver for MT9M111/MT9M112/MT9M131 CMOS Image Sensor from Micron/Aptina
 *
 * Copyright (C) 2008, Robert Jarzmik <robert.jarzmik@free.fr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/videodev2.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/log2.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/v4l2-mediabus.h>
#include <linux/module.h>

#include <linux/pinctrl/consumer.h>

#include <media/soc_camera.h>
#include <media/v4l2-clk.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>

/*
 * MT9M111, MT9M112 and MT9M131:
 * i2c address is 0x48 or 0x5d (depending on SADDR pin)
 * The platform has to define struct i2c_board_info objects and link to them
 * from struct soc_camera_host_desc
 */

/*
 * Sensor core register addresses (0x000..0x0ff)
 */
#define MT9M111_CHIP_VERSION		0x000
#define MT9M111_ROW_START		0x001
#define MT9M111_COLUMN_START		0x002
#define MT9M111_WINDOW_HEIGHT		0x003
#define MT9M111_WINDOW_WIDTH		0x004
#define MT9M111_HORIZONTAL_BLANKING_B	0x005
#define MT9M111_VERTICAL_BLANKING_B	0x006
#define MT9M111_HORIZONTAL_BLANKING_A	0x007
#define MT9M111_VERTICAL_BLANKING_A	0x008
#define MT9M111_SHUTTER_WIDTH		0x009
#define MT9M111_ROW_SPEED		0x00a
#define MT9M111_EXTRA_DELAY		0x00b
#define MT9M111_SHUTTER_DELAY		0x00c
#define MT9M111_RESET			0x00d
#define MT9M111_READ_MODE_B		0x020
#define MT9M111_READ_MODE_A		0x021
#define MT9M111_FLASH_CONTROL		0x023
#define MT9M111_GREEN1_GAIN		0x02b
#define MT9M111_BLUE_GAIN		0x02c
#define MT9M111_RED_GAIN		0x02d
#define MT9M111_GREEN2_GAIN		0x02e
#define MT9M111_GLOBAL_GAIN		0x02f
#define MT9M111_CONTEXT_CONTROL		0x0c8
#define MT9M111_PAGE_MAP		0x0f0
#define MT9M111_BYTE_WISE_ADDR		0x0f1

#define MT9M111_RESET_SYNC_CHANGES	(1 << 15)
#define MT9M111_RESET_RESTART_BAD_FRAME	(1 << 9)
#define MT9M111_RESET_SHOW_BAD_FRAMES	(1 << 8)
#define MT9M111_RESET_RESET_SOC		(1 << 5)
#define MT9M111_RESET_OUTPUT_DISABLE	(1 << 4)
#define MT9M111_RESET_CHIP_ENABLE	(1 << 3)
#define MT9M111_RESET_ANALOG_STANDBY	(1 << 2)
#define MT9M111_RESET_RESTART_FRAME	(1 << 1)
#define MT9M111_RESET_RESET_MODE	(1 << 0)

#define MT9M111_RM_FULL_POWER_RD	(0 << 10)
#define MT9M111_RM_LOW_POWER_RD		(1 << 10)
#define MT9M111_RM_COL_SKIP_4X		(1 << 5)
#define MT9M111_RM_ROW_SKIP_4X		(1 << 4)
#define MT9M111_RM_COL_SKIP_2X		(1 << 3)
#define MT9M111_RM_ROW_SKIP_2X		(1 << 2)
#define MT9M111_RMB_MIRROR_COLS		(1 << 1)
#define MT9M111_RMB_MIRROR_ROWS		(1 << 0)
#define MT9M111_CTXT_CTRL_RESTART	(1 << 15)
#define MT9M111_CTXT_CTRL_DEFECTCOR_B	(1 << 12)
#define MT9M111_CTXT_CTRL_RESIZE_B	(1 << 10)
#define MT9M111_CTXT_CTRL_CTRL2_B	(1 << 9)
#define MT9M111_CTXT_CTRL_GAMMA_B	(1 << 8)
#define MT9M111_CTXT_CTRL_XENON_EN	(1 << 7)
#define MT9M111_CTXT_CTRL_READ_MODE_B	(1 << 3)
#define MT9M111_CTXT_CTRL_LED_FLASH_EN	(1 << 2)
#define MT9M111_CTXT_CTRL_VBLANK_SEL_B	(1 << 1)
#define MT9M111_CTXT_CTRL_HBLANK_SEL_B	(1 << 0)

/*
 * Colorpipe register addresses (0x100..0x1ff)
 */
#define MT9M111_OPER_MODE_CTRL		0x106
#define MT9M111_OUTPUT_FORMAT_CTRL	0x108
#define MT9M111_REDUCER_XZOOM_B		0x1a0
#define MT9M111_REDUCER_XSIZE_B		0x1a1
#define MT9M111_REDUCER_YZOOM_B		0x1a3
#define MT9M111_REDUCER_YSIZE_B		0x1a4
#define MT9M111_REDUCER_XZOOM_A		0x1a6
#define MT9M111_REDUCER_XSIZE_A		0x1a7
#define MT9M111_REDUCER_YZOOM_A		0x1a9
#define MT9M111_REDUCER_YSIZE_A		0x1aa

#define MT9M111_OUTPUT_FORMAT_CTRL2_A	0x13a
#define MT9M111_OUTPUT_FORMAT_CTRL2_B	0x19b

#define MT9M111_OPMODE_AUTOEXPO_EN	(1 << 14)
#define MT9M111_OPMODE_AUTOWHITEBAL_EN	(1 << 1)
#define MT9M111_OUTFMT_FLIP_BAYER_COL	(1 << 9)
#define MT9M111_OUTFMT_FLIP_BAYER_ROW	(1 << 8)
#define MT9M111_OUTFMT_PROCESSED_BAYER	(1 << 14)
/* TODO: this is undocumented and mentioned in TN09163_A note only */
#define MT9M111_OUTFMT_SOC_AS_SENSOR	BIT(12)
#define MT9M111_OUTFMT_BYPASS_IFP	(1 << 10)
#define MT9M111_OUTFMT_INV_PIX_CLOCK	(1 << 9)
#define MT9M111_OUTFMT_RGB		(1 << 8)
#define MT9M111_OUTFMT_RGB565		(0 << 6)
#define MT9M111_OUTFMT_RGB555		(1 << 6)
#define MT9M111_OUTFMT_RGB444x		(2 << 6)
#define MT9M111_OUTFMT_RGBx444		(3 << 6)
#define MT9M111_OUTFMT_TST_RAMP_OFF	(0 << 4)
#define MT9M111_OUTFMT_TST_RAMP_COL	(1 << 4)
#define MT9M111_OUTFMT_TST_RAMP_ROW	(2 << 4)
#define MT9M111_OUTFMT_TST_RAMP_FRAME	(3 << 4)
#define MT9M111_OUTFMT_SHIFT_3_UP	(1 << 3)
#define MT9M111_OUTFMT_AVG_CHROMA	(1 << 2)
#define MT9M111_OUTFMT_SWAP_YCbCr_C_Y_RGB_EVEN	(1 << 1)
#define MT9M111_OUTFMT_SWAP_YCbCr_Cb_Cr_RGB_R_B	(1 << 0)

#define V4L2_CID_SKIP_X		(V4L2_CID_USER_BASE | 0x1000)
#define V4L2_CID_SKIP_Y		(V4L2_CID_USER_BASE | 0x1001)
#define V4L2_CID_X_PIXEL_RATE	(V4L2_CID_USER_BASE | 0x1002)

/*
 * Camera control register addresses (0x200..0x2ff not implemented)
 */

#define reg_read(reg) mt9m111_reg_read(client, MT9M111_##reg)
#define reg_write(reg, val) mt9m111_reg_write(client, MT9M111_##reg, (val))
#define reg_set(reg, val) mt9m111_reg_set(client, MT9M111_##reg, (val))
#define reg_clear(reg, val) mt9m111_reg_clear(client, MT9M111_##reg, (val))
#define reg_mask(reg, val, mask) mt9m111_reg_mask(client, MT9M111_##reg, \
		(val), (mask))

#define MT9M111_MIN_DARK_ROWS	8
#define MT9M111_MIN_DARK_COLS	26
#define MT9M111_MAX_HEIGHT	1024
#define MT9M111_MAX_WIDTH	1280

struct mt9m111_context {
	u16 read_mode;
	u16 blanking_h;
	u16 blanking_v;
	u16 reducer_xzoom;
	u16 reducer_yzoom;
	u16 reducer_xsize;
	u16 reducer_ysize;
	u16 output_fmt_ctrl2;
	u16 control;
};

static struct mt9m111_context context_a = {
	.read_mode		= MT9M111_READ_MODE_A,
	.blanking_h		= MT9M111_HORIZONTAL_BLANKING_A,
	.blanking_v		= MT9M111_VERTICAL_BLANKING_A,
	.reducer_xzoom		= MT9M111_REDUCER_XZOOM_A,
	.reducer_yzoom		= MT9M111_REDUCER_YZOOM_A,
	.reducer_xsize		= MT9M111_REDUCER_XSIZE_A,
	.reducer_ysize		= MT9M111_REDUCER_YSIZE_A,
	.output_fmt_ctrl2	= MT9M111_OUTPUT_FORMAT_CTRL2_A,
	.control		= MT9M111_CTXT_CTRL_RESTART,
};

static struct mt9m111_context context_b = {
	.read_mode		= MT9M111_READ_MODE_B,
	.blanking_h		= MT9M111_HORIZONTAL_BLANKING_B,
	.blanking_v		= MT9M111_VERTICAL_BLANKING_B,
	.reducer_xzoom		= MT9M111_REDUCER_XZOOM_B,
	.reducer_yzoom		= MT9M111_REDUCER_YZOOM_B,
	.reducer_xsize		= MT9M111_REDUCER_XSIZE_B,
	.reducer_ysize		= MT9M111_REDUCER_YSIZE_B,
	.output_fmt_ctrl2	= MT9M111_OUTPUT_FORMAT_CTRL2_B,
	.control		= MT9M111_CTXT_CTRL_RESTART |
		MT9M111_CTXT_CTRL_DEFECTCOR_B | MT9M111_CTXT_CTRL_RESIZE_B |
		MT9M111_CTXT_CTRL_CTRL2_B | MT9M111_CTXT_CTRL_GAMMA_B |
		MT9M111_CTXT_CTRL_READ_MODE_B | MT9M111_CTXT_CTRL_VBLANK_SEL_B |
		MT9M111_CTXT_CTRL_HBLANK_SEL_B,
};

/* MT9M111 has only one fixed colorspace per pixelcode */
struct mt9m111_datafmt {
	u32	code;
	enum v4l2_colorspace		colorspace;
	bool bypass_ifp:1;
	bool is_bayer:1;
};

static const struct mt9m111_datafmt mt9m111_colour_fmts[] = {
	{MEDIA_BUS_FMT_YUYV8_2X8, V4L2_COLORSPACE_JPEG, false, false},
	{MEDIA_BUS_FMT_YVYU8_2X8, V4L2_COLORSPACE_JPEG, false, false},
	{MEDIA_BUS_FMT_UYVY8_2X8, V4L2_COLORSPACE_JPEG, false, false},
	{MEDIA_BUS_FMT_VYUY8_2X8, V4L2_COLORSPACE_JPEG, false, false},
	{MEDIA_BUS_FMT_RGB555_2X8_PADHI_LE, V4L2_COLORSPACE_SRGB, false, false},
	{MEDIA_BUS_FMT_RGB555_2X8_PADHI_BE, V4L2_COLORSPACE_SRGB, false, false},
	{MEDIA_BUS_FMT_RGB565_2X8_LE, V4L2_COLORSPACE_SRGB, false, false},
	{MEDIA_BUS_FMT_RGB565_2X8_BE, V4L2_COLORSPACE_SRGB, false, false},
	{MEDIA_BUS_FMT_BGR565_2X8_LE, V4L2_COLORSPACE_SRGB, false, false},
	{MEDIA_BUS_FMT_BGR565_2X8_BE, V4L2_COLORSPACE_SRGB, false, false},
	{MEDIA_BUS_FMT_SBGGR10_2X8_PADHI_LE, V4L2_COLORSPACE_SRGB, true, true},
};

static const struct mt9m111_datafmt mt9m111_10bit_fmts[] = {
	{MEDIA_BUS_FMT_SBGGR10_1X10, V4L2_COLORSPACE_SRGB, true, true},
	{MEDIA_BUS_FMT_SGBRG10_1X10, V4L2_COLORSPACE_SRGB, true, true},
	{MEDIA_BUS_FMT_SGRBG10_1X10, V4L2_COLORSPACE_SRGB, true, true},
	{MEDIA_BUS_FMT_SRGGB10_1X10, V4L2_COLORSPACE_SRGB, true, true},
};

static const struct mt9m111_datafmt mt9m111_processed_fmts[] = {
	{MEDIA_BUS_FMT_SBGGR8_1X8, V4L2_COLORSPACE_SRGB, false, true},
	{MEDIA_BUS_FMT_SGBRG8_1X8, V4L2_COLORSPACE_SRGB, false, true},
	{MEDIA_BUS_FMT_SGRBG8_1X8, V4L2_COLORSPACE_SRGB, false, true},
	{MEDIA_BUS_FMT_SRGGB8_1X8, V4L2_COLORSPACE_SRGB, false, true},
};

enum mt9m111_pin_state {
	/* pixel signals, i2c + clock are on; set, when sensor is streaming */
	PIN_STATE_ACTIVE,
	/* pixel signals are not needed; i2c + clock are on */
	PIN_STATE_IDLE,
	/* pixel signals, i2c + clock are not needed */
	PIN_STATE_SLEEP,
};

struct mt9m111 {
	struct v4l2_subdev subdev;
	struct media_pad pad;
	struct v4l2_ctrl_handler hdl;
	struct v4l2_ctrl *gain;
	struct mt9m111_context *ctx;
	struct v4l2_rect rect;	/* cropping rectangle */
	struct v4l2_clk *clk;
	unsigned int width;	/* output */
	unsigned int height;	/* sizes */
	struct mutex power_lock; /* lock to protect power_count */
	int power_count;
	const struct mt9m111_datafmt *fmt;
	int lastpage;	/* PageMap cache value */

	unsigned char			skip_x; /* shift count */
	unsigned char			skip_y; /* shift count */

	bool				invert_pixclk:1;
	bool				allow_10bit:1;
	bool				allow_burst:1;

	bool				dirty_dim:1;
	bool				is_streaming:1;

	struct mutex			dev_lock;
	unsigned int			ref_cnt;

#ifdef CONFIG_PINCTRL
	struct pinctrl			*pinctrl;
	struct pinctrl_state		*pin_st[3];
#endif
};

static int mt9m111_pinctrl_state(struct mt9m111 *mt9m111,
				 enum mt9m111_pin_state state)
{
	struct pinctrl		*pinctrl;
	struct pinctrl_state	*st;

#ifdef CONFIG_PINCTRL
	pinctrl = mt9m111->pinctrl;
	st      = mt9m111->pin_st[state];
#else
	st      = NULL;
#endif

	if (!st)
		return 0;

	return pinctrl_select_state(mt9m111->pinctrl, st);
}

/* ensures that sensor is at least in IDLE state */
static int mt9m111_get_device(struct mt9m111 *mt9m111)
{
	int		rc;
	bool		have_clk = false;

	mutex_lock(&mt9m111->dev_lock);

	if (mt9m111->ref_cnt == 0) {
		struct i2c_client	*client =
			v4l2_get_subdevdata(&mt9m111->subdev);

		if (mt9m111->clk) {
			rc = v4l2_clk_enable(mt9m111->clk);
			if (rc < 0) {
				dev_err(&client->dev,
					"failed to enable clock: %d\n", rc);
				goto out;
			}

			have_clk = true;

		}

		rc = mt9m111_pinctrl_state(mt9m111, PIN_STATE_IDLE);
		if (rc < 0) {
			dev_err(&client->dev, "failed to setup pins: %d\n", rc);
			goto out;
		}
	}

	++mt9m111->ref_cnt;
	rc = 0;

out:
	if (rc < 0) {
		if (have_clk)
			v4l2_clk_disable(mt9m111->clk);
	}

	mutex_unlock(&mt9m111->dev_lock);

	return rc;
}

static void mt9m111_put_device(struct mt9m111 *mt9m111)
{
	int		rc;

	mutex_lock(&mt9m111->dev_lock);

	if (WARN_ON(mt9m111->ref_cnt == 0))
		goto out;

	if (mt9m111->ref_cnt == 1) {
		struct i2c_client	*client =
			v4l2_get_subdevdata(&mt9m111->subdev);

		rc = mt9m111_pinctrl_state(mt9m111, PIN_STATE_SLEEP);
		if (rc < 0) {
			dev_warn(&client->dev,
				 "failed to disable pins: %d\n", rc);
			/* ignore error */
		}

		if (mt9m111->clk)
			v4l2_clk_disable(mt9m111->clk);
	}

	--mt9m111->ref_cnt;

out:
	mutex_unlock(&mt9m111->dev_lock);
}

/* Find a data format by a pixel code */
static const struct mt9m111_datafmt *mt9m111_find_datafmt(struct mt9m111 *mt9m111,
						u32 code)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mt9m111_colour_fmts); i++)
		if (mt9m111_colour_fmts[i].code == code)
			return mt9m111_colour_fmts + i;

	if (mt9m111->allow_10bit) {
		for (i = 0; i < ARRAY_SIZE(mt9m111_10bit_fmts); i++)
			if (mt9m111_10bit_fmts[i].code == code)
				return mt9m111_10bit_fmts + i;
	}

	if (mt9m111->allow_burst) {
		for (i = 0; i < ARRAY_SIZE(mt9m111_processed_fmts); i++)
			if (mt9m111_processed_fmts[i].code == code)
				return mt9m111_processed_fmts + i;
	}

	return mt9m111->fmt;
}

static struct mt9m111 *to_mt9m111(const struct i2c_client *client)
{
	return container_of(i2c_get_clientdata(client), struct mt9m111, subdev);
}

static int reg_page_map_set(struct i2c_client *client, const u16 reg)
{
	int ret;
	u16 page;
	struct mt9m111 *mt9m111 = to_mt9m111(client);

	WARN_ON(mt9m111->ref_cnt == 0);

	page = (reg >> 8);
	if (page == mt9m111->lastpage)
		return 0;
	if (page > 2)
		return -EINVAL;

	ret = i2c_smbus_write_word_swapped(client, MT9M111_PAGE_MAP, page);
	if (!ret)
		mt9m111->lastpage = page;
	return ret;
}

static int mt9m111_reg_read(struct i2c_client *client, const u16 reg)
{
	struct mt9m111 *mt9m111 = to_mt9m111(client);
	int ret;

	WARN_ON(mt9m111->ref_cnt == 0);

	ret = reg_page_map_set(client, reg);
	if (!ret)
		ret = i2c_smbus_read_word_swapped(client, reg & 0xff);

	dev_dbg(&client->dev, "read  reg.%03x -> %04x\n", reg, ret);
	return ret;
}

static int mt9m111_reg_write(struct i2c_client *client, const u16 reg,
			     const u16 data)
{
	struct mt9m111 *mt9m111 = to_mt9m111(client);
	int ret;

	WARN_ON(mt9m111->ref_cnt == 0);

	ret = reg_page_map_set(client, reg);
	if (!ret)
		ret = i2c_smbus_write_word_swapped(client, reg & 0xff, data);
	dev_dbg(&client->dev, "write reg.%03x = %04x -> %d\n", reg, data, ret);
	return ret;
}

static int mt9m111_reg_set(struct i2c_client *client, const u16 reg,
			   const u16 data)
{
	int ret;

	ret = mt9m111_reg_read(client, reg);
	if (ret >= 0)
		ret = mt9m111_reg_write(client, reg, ret | data);
	return ret;
}

static int mt9m111_reg_clear(struct i2c_client *client, const u16 reg,
			     const u16 data)
{
	int ret;

	ret = mt9m111_reg_read(client, reg);
	if (ret >= 0)
		ret = mt9m111_reg_write(client, reg, ret & ~data);
	return ret;
}

static int mt9m111_reg_mask(struct i2c_client *client, const u16 reg,
			    const u16 data, const u16 mask)
{
	int ret;

	ret = mt9m111_reg_read(client, reg);
	if (ret >= 0)
		ret = mt9m111_reg_write(client, reg, (ret & ~mask) | data);
	return ret;
}

static int mt9m111_set_context(struct mt9m111 *mt9m111,
			       struct mt9m111_context *ctx)
{
	struct i2c_client *client = v4l2_get_subdevdata(&mt9m111->subdev);
	return reg_write(CONTEXT_CONTROL, ctx->control);
}

static int _mt9m111_set_selection(struct mt9m111 *mt9m111,
				  struct v4l2_rect const *r,
				  struct mt9m111_datafmt const *fmt,
				  unsigned int width, unsigned int height)
{
	struct i2c_client	*client = v4l2_get_subdevdata(&mt9m111->subdev);
	int			rc;
	bool			allow_scaling;
	struct mt9m111_context	*ctx = mt9m111->ctx;

	struct regval {
		unsigned int	reg;
		unsigned int	val;
	};

	struct regval		setup[12];
	struct regval		*s = setup;
	struct regval const	*reg;


	if (!fmt  || !ctx ||
	    mt9m111->width == 0 || mt9m111->height == 0)
		return -EINVAL;

	allow_scaling = mt9m111->allow_burst && !fmt->bypass_ifp;

	dev_dbg(&client->dev,
		"setting selection %ux%u+%ux%u -> %ux%u >> (%d,%d)\n",
		r->left, r->top, r->width, r->height,
		width, height,
		mt9m111->skip_x, mt9m111->skip_y);

	/* cropping parameters */

	*s++ = (struct regval){ MT9M111_COLUMN_START,  r->left };
	*s++ = (struct regval){ MT9M111_ROW_START,     r->top };
	*s++ = (struct regval){ MT9M111_WINDOW_WIDTH,  r->width };
	*s++ = (struct regval){ MT9M111_WINDOW_HEIGHT, r->height };


	/* output parameters */

	/* note: this can be rejected/ignored because constraints are
	 * violated; write it again below */
	if (mt9m111->dirty_dim || width != mt9m111->width)
		*s++ = (struct regval){ ctx->reducer_xsize,
					width >> mt9m111->skip_x };

	if (mt9m111->dirty_dim || r->width != mt9m111->rect.width)
		*s++ = (struct regval){ ctx->reducer_xzoom,    r->width };

	/* set reduxer_size twice to handle the case when the (new_)size <
	 * (old_)zoom constraint is violated */
	if (mt9m111->dirty_dim || width != mt9m111->width)
		*s++ = (struct regval){ ctx->reducer_xsize,
					width >> mt9m111->skip_x };


	/* note: this can be rejected/ignored because constraints are
	 * violated; write it again below */
	if (mt9m111->dirty_dim || height != mt9m111->height)
		*s++ = (struct regval){ ctx->reducer_ysize,
					height >> mt9m111->skip_y };

	if (mt9m111->dirty_dim || r->height != mt9m111->rect.height)
		*s++ = (struct regval){ ctx->reducer_yzoom, r->height };

	/* set reduxer_size twice to handle the case when the (new_)size <
	 * (old_)zoom constraint is violated */
	if (mt9m111->dirty_dim || height != mt9m111->height)
		*s++ = (struct regval){ ctx->reducer_ysize,
					height >> mt9m111->skip_y };


	rc = mt9m111_get_device(mt9m111);
	if (rc < 0)
		return rc;

	for (reg = &setup[0]; reg < s; ++reg) {
		rc = mt9m111_reg_write(client, reg->reg, reg->val);
		if (rc < 0)
			break;
	}

	if (rc < 0)
		goto out;

	rc = mt9m111_reg_mask(client, ctx->read_mode,
			      ((mt9m111->skip_x == 0 ? 0 :
				mt9m111->skip_x == 1 ? 1 :
				mt9m111->skip_x == 2 ? 4 : 5) << 3) |
			      ((mt9m111->skip_y == 0 ? 0 :
				mt9m111->skip_y == 1 ? 1 :
				mt9m111->skip_y == 2 ? 4 : 5) << 2),
			      0x0f << 2);

	if (rc < 0)
		goto out;

	mt9m111->width = width;
	mt9m111->height = height;
	mt9m111->rect = *r;
	mt9m111->dirty_dim = false;

	rc = 0;

out:
	mt9m111_put_device(mt9m111);

	return rc;
}

static int mt9m111_enable(struct mt9m111 *mt9m111)
{
	struct i2c_client *client = v4l2_get_subdevdata(&mt9m111->subdev);
	return reg_write(RESET, MT9M111_RESET_CHIP_ENABLE);
}

static int mt9m111_reset(struct mt9m111 *mt9m111)
{
	struct i2c_client *client = v4l2_get_subdevdata(&mt9m111->subdev);
	int ret;

	ret = reg_set(RESET, MT9M111_RESET_RESET_MODE);
	if (!ret)
		ret = reg_set(RESET, MT9M111_RESET_RESET_SOC);
	if (!ret)
		ret = reg_clear(RESET, MT9M111_RESET_RESET_MODE
				| MT9M111_RESET_RESET_SOC);

	return ret;
}

static int mt9m111_get_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *mf = &format->format;
	struct mt9m111 *mt9m111 = container_of(sd, struct mt9m111, subdev);

	if (format->pad)
		return -EINVAL;

	mf->width	= mt9m111->width >> mt9m111->skip_x;
	mf->height	= mt9m111->height >> mt9m111->skip_y;
	mf->code	= mt9m111->fmt->code;
	mf->colorspace	= mt9m111->fmt->colorspace;
	mf->field	= V4L2_FIELD_NONE;

	return 0;
}

static int mt9m111_set_pixfmt(struct mt9m111 *mt9m111,
			      u32 code)
{
	struct i2c_client *client = v4l2_get_subdevdata(&mt9m111->subdev);
	u16 data_outfmt2, mask_outfmt2 = MT9M111_OUTFMT_PROCESSED_BAYER |
		MT9M111_OUTFMT_BYPASS_IFP | MT9M111_OUTFMT_RGB |
		MT9M111_OUTFMT_SOC_AS_SENSOR |
		MT9M111_OUTFMT_RGB565 | MT9M111_OUTFMT_RGB555 |
		MT9M111_OUTFMT_RGB444x | MT9M111_OUTFMT_RGBx444 |
		MT9M111_OUTFMT_SWAP_YCbCr_C_Y_RGB_EVEN |
		MT9M111_OUTFMT_SWAP_YCbCr_Cb_Cr_RGB_R_B |
		MT9M111_OUTFMT_INV_PIX_CLOCK;
	int ret;

	switch (code) {
	case MEDIA_BUS_FMT_SBGGR8_1X8:
	case MEDIA_BUS_FMT_SGBRG8_1X8:
	case MEDIA_BUS_FMT_SGRBG8_1X8:
	case MEDIA_BUS_FMT_SRGGB8_1X8:
		data_outfmt2 = MT9M111_OUTFMT_PROCESSED_BAYER |
			MT9M111_OUTFMT_RGB;
		break;

	case MEDIA_BUS_FMT_SBGGR10_1X10:
	case MEDIA_BUS_FMT_SGBRG10_1X10:
	case MEDIA_BUS_FMT_SGRBG10_1X10:
	case MEDIA_BUS_FMT_SRGGB10_1X10:
		data_outfmt2 = MT9M111_OUTFMT_SOC_AS_SENSOR;
		break;

	case MEDIA_BUS_FMT_SBGGR10_2X8_PADHI_LE:
		data_outfmt2 = MT9M111_OUTFMT_BYPASS_IFP | MT9M111_OUTFMT_RGB;
		break;
	case MEDIA_BUS_FMT_RGB555_2X8_PADHI_LE:
		data_outfmt2 = MT9M111_OUTFMT_RGB | MT9M111_OUTFMT_RGB555 |
			MT9M111_OUTFMT_SWAP_YCbCr_C_Y_RGB_EVEN;
		break;
	case MEDIA_BUS_FMT_RGB555_2X8_PADHI_BE:
		data_outfmt2 = MT9M111_OUTFMT_RGB | MT9M111_OUTFMT_RGB555;
		break;
	case MEDIA_BUS_FMT_RGB565_2X8_LE:
		data_outfmt2 = MT9M111_OUTFMT_RGB | MT9M111_OUTFMT_RGB565 |
			MT9M111_OUTFMT_SWAP_YCbCr_C_Y_RGB_EVEN;
		break;
	case MEDIA_BUS_FMT_RGB565_2X8_BE:
		data_outfmt2 = MT9M111_OUTFMT_RGB | MT9M111_OUTFMT_RGB565;
		break;
	case MEDIA_BUS_FMT_BGR565_2X8_BE:
		data_outfmt2 = MT9M111_OUTFMT_RGB | MT9M111_OUTFMT_RGB565 |
			MT9M111_OUTFMT_SWAP_YCbCr_Cb_Cr_RGB_R_B;
		break;
	case MEDIA_BUS_FMT_BGR565_2X8_LE:
		data_outfmt2 = MT9M111_OUTFMT_RGB | MT9M111_OUTFMT_RGB565 |
			MT9M111_OUTFMT_SWAP_YCbCr_C_Y_RGB_EVEN |
			MT9M111_OUTFMT_SWAP_YCbCr_Cb_Cr_RGB_R_B;
		break;
	case MEDIA_BUS_FMT_UYVY8_2X8:
		data_outfmt2 = 0;
		break;
	case MEDIA_BUS_FMT_VYUY8_2X8:
		data_outfmt2 = MT9M111_OUTFMT_SWAP_YCbCr_Cb_Cr_RGB_R_B;
		break;
	case MEDIA_BUS_FMT_YUYV8_2X8:
		data_outfmt2 = MT9M111_OUTFMT_SWAP_YCbCr_C_Y_RGB_EVEN;
		break;
	case MEDIA_BUS_FMT_YVYU8_2X8:
		data_outfmt2 = MT9M111_OUTFMT_SWAP_YCbCr_C_Y_RGB_EVEN |
			MT9M111_OUTFMT_SWAP_YCbCr_Cb_Cr_RGB_R_B;
		break;
	default:
		dev_err(&client->dev, "Pixel format not handled: %x\n", code);
		return -EINVAL;
	}

	if (mt9m111->invert_pixclk)
		data_outfmt2 |= MT9M111_OUTFMT_INV_PIX_CLOCK;

	ret = mt9m111_reg_mask(client, context_a.output_fmt_ctrl2,
			       data_outfmt2, mask_outfmt2);
	if (!ret)
		ret = mt9m111_reg_mask(client, context_b.output_fmt_ctrl2,
				       data_outfmt2, mask_outfmt2);

	return ret;
}

#ifdef CONFIG_VIDEO_ADV_DEBUG
static int mt9m111_g_register(struct v4l2_subdev *sd,
			      struct v4l2_dbg_register *reg)
{
	struct mt9m111 *mt9m111 = container_of(sd, struct mt9m111, subdev);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int rc;
	int val;

	if (reg->reg > 0x2ff)
		return -EINVAL;

	rc = mt9m111_get_device(mt9m111);
	if (rc < 0)
		return rc;

	val = mt9m111_reg_read(client, reg->reg);
	reg->size = 2;
	reg->val = (u64)val;

	if (reg->val > 0xffff)
		rc = -EIO;
	else
		rc = 0;

	mt9m111_put_device(mt9m111);

	return rc;
}

static int mt9m111_s_register(struct v4l2_subdev *sd,
			      const struct v4l2_dbg_register *reg)
{
	struct mt9m111 *mt9m111 = container_of(sd, struct mt9m111, subdev);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int rc;

	if (reg->reg > 0x2ff)
		return -EINVAL;

	rc = mt9m111_get_device(mt9m111);
	if (rc < 0)
		return rc;

	rc = mt9m111_reg_write(client, reg->reg, reg->val);

	mt9m111_put_device(mt9m111);

	return rc;
}
#endif

static int mt9m111_set_flip(struct mt9m111 *mt9m111, int flip, int mask)
{
	struct i2c_client *client = v4l2_get_subdevdata(&mt9m111->subdev);
	int ret;

	if (flip)
		ret = mt9m111_reg_set(client, mt9m111->ctx->read_mode, mask);
	else
		ret = mt9m111_reg_clear(client, mt9m111->ctx->read_mode, mask);

	return ret;
}

static int mt9m111_get_global_gain(struct mt9m111 *mt9m111)
{
	struct i2c_client *client = v4l2_get_subdevdata(&mt9m111->subdev);
	int data;

	data = reg_read(GLOBAL_GAIN);
	if (data >= 0)
		return (data & 0x2f) * (1 << ((data >> 10) & 1)) *
			(1 << ((data >> 9) & 1));
	return data;
}

static int mt9m111_set_global_gain(struct mt9m111 *mt9m111, int gain)
{
	struct i2c_client *client = v4l2_get_subdevdata(&mt9m111->subdev);
	u16 val;

	if (gain > 63 * 2 * 2)
		return -EINVAL;

	if ((gain >= 64 * 2) && (gain < 63 * 2 * 2))
		val = (1 << 10) | (1 << 9) | (gain / 4);
	else if ((gain >= 64) && (gain < 64 * 2))
		val = (1 << 9) | (gain / 2);
	else
		val = gain;

	return reg_write(GLOBAL_GAIN, val);
}

static int mt9m111_set_autoexposure(struct mt9m111 *mt9m111, int val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&mt9m111->subdev);

	if (val == V4L2_EXPOSURE_AUTO)
		return reg_set(OPER_MODE_CTRL, MT9M111_OPMODE_AUTOEXPO_EN);
	return reg_clear(OPER_MODE_CTRL, MT9M111_OPMODE_AUTOEXPO_EN);
}

static int mt9m111_set_autowhitebalance(struct mt9m111 *mt9m111, int on)
{
	struct i2c_client *client = v4l2_get_subdevdata(&mt9m111->subdev);

	if (on)
		return reg_set(OPER_MODE_CTRL, MT9M111_OPMODE_AUTOWHITEBAL_EN);
	return reg_clear(OPER_MODE_CTRL, MT9M111_OPMODE_AUTOWHITEBAL_EN);
}

static int mt9m111_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mt9m111 *mt9m111 = container_of(ctrl->handler,
					       struct mt9m111, hdl);

	switch (ctrl->id) {
	case V4L2_CID_VFLIP:
		return mt9m111_set_flip(mt9m111, ctrl->val,
					MT9M111_RMB_MIRROR_ROWS);
	case V4L2_CID_HFLIP:
		return mt9m111_set_flip(mt9m111, ctrl->val,
					MT9M111_RMB_MIRROR_COLS);
	case V4L2_CID_GAIN:
		return mt9m111_set_global_gain(mt9m111, ctrl->val);
	case V4L2_CID_EXPOSURE_AUTO:
		return mt9m111_set_autoexposure(mt9m111, ctrl->val);
	case V4L2_CID_AUTO_WHITE_BALANCE:
		return mt9m111_set_autowhitebalance(mt9m111, ctrl->val);

	case V4L2_CID_SKIP_X:
	case V4L2_CID_SKIP_Y:
		if (mt9m111->is_streaming)
			return -EBUSY;

		if (ctrl->id == V4L2_CID_SKIP_X)
			mt9m111->skip_x = ctrl->val;
		else
			mt9m111->skip_y = ctrl->val;

		mt9m111->dirty_dim = true;

		return _mt9m111_set_selection(mt9m111, &mt9m111->rect,
					      mt9m111->fmt,
					      mt9m111->width, mt9m111->height);

	case V4L2_CID_X_PIXEL_RATE:
		if (mt9m111->clk) {
			v4l2_clk_set_rate(mt9m111->clk, ctrl->val);
			ctrl->val = v4l2_clk_get_rate(mt9m111->clk);
		}

		return 0;
	}

	return -EINVAL;
}

static int _mt9m111_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mt9m111 *mt9m111 = container_of(ctrl->handler,
					       struct mt9m111, hdl);
	int	rc;

	rc = mt9m111_get_device(mt9m111);
	if (rc < 0)
		return rc;

	rc = mt9m111_s_ctrl(ctrl);

	mt9m111_put_device(mt9m111);

	return rc;
}

static int mt9m111_suspend(struct mt9m111 *mt9m111)
{
	struct i2c_client *client = v4l2_get_subdevdata(&mt9m111->subdev);
	int ret;

	v4l2_ctrl_s_ctrl(mt9m111->gain, mt9m111_get_global_gain(mt9m111));

	ret = reg_set(RESET, MT9M111_RESET_RESET_MODE);
	if (!ret)
		ret = reg_set(RESET, MT9M111_RESET_RESET_SOC |
			      MT9M111_RESET_OUTPUT_DISABLE |
			      MT9M111_RESET_ANALOG_STANDBY);
	if (!ret)
		ret = reg_clear(RESET, MT9M111_RESET_CHIP_ENABLE);

	return ret;
}

static void mt9m111_restore_state(struct mt9m111 *mt9m111)
{
	mt9m111->dirty_dim = true;

	mt9m111_set_context(mt9m111, mt9m111->ctx);
	mt9m111_set_pixfmt(mt9m111, mt9m111->fmt->code);
	_mt9m111_set_selection(mt9m111, &mt9m111->rect,
			       mt9m111->fmt,
			       mt9m111->width, mt9m111->height);
	v4l2_ctrl_handler_setup(&mt9m111->hdl);
}

static int mt9m111_resume(struct mt9m111 *mt9m111)
{
	int ret = mt9m111_enable(mt9m111);
	if (!ret)
		ret = mt9m111_reset(mt9m111);
	if (!ret)
		mt9m111_restore_state(mt9m111);

	return ret;
}

static int mt9m111_init(struct mt9m111 *mt9m111)
{
	struct i2c_client *client = v4l2_get_subdevdata(&mt9m111->subdev);
	int ret;

	ret = mt9m111_enable(mt9m111);
	if (!ret)
		ret = mt9m111_reset(mt9m111);
	if (!ret)
		ret = mt9m111_set_context(mt9m111, mt9m111->ctx);
	if (ret)
		dev_err(&client->dev, "mt9m111 init failed: %d\n", ret);
	return ret;
}

static int mt9m111_power_on(struct mt9m111 *mt9m111)
{
	struct i2c_client *client = v4l2_get_subdevdata(&mt9m111->subdev);
	struct soc_camera_subdev_desc *ssdd = soc_camera_i2c_to_desc(client);
	int ret;

	ret = soc_camera_power_on(&client->dev, ssdd, mt9m111->clk);
	if (ret < 0)
		return ret;

	ret = mt9m111_resume(mt9m111);
	if (ret < 0) {
		dev_err(&client->dev, "Failed to resume the sensor: %d\n", ret);
		soc_camera_power_off(&client->dev, ssdd, mt9m111->clk);
	}

	return ret;
}

static void mt9m111_power_off(struct mt9m111 *mt9m111)
{
	struct i2c_client *client = v4l2_get_subdevdata(&mt9m111->subdev);
	struct soc_camera_subdev_desc *ssdd = soc_camera_i2c_to_desc(client);

	mt9m111_suspend(mt9m111);
	soc_camera_power_off(&client->dev, ssdd, mt9m111->clk);
}

static int mt9m111_s_power(struct v4l2_subdev *sd, int on)
{
	struct mt9m111 *mt9m111 = container_of(sd, struct mt9m111, subdev);
	int ret = 0;

	ret = mt9m111_get_device(mt9m111);
	if (ret < 0)
		return ret;

	mutex_lock(&mt9m111->power_lock);

	/*
	 * If the power count is modified from 0 to != 0 or from != 0 to 0,
	 * update the power state.
	 */
	if (mt9m111->power_count == !on) {
		if (on)
			ret = mt9m111_power_on(mt9m111);
		else
			mt9m111_power_off(mt9m111);
	}

	if (!ret) {
		/* Update the power count. */
		mt9m111->power_count += on ? 1 : -1;
		WARN_ON(mt9m111->power_count < 0);
	}

	mutex_unlock(&mt9m111->power_lock);

	mt9m111_put_device(mt9m111);

	return ret;
}

static int mt9m111_querycap(struct mt9m111 *mt9m111,
			    struct v4l2_capability *cap)
{
	strcpy(cap->driver, "mt9m111");

	return 0;
}

static long mt9m111_core_ioctl(struct v4l2_subdev *sd,
			       unsigned int cmd, void *arg)
{
	struct mt9m111 *mt9m111 = container_of(sd, struct mt9m111, subdev);

	switch (cmd) {
	case VIDIOC_QUERYCAP:
		return mt9m111_querycap(mt9m111, arg);

	default:
		return -ENOTTY;
	}
}

static const struct v4l2_ctrl_ops mt9m111_ctrl_ops = {
	.s_ctrl = _mt9m111_s_ctrl,
};

static struct v4l2_subdev_core_ops mt9m111_subdev_core_ops = {
	.s_power	= mt9m111_s_power,
	.ioctl		= mt9m111_core_ioctl,

#ifdef CONFIG_VIDEO_ADV_DEBUG
	.g_register	= mt9m111_g_register,
	.s_register	= mt9m111_s_register,
#endif
};

static struct mt9m111_datafmt const *mt9m111_fmt_by_idx(
	struct mt9m111 const *mt9m111, size_t idx)
{
	size_t	cnt;

	cnt = ARRAY_SIZE(mt9m111_colour_fmts);
	if (idx < cnt)
		return &mt9m111_colour_fmts[idx];
	idx -= cnt;

	if (mt9m111->allow_10bit) {
		cnt = ARRAY_SIZE(mt9m111_10bit_fmts);
		if (idx < cnt)
			return &mt9m111_10bit_fmts[idx];
		idx -= cnt;
	}

	if (mt9m111->allow_burst) {
		cnt = ARRAY_SIZE(mt9m111_processed_fmts);
		if (idx < cnt)
			return &mt9m111_processed_fmts[idx];
		idx -= cnt;
	}

	return NULL;
}

static int mt9m111_enum_mbus_code(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_mbus_code_enum *code)
{
	struct mt9m111		*mt9m111 =
		container_of(sd, struct mt9m111, subdev);
	struct mt9m111_datafmt const *fmt =
		mt9m111_fmt_by_idx(mt9m111, code->index);

	if (!fmt)
		return -EINVAL;

	code->code = fmt->code;
	return 0;
}

static int mt9m111_g_mbus_config(struct v4l2_subdev *sd,
				struct v4l2_mbus_config *cfg)
{
	struct mt9m111 *mt9m111 = container_of(sd, struct mt9m111, subdev);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct soc_camera_subdev_desc *ssdd = soc_camera_i2c_to_desc(client);

	cfg->flags = V4L2_MBUS_MASTER |
		V4L2_MBUS_HSYNC_ACTIVE_HIGH | V4L2_MBUS_VSYNC_ACTIVE_HIGH |
		V4L2_MBUS_DATA_ACTIVE_HIGH;
	cfg->flags |= (mt9m111->invert_pixclk ?
		       V4L2_MBUS_PCLK_SAMPLE_RISING :
		       V4L2_MBUS_PCLK_SAMPLE_FALLING);
	cfg->type = V4L2_MBUS_PARALLEL;
	cfg->flags = soc_camera_apply_board_flags(ssdd, cfg);

	return 0;
}

static int mt9m111_s_stream_on(struct mt9m111 *mt9m111)
{
	struct i2c_client	*client = v4l2_get_subdevdata(&mt9m111->subdev);
	int			rc;

	rc = mt9m111_get_device(mt9m111);
	if (rc < 0)
		return rc;

	rc = mt9m111_pinctrl_state(mt9m111, PIN_STATE_ACTIVE);
	if (rc < 0) {
		dev_err(&client->dev, "failed to set pins to active: %d\n", rc);
		goto out;
	}

	mt9m111->is_streaming = true;
	rc = 0;

out:
	if (rc < 0)
		mt9m111_put_device(mt9m111);

	return rc;
}

static int mt9m111_s_stream_off(struct mt9m111 *mt9m111)
{
	struct i2c_client	*client = v4l2_get_subdevdata(&mt9m111->subdev);
	int			rc;

	rc = mt9m111_pinctrl_state(mt9m111, PIN_STATE_IDLE);
	if (rc < 0) {
		dev_warn(&client->dev, "failed to set pins to idle: %d\n", rc);
		/* ignore error */
	}

	mt9m111_put_device(mt9m111);

	mt9m111->is_streaming = false;

	return 0;
}

static int mt9m111_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct mt9m111 *mt9m111 = container_of(sd, struct mt9m111, subdev);

	/* TODO: is 'is_streaming' protected by locks in the upper layers? */

	if (enable && mt9m111->is_streaming)
		return -EBUSY;
	else if (!enable && !mt9m111->is_streaming)
		return -EINVAL;
	else if (enable)
		return mt9m111_s_stream_on(mt9m111);
	else
		return mt9m111_s_stream_off(mt9m111);
}

static struct v4l2_subdev_video_ops mt9m111_subdev_video_ops = {
	.g_mbus_config	= mt9m111_g_mbus_config,
	.s_stream	= mt9m111_s_stream,
};

static int mt9m111_pad_enum_frame_size(struct v4l2_subdev *sd,
				       struct v4l2_subdev_pad_config *cfg,
				       struct v4l2_subdev_frame_size_enum *fse)
{
	struct mt9m111 *mt9m111 = container_of(sd, struct mt9m111, subdev);
	struct mt9m111_datafmt const *fmt =
		mt9m111_fmt_by_idx(mt9m111, fse->index);

	if (fse->pad != 0 || !fmt)
		return -EINVAL;

	fse->min_width = 2;
	fse->max_width = 1280 >> mt9m111->skip_x;
	fse->min_height = 2;
	fse->max_height = 1024 >> mt9m111->skip_y;

	return 0;
}

static struct v4l2_rect *
_mt9m111_get_pad_crop(struct mt9m111 *sensor,
		      struct v4l2_subdev_pad_config *cfg,
		      unsigned int pad,
		      enum v4l2_subdev_format_whence which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_crop(&sensor->subdev, cfg, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &sensor->rect;
	default:
		return NULL;
	}
}

static int mt9m111_get_selection(struct v4l2_subdev *subdev,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_selection *sel)
{
	struct mt9m111 *mt9m111 = container_of(subdev, struct mt9m111, subdev);
	int			rc;
	struct v4l2_rect	*r;

	if (sel->pad != 0)
		return -EINVAL;

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r = (struct v4l2_rect){
			.left	= 30,
			.width	= 1280,
			.top	= 12,
			.height = 1024,
		};
		rc = 0;
		break;

	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r = (struct v4l2_rect){
			.left	= 0,
			.width	= 1316,
			.top	= 0,
			.height = 1048,
		};
		rc = 0;
		break;

	case V4L2_SEL_TGT_CROP:
		r = _mt9m111_get_pad_crop(mt9m111, cfg, sel->pad, sel->which);
		if (!r) {
			rc = -EINVAL;
			break;
		}

		sel->r = *r;
		rc = 0;
		break;

	default:
		rc = -EINVAL;
		break;
	}

	return rc;
}

static void mt9m111_clamp(unsigned int *pos, unsigned int *len,
			  unsigned int min_pos, unsigned  int max_len,
			  unsigned int alignment)
{
	unsigned int	p = *pos;
	unsigned int	l = *len;

	l = clamp_val(l, 0, max_len);
	l = ALIGN(l, alignment);

	p = clamp_val(p, min_pos, min_pos + max_len - l);

	*len = l;
	*pos = p;
}

static int _mt9m111_try_selection(struct mt9m111 *mt9m111, struct v4l2_rect *r,
				  unsigned int *width, unsigned int *height,
				  struct mt9m111_datafmt const *fmt)
{
	bool			allow_scaling;
	struct i2c_client	*client = v4l2_get_subdevdata(&mt9m111->subdev);

	dev_dbg(&client->dev, "%s([%ux%u+%ux%u], %dx%d, %04x\n", __func__,
		r->left, r->top, r->width, r->height,
		width ? (int)*width : -1,
		height ? (int)*height : -1,
		fmt->code);

	if (!fmt || (width && *width == 0) || (height && *height == 0))
		return -EINVAL;

	allow_scaling = mt9m111->allow_burst && !fmt->bypass_ifp;

	mt9m111_clamp(&r->left, &r->width,
		      MT9M111_MIN_DARK_COLS, MT9M111_MAX_WIDTH,
		      (fmt->is_bayer ? 2 : 1) << mt9m111->skip_x);

	if (!allow_scaling && width)
		*width = r->width;

	mt9m111_clamp(&r->top, &r->height,
		      MT9M111_MIN_DARK_ROWS, MT9M111_MAX_HEIGHT,
		      (fmt->is_bayer ? 2 : 1) << mt9m111->skip_y);

	if (!allow_scaling && height)
		*height  = r->height;

	dev_dbg(&client->dev, "--> ([%ux%u+%ux%u], %dx%d, %04x\n",
		r->left, r->top, r->width, r->height,
		width ? (int)*width : -1,
		height ? (int)*height : -1,
		fmt->code);

	return 0;
}

static int mt9m111_set_selection(struct v4l2_subdev *subdev,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_selection *sel)
{
	struct mt9m111 *mt9m111 = container_of(subdev, struct mt9m111, subdev);
	int			rc;
	struct v4l2_rect	r = sel->r;

	if (sel->pad != 0)
		return -EINVAL;

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		/* readonly properties; can not be set */
		rc = -EINVAL;
		break;

	case V4L2_SEL_TGT_CROP:
		rc = _mt9m111_try_selection(mt9m111, &r, NULL, NULL,
					    mt9m111->fmt);
		if (rc < 0)
			break;

		if (mt9m111->is_streaming &&
		    (r.width != mt9m111->rect.width ||
		     r.height != mt9m111->rect.height)) {
			/* forbid change of output dimension when streaming is
			 * active */
			rc = -EBUSY;
		} else if (sel->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
			rc = _mt9m111_set_selection(mt9m111, &r, mt9m111->fmt,
						    mt9m111->width,
						    mt9m111->height);
		} else {
			*v4l2_subdev_get_try_crop(subdev, cfg, sel->pad) = r;
			rc = 0;
		}

		break;

	default:
		rc = -EINVAL;
		break;
	}

	return rc;
}

static int mt9m111_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_format *format)
{
	struct mt9m111 *mt9m111 = container_of(sd, struct mt9m111, subdev);
	struct v4l2_mbus_framefmt	*mf = &format->format;
	struct mt9m111_datafmt const	*fmt;
	struct v4l2_rect		r = mt9m111->rect;
	int				rc;
	/* we are working with unscaled values internally */
	unsigned int			width = mf->width << mt9m111->skip_x;
	unsigned int			height = mf->height << mt9m111->skip_y;

	if (format->pad != 0)
		return -EINVAL;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE && mt9m111->is_streaming)
		return -EBUSY;

	fmt = mt9m111_find_datafmt(mt9m111, mf->code);

	rc = _mt9m111_try_selection(mt9m111, &r, &width, &height, fmt);
	if (rc < 0)
		return rc;

	mf->width  = width >> mt9m111->skip_x;
	mf->height = height >> mt9m111->skip_y;
	mf->code   = fmt->code;

	if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
		cfg->try_fmt = *mf;
		return 0;
	}

	rc = mt9m111_get_device(mt9m111);
	if (rc < 0)
		return rc;

	rc = _mt9m111_set_selection(mt9m111, &r, fmt, width, height);
	if (rc < 0)
		goto out;

	rc = mt9m111_set_pixfmt(mt9m111, fmt->code);
	if (rc < 0)
		goto out;

	mt9m111->fmt = fmt;
	rc = 0;

out:
	mt9m111_put_device(mt9m111);

	return rc;
}

static const struct v4l2_subdev_pad_ops mt9m111_subdev_pad_ops = {
	.enum_mbus_code = mt9m111_enum_mbus_code,
	.enum_frame_size= mt9m111_pad_enum_frame_size,
	.get_fmt	= mt9m111_get_fmt,
	.set_fmt	= mt9m111_set_fmt,
	.get_selection	= mt9m111_get_selection,
	.set_selection	= mt9m111_set_selection,
};

static struct v4l2_subdev_ops mt9m111_subdev_ops = {
	.core	= &mt9m111_subdev_core_ops,
	.video	= &mt9m111_subdev_video_ops,
	.pad	= &mt9m111_subdev_pad_ops,
};

/*
 * Interface active, can use i2c. If it fails, it can indeed mean, that
 * this wasn't our capture interface, so, we wait for the right one
 */
static int mt9m111_video_probe(struct i2c_client *client)
{
	struct mt9m111 *mt9m111 = to_mt9m111(client);
	s32 data;
	int ret;

	ret = mt9m111_get_device(mt9m111);
	if (ret < 0)
		return ret;

	ret = mt9m111_s_power(&mt9m111->subdev, 1);
	if (ret < 0)
		goto out;

	data = reg_read(CHIP_VERSION);

	switch (data) {
	case 0x143a: /* MT9M111 or MT9M131 */
		dev_info(&client->dev,
			"Detected a MT9M111/MT9M131 chip ID %x\n", data);
		break;
	case 0x148c: /* MT9M112 */
		dev_info(&client->dev, "Detected a MT9M112 chip ID %x\n", data);
		break;
	default:
		dev_err(&client->dev,
			"No MT9M111/MT9M112/MT9M131 chip detected register read %x\n",
			data);
		ret = -ENODEV;
		goto done;
	}

	ret = mt9m111_init(mt9m111);
	if (ret)
		goto done;

	ret = v4l2_ctrl_handler_setup(&mt9m111->hdl);

done:
	mt9m111_s_power(&mt9m111->subdev, 0);

out:
	mt9m111_put_device(mt9m111);

	return ret;
}

static int mt9m111_init_pinctrl(struct mt9m111 *mt9m111, struct device *dev)
{
	static struct {
		enum mt9m111_pin_state		state;
		char const			*name;
	} const		STATES[] = {
		{ PIN_STATE_ACTIVE,  PINCTRL_STATE_DEFAULT },
		{ PIN_STATE_IDLE,    PINCTRL_STATE_IDLE },
		{ PIN_STATE_SLEEP,   PINCTRL_STATE_SLEEP },
	};

	int		rc;
	size_t		i;
	struct pinctrl			*pinctrl;
	struct pinctrl_state		*pin_st[3];

	if (!IS_ENABLED(CONFIG_PINCTRL))
		return 0;

	pinctrl = devm_pinctrl_get(dev);
	rc = PTR_ERR_OR_ZERO(pinctrl);
	if (rc < 0) {
		if (rc != -EPROBE_DEFER)
			dev_err(dev, "failed to get pinctl: %d\n", rc);
		mt9m111->pinctrl = NULL;
		goto out;
	}

	if (!pinctrl)
		return 0;

	for (i = 0; i < ARRAY_SIZE(STATES); ++i) {
		enum mt9m111_pin_state		st = STATES[i].state;
		char const			*name = STATES[i].name;
		struct pinctrl_state		*pst;

		/* order is important for now because we use the previous
		 * state when actual one is not available */
		BUG_ON(st != i);

		pst = pinctrl_lookup_state(pinctrl, name);
		rc = PTR_ERR_OR_ZERO(pst);
		if (rc == -ENODEV) {
			/* see BUG_ON above! */
			pst = i == 0 ? NULL : pin_st[i-1];
		} else if (rc < 0) {
			dev_err(dev,
				"failed to get '%s' pinctl state: %d\n",
				name, rc);
			goto out;
		}
		pin_st[i] = pst;
	}

#ifdef CONFIG_PINCTRL
	BUILD_BUG_ON(sizeof mt9m111->pin_st != sizeof pin_st);

	mt9m111->pinctrl = pinctrl;
	memcpy(mt9m111->pin_st, pin_st, sizeof pin_st);
#endif

	rc = 0;

out:
	return rc;
}

static char const * const	mt9m111_menu_skip[] = {
	"1x",
	"2x",
	"4x",
	"8x",
};

static struct v4l2_ctrl_config const	mt9m111_ctrls[] = {
	{
		.ops		= &mt9m111_ctrl_ops,
		.id		= V4L2_CID_SKIP_X,
		.type		= V4L2_CTRL_TYPE_MENU,
		.name		= "skip-x",
		.min		= 0,
		.max		= ARRAY_SIZE(mt9m111_menu_skip) - 1,
		.qmenu		= mt9m111_menu_skip,
	}, {
		.ops		= &mt9m111_ctrl_ops,
		.id		= V4L2_CID_SKIP_Y,
		.type		= V4L2_CTRL_TYPE_MENU,
		.name		= "skip-y",
		.min		= 0,
		.max		= ARRAY_SIZE(mt9m111_menu_skip) - 1,
		.qmenu		= mt9m111_menu_skip,
	}, {
		.ops		= &mt9m111_ctrl_ops,
		.id		= V4L2_CID_X_PIXEL_RATE,
		.type		= V4L2_CTRL_TYPE_INTEGER,
		.name		= "X Pixel Rate",
		.min		=  2000000,
		.max		= 54000000,
		.def		= 27000000,
		.step		= 1,
	}
};

static int mt9m111_probe(struct i2c_client *client,
			 const struct i2c_device_id *did)
{
	struct mt9m111 *mt9m111;
	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);
	struct soc_camera_subdev_desc *ssdd = soc_camera_i2c_to_desc(client);
	size_t i;
	int ret;

	if (client->dev.of_node) {
		ssdd = devm_kzalloc(&client->dev, sizeof(*ssdd), GFP_KERNEL);
		if (!ssdd)
			return -ENOMEM;
		client->dev.platform_data = ssdd;
	}
	if (!ssdd) {
		dev_err(&client->dev, "mt9m111: driver needs platform data\n");
		return -EINVAL;
	}

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_WORD_DATA)) {
		dev_warn(&adapter->dev,
			 "I2C-Adapter doesn't support I2C_FUNC_SMBUS_WORD\n");
		return -EIO;
	}

	mt9m111 = devm_kzalloc(&client->dev, sizeof(struct mt9m111), GFP_KERNEL);
	if (!mt9m111)
		return -ENOMEM;

	mt9m111->clk = v4l2_clk_get(&client->dev, "mclk");
	if (IS_ERR(mt9m111->clk))
		return -EPROBE_DEFER;

	mutex_init(&mt9m111->dev_lock);

	if (mt9m111->clk)
		/* setup a valid initial rate */
		v4l2_clk_set_rate(mt9m111->clk, 27000000);

	/* Default HIGHPOWER context */
	mt9m111->ctx = &context_b;

	mt9m111->invert_pixclk = of_property_read_bool(client->dev.of_node,
						       "phytec,invert-pixclk");
	mt9m111->allow_10bit = of_property_read_bool(client->dev.of_node,
						     "phytec,allow-10bit");
	mt9m111->allow_burst = of_property_read_bool(client->dev.of_node,
						     "phytec,allow-burst");

	ret = mt9m111_init_pinctrl(mt9m111, &client->dev);
	if (ret < 0) {
		dev_warn(&client->dev,
			 "failed to inialize pinctrl; skipping it for now: %d\n",
			 ret);
	}

	v4l2_i2c_subdev_init(&mt9m111->subdev, client, &mt9m111_subdev_ops);
	v4l2_ctrl_handler_init(&mt9m111->hdl, ARRAY_SIZE(mt9m111_ctrls) + 5);
	v4l2_ctrl_new_std(&mt9m111->hdl, &mt9m111_ctrl_ops,
			V4L2_CID_VFLIP, 0, 1, 1, 0);
	v4l2_ctrl_new_std(&mt9m111->hdl, &mt9m111_ctrl_ops,
			V4L2_CID_HFLIP, 0, 1, 1, 0);
	v4l2_ctrl_new_std(&mt9m111->hdl, &mt9m111_ctrl_ops,
			V4L2_CID_AUTO_WHITE_BALANCE, 0, 1, 1, 1);
	mt9m111->gain = v4l2_ctrl_new_std(&mt9m111->hdl, &mt9m111_ctrl_ops,
			V4L2_CID_GAIN, 0, 63 * 2 * 2, 1, 32);
	v4l2_ctrl_new_std_menu(&mt9m111->hdl,
			&mt9m111_ctrl_ops, V4L2_CID_EXPOSURE_AUTO, 1, 0,
			V4L2_EXPOSURE_AUTO);

	for (i = 0; i < ARRAY_SIZE(mt9m111_ctrls); ++i)
		v4l2_ctrl_new_custom(&mt9m111->hdl, &mt9m111_ctrls[i], NULL);

	mt9m111->subdev.ctrl_handler = &mt9m111->hdl;
	if (mt9m111->hdl.error) {
		ret = mt9m111->hdl.error;
		goto out_clkput;
	}

	mt9m111->subdev.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	mt9m111->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_init(&mt9m111->subdev.entity, 1, &mt9m111->pad, 0);
	if (ret < 0)
		goto out_hdlfree;

	/* Second stage probe - when a capture adapter is there */
	mt9m111->rect.left	= MT9M111_MIN_DARK_COLS;
	mt9m111->rect.top	= MT9M111_MIN_DARK_ROWS;
	mt9m111->rect.width	= MT9M111_MAX_WIDTH;
	mt9m111->rect.height	= MT9M111_MAX_HEIGHT;
	mt9m111->width		= mt9m111->rect.width;
	mt9m111->height		= mt9m111->rect.height;
	mt9m111->fmt		= &mt9m111_colour_fmts[0];
	mt9m111->dirty_dim	= true;
	mt9m111->lastpage	= -1;
	mutex_init(&mt9m111->power_lock);

	ret = soc_camera_power_init(&client->dev, ssdd);
	if (ret < 0)
		goto out_hdlfree;

	ret = mt9m111_video_probe(client);
	if (ret < 0)
		goto out_hdlfree;

	mt9m111->subdev.dev = &client->dev;
	ret = v4l2_async_register_subdev(&mt9m111->subdev);
	if (ret < 0)
		goto out_hdlfree;

	return 0;

out_hdlfree:
	if (mt9m111->subdev.entity.links)
		media_entity_cleanup(&mt9m111->subdev.entity);

	v4l2_ctrl_handler_free(&mt9m111->hdl);
out_clkput:
	v4l2_clk_put(mt9m111->clk);

	WARN_ON(ret < 0 && mt9m111->ref_cnt > 0);

	return ret;
}

static int mt9m111_remove(struct i2c_client *client)
{
	struct mt9m111 *mt9m111 = to_mt9m111(client);
	bool have_dev;

	have_dev = mt9m111_get_device(mt9m111) == 0;

	v4l2_async_unregister_subdev(&mt9m111->subdev);
	v4l2_clk_put(mt9m111->clk);
	media_entity_cleanup(&mt9m111->subdev.entity);
	v4l2_ctrl_handler_free(&mt9m111->hdl);

	if (have_dev)
		mt9m111_put_device(mt9m111);

	WARN_ON(mt9m111->ref_cnt > 0);

	return 0;
}
static const struct of_device_id mt9m111_of_match[] = {
	{ .compatible = "micron,mt9m111", },
	{},
};
MODULE_DEVICE_TABLE(of, mt9m111_of_match);

static const struct i2c_device_id mt9m111_id[] = {
	{ "mt9m111", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mt9m111_id);

static struct i2c_driver mt9m111_i2c_driver = {
	.driver = {
		.name = "mt9m111",
		.of_match_table = of_match_ptr(mt9m111_of_match),
	},
	.probe		= mt9m111_probe,
	.remove		= mt9m111_remove,
	.id_table	= mt9m111_id,
};

module_i2c_driver(mt9m111_i2c_driver);

MODULE_DESCRIPTION("Micron/Aptina MT9M111/MT9M112/MT9M131 Camera driver");
MODULE_AUTHOR("Robert Jarzmik");
MODULE_LICENSE("GPL");
