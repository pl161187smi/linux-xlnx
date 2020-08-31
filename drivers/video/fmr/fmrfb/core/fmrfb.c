/* fmrfb.c */
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/console.h>
#include <linux/videodev2.h>
#include "fmrfb.h"

#define FMRFB_PSEUDO_PALETTE_SZ 256

#define LOGICVC_PIX_FMT_AYUV  v4l2_fourcc('A', 'Y', 'U', 'V')
#define LOGICVC_PIX_FMT_AVUY  v4l2_fourcc('A', 'V', 'U', 'Y')
#define LOGICVC_PIX_FMT_ALPHA v4l2_fourcc('A', '8', ' ', ' ')

#define OFST_DYNCLK_CTRL 	0x0
#define OFST_DYNCLK_STATUS 	0x4
#define OFST_DYNCLK_CLK_L 	0x8
#define OFST_DYNCLK_FB_L 	0x0C
#define OFST_DYNCLK_FB_H_CLK_H 	0x10
#define OFST_DYNCLK_DIV 	0x14 
#define OFST_DYNCLK_LOCK_L 	0x18
#define OFST_DYNCLK_FLTR_LOCK_H 0x1C
        
#define BIT_DYNCLK_START 	0
#define BIT_DYNCLK_RUNNING 	0

struct clkmode {
		double freq;
		u32 fb_mult;
		u32 clk_div;
		u32 main_div;
};

static struct fmrfb_vmode_data fmrfb_vmode = {
	.fb_vmode = {
		.refresh = 60,
		.xres = 1024,
		.yres = 768,
		.pixclock = KHZ2PICOS(65000),
		.left_margin = 160,
		.right_margin = 24,
		.upper_margin = 29,
		.lower_margin = 3,
		.hsync_len = 136,
		.vsync_len = 6,
		.vmode = FB_VMODE_NONINTERLACED
	},
	.fb_vmode_name = "1024x768"
};

static unsigned short logicvc_layer_reg_offset[] = {
	(LOGICVC_LAYER_BASE_OFFSET + LOGICVC_LAYER_0_OFFSET),
	(LOGICVC_LAYER_BASE_OFFSET + LOGICVC_LAYER_1_OFFSET),
	(LOGICVC_LAYER_BASE_OFFSET + LOGICVC_LAYER_2_OFFSET),
	(LOGICVC_LAYER_BASE_OFFSET + LOGICVC_LAYER_3_OFFSET),
	(LOGICVC_LAYER_BASE_OFFSET + LOGICVC_LAYER_4_OFFSET)
};

static unsigned short logicvc_clut_reg_offset[] = {
	(LOGICVC_CLUT_BASE_OFFSET + LOGICVC_CLUT_L0_CLUT_0_OFFSET),
	(LOGICVC_CLUT_BASE_OFFSET + LOGICVC_CLUT_L0_CLUT_1_OFFSET),
	(LOGICVC_CLUT_BASE_OFFSET + LOGICVC_CLUT_L1_CLUT_0_OFFSET),
	(LOGICVC_CLUT_BASE_OFFSET + LOGICVC_CLUT_L1_CLUT_1_OFFSET),
	(LOGICVC_CLUT_BASE_OFFSET + LOGICVC_CLUT_L2_CLUT_0_OFFSET),
	(LOGICVC_CLUT_BASE_OFFSET + LOGICVC_CLUT_L2_CLUT_1_OFFSET),
	(LOGICVC_CLUT_BASE_OFFSET + LOGICVC_CLUT_L3_CLUT_0_OFFSET),
	(LOGICVC_CLUT_BASE_OFFSET + LOGICVC_CLUT_L3_CLUT_1_OFFSET),
	(LOGICVC_CLUT_BASE_OFFSET + LOGICVC_CLUT_L4_CLUT_0_OFFSET),
	(LOGICVC_CLUT_BASE_OFFSET + LOGICVC_CLUT_L4_CLUT_1_OFFSET)
};

static char *fmrfb_mode_option;

/* Function declarations */
static int fmrfb_set_timings(struct fb_info *fbi, int bpp);
static void fmrfb_logicvc_disp_ctrl(struct fb_info *fbi, bool enable);
static void fmrfb_enable_logicvc_output(struct fb_info *fbi);
static void fmrfb_disable_logicvc_output(struct fb_info *fbi);
static void fmrfb_enable_logicvc_layer(struct fb_info *fbi);
static void fmrfb_disable_logicvc_layer(struct fb_info *fbi);
static void fmrfb_fbi_update(struct fb_info *fbi);

/******************************************************************************/

static u32 fmrfb_get_reg(void *base_virt, unsigned long offset,
	struct fmrfb_layer_data *ld)
{
	return readl(base_virt + offset);
}

static void fmrfb_set_reg(u32 value,
	void *base_virt, unsigned long offset,
	struct fmrfb_layer_data *ld)
{
	writel(value, (base_virt + offset));
}

static unsigned long fmrfb_get_reg_mem_addr(
	void *base_virt, unsigned long offset,
	struct fmrfb_layer_data *ld)
{
	unsigned long ordinal = offset >> 3;

	if ((unsigned long)base_virt - (unsigned long)ld->reg_base_virt) {
		return (unsigned long)(&ld->layer_reg_list->hpos_reg) +
			(ordinal * sizeof(unsigned long));
	} else {
		return (unsigned long)(&ld->fmrfb_cd->reg_list->dtype_reg) +
			(ordinal * sizeof(unsigned long));
	}
}

static u32 fmrfb_get_reg_mem(
	void *base_virt, unsigned long offset,
	struct fmrfb_layer_data *ld)
{
	return *((unsigned long *)fmrfb_get_reg_mem_addr(
		base_virt, offset, ld));
}

static void fmrfb_set_reg_mem(u32 value,
	void *base_virt, unsigned long offset,
	struct fmrfb_layer_data *ld)
{
	unsigned long *reg_mem_addr =
		(unsigned long *)fmrfb_get_reg_mem_addr(
			base_virt, offset, ld);
	*reg_mem_addr = value;
	writel((*reg_mem_addr), (base_virt + offset));
}

/******************************************************************************/

static irqreturn_t fmrfb_isr(int irq, void *dev_id)
{
	struct fb_info **afbi = dev_get_drvdata(dev_id);
	struct fb_info *fbi = afbi[0];
	struct fmrfb_layer_data *ld = fbi->par;
	struct fmrfb_common_data *cd = ld->fmrfb_cd;
	u32 isr;

	driver_devel("%s IRQ %d\n", __func__, irq);

	isr = readl(ld->reg_base_virt + LOGICVC_INT_STAT_ROFF);
	if (isr & LOGICVC_V_SYNC_INT) {
		writel(LOGICVC_V_SYNC_INT,
			ld->reg_base_virt + LOGICVC_INT_STAT_ROFF);
		cd->vsync.cnt++;
		wake_up_interruptible(&cd->vsync.wait);
		return IRQ_HANDLED;
	} else {
		return IRQ_NONE;
	}
}

/******************************************************************************/

static int fmrfb_open(struct fb_info *fbi, int user)
{
	struct fmrfb_layer_data *ld = fbi->par;

	driver_devel("%s\n", __func__);

	if (ld->layer_use_ref == 0) {
		/* turn on layer */
		fmrfb_enable_logicvc_layer(fbi);
	}
	ld->layer_use_ref++;
	ld->fmrfb_cd->fmrfb_use_ref++;

	return 0;
}

static int fmrfb_release(struct fb_info *fbi, int user)
{
	struct fmrfb_layer_data *ld = fbi->par;

	driver_devel("%s\n", __func__);

	ld->layer_use_ref--;
	if (ld->layer_use_ref == 0) {
		/* turn off layer */
		fmrfb_disable_logicvc_layer(fbi);
	}
	ld->fmrfb_cd->fmrfb_use_ref--;

	return 0;
}

/******************************************************************************/

static int fmrfb_check_var(struct fb_var_screeninfo *var,
	struct fb_info *fbi)
{
	struct fmrfb_layer_data *ld = fbi->par;
	struct fmrfb_layer_fix_data *lfdata = &ld->layer_fix;

	driver_devel("%s\n", __func__);

	if (var->xres < LOGICVC_MIN_XRES)
		var->xres = LOGICVC_MIN_XRES;
	if (var->xres > LOGICVC_MAX_XRES)
		var->xres = LOGICVC_MAX_XRES;
	if (var->yres < LOGICVC_MIN_VRES)
		var->yres = LOGICVC_MIN_VRES;
	if (var->yres > LOGICVC_MAX_VRES)
		var->yres = LOGICVC_MAX_VRES;

	if (var->xres_virtual < var->xres)
		var->xres_virtual = var->xres;
	if (var->xres_virtual > lfdata->width)
		var->xres_virtual = lfdata->width;
	if (var->yres_virtual < var->yres)
		var->yres_virtual = var->yres;
	if (var->yres_virtual > lfdata->height)
		var->yres_virtual = lfdata->height;

	if ((var->xoffset + var->xres) >= var->xres_virtual)
		var->xoffset = var->xres_virtual - var->xres - 1;
	if ((var->yoffset + var->yres) >= var->yres_virtual)
		var->yoffset = var->yres_virtual - var->yres - 1;

	if (var->bits_per_pixel != fbi->var.bits_per_pixel) {
		if (var->bits_per_pixel == 24)
			var->bits_per_pixel = 32;
		else
			var->bits_per_pixel = fbi->var.bits_per_pixel;
	}

	var->grayscale = fbi->var.grayscale;

	var->transp.offset = fbi->var.transp.offset;
	var->transp.length = fbi->var.transp.length;
	var->transp.msb_right = fbi->var.transp.msb_right;
	var->red.offset = fbi->var.red.offset;
	var->red.length = fbi->var.red.length;
	var->red.msb_right = fbi->var.red.msb_right;
	var->green.offset = fbi->var.green.offset;
	var->green.length = fbi->var.green.length;
	var->green.msb_right = fbi->var.green.msb_right;
	var->blue.offset = fbi->var.blue.offset;
	var->blue.length = fbi->var.blue.length;
	var->blue.msb_right = fbi->var.blue.msb_right;
	var->height = fbi->var.height;
	var->width = fbi->var.width;
	var->sync = fbi->var.sync;
	var->rotate = fbi->var.rotate;

	return 0;
}

static int fmrfb_set_par(struct fb_info *fbi)
{
	struct fb_info **afbi = NULL;
	struct fmrfb_layer_data *ld = fbi->par;
	struct fmrfb_common_data *cd = ld->fmrfb_cd;
	int rc = 0;
	int i;
	char vmode_opt[VMODE_NAME_SZ];
	bool resolution_change, layer_on[LOGICVC_MAX_LAYERS];

	driver_devel("%s\n", __func__);

	#if 1
	if (cd->fmrfb_flags & FMRFB_FLAG_VMODE_SET)
		return 0;

	if (!(cd->fmrfb_flags & FMRFB_FLAG_EDID_VMODE) &&
		((fbi->var.xres ==
			cd->vmode_data_current.fb_vmode.xres) ||
		(fbi->var.yres ==
			cd->vmode_data_current.fb_vmode.yres))) {
		resolution_change = false;
	} else {
		resolution_change = true;
	}

	if (resolution_change ||
		(cd->fmrfb_flags & FMRFB_FLAG_VMODE_INIT)) {

		if (!(cd->fmrfb_flags & FMRFB_FLAG_VMODE_INIT)) {
			struct fmrfb_layer_data *ld;
			/* store id's of enabled layers */
			afbi = dev_get_drvdata(fbi->device);
			for (i = 0; i < cd->fmrfb_layers; i++) {
				ld = afbi[i]->par;
				if (ld->layer_ctrl_flags & LOGICVC_LAYER_ON)
					layer_on[i] = true;
				else
					layer_on[i] = false;
			}
		}

		fmrfb_disable_logicvc_output(fbi);
		fmrfb_logicvc_disp_ctrl(fbi, false);

		if (!(cd->fmrfb_flags & FMRFB_FLAG_VMODE_INIT)) {
			/* we want 60Hz refresh rate */
			cd->vmode_data_current.fb_vmode.refresh = 60;
			sprintf(vmode_opt, "%dx%d%s-%d@%d%s",
				fbi->var.xres, fbi->var.yres,
				cd->vmode_data_current.fb_vmode_opts_cvt,
				fbi->var.bits_per_pixel,
				cd->vmode_data_current.fb_vmode.refresh,
				cd->vmode_data_current.fb_vmode_opts_ext);
			if (!strcmp(cd->vmode_data.fb_vmode_name, vmode_opt)) {
				cd->vmode_data_current = cd->vmode_data;
			} else {
				fmrfb_mode_option = vmode_opt;
				rc = fmrfb_set_timings(
					fbi, fbi->var.bits_per_pixel);
				fmrfb_mode_option = NULL;
			}
		}
		if (!rc) {
			if (cd->fmrfb_flags & FMRFB_FLAG_PIXCLK_VALID) {
				#if 0
				rc = fmrfb_hw_pixclk_set(
					cd->fmrfb_pixclk_src_id,
					PICOS2KHZ(cd->vmode_data_current.fb_vmode.pixclock));
				if (rc)
					pr_err("Error fmrfb changing pixel clock\n");
				#endif
			}
			fmrfb_fbi_update(fbi);
			pr_info("fmrfb video mode: %dx%d%s-%d@%d%s\n",
				fbi->var.xres, fbi->var.yres,
				cd->vmode_data_current.fb_vmode_opts_cvt,
				fbi->var.bits_per_pixel,
				cd->vmode_data_current.fb_vmode.refresh,
				cd->vmode_data_current.fb_vmode_opts_ext);
		}

		fmrfb_enable_logicvc_output(fbi);
		fmrfb_logicvc_disp_ctrl(fbi, true);

		/* set flag used for finding video mode only once */
		if (cd->fmrfb_flags & FMRFB_FLAG_VMODE_INIT)
			cd->fmrfb_flags |= FMRFB_FLAG_VMODE_SET;
		/* used only when resolution is changed */
		if (!(cd->fmrfb_flags & FMRFB_FLAG_VMODE_SET)) {
			if (afbi) {
				for (i = 0; i < cd->fmrfb_layers; i++)
					if (layer_on[i])
						fmrfb_enable_logicvc_layer(afbi[i]);
			} else {
				fmrfb_enable_logicvc_layer(fbi);
			}
		}
	}
	#endif

	return rc;
}

static int fmrfb_set_color_hw_rgb_to_yuv(
	u16 *transp, u16 *red, u16 *green, u16 *blue, int len, int idx,
	struct fmrfb_layer_data *ld)
{
	struct fmrfb_common_data *cd = ld->fmrfb_cd;
	u32 yuv_pixel;
	u32 y, cb, cr;
	u32 ykr, ykg, ykb, yk;
	u32 crkr, crkg, crkb;
	u32 cbkr, cbkg, cbkb;

	driver_devel("%s\n", __func__);

	if (idx > (LOGICVC_CLUT_SIZE-1) || len > LOGICVC_CLUT_SIZE)
		return -EINVAL;

	if ((cd->fmrfb_display_interface_type >> 4) == LOGICVC_DI_ITU656) {
		ykr  = 29900;
		ykg  = 58700;
		ykb  = 11400;
		yk   = 1600000;
		crkr = 51138;
		crkg = 42820;
		crkb = 8316;
		cbkr = 17258;
		cbkg = 33881;
		cbkb = 51140;
	} else {
		ykr  = 29900;
		ykg  = 58700;
		ykb  = 11400;
		yk   = 0;
		crkr = 49980;
		crkg = 41850;
		crkb = 8128;
		cbkr = 16868;
		cbkg = 33107;
		cbkb = 49970;
	}

	while (len > 0) {
		y = (
				(ykr * (red[idx] & 0xFF))
					+
				(ykg * (green[idx] & 0xFF))
					+
				(ykb * (blue[idx] & 0xFF))
					+
				 yk
			)
				/
			100000;
		cr = (
				(crkr * (red[idx] & 0xFF))
					-
				(crkg * (green[idx] & 0xFF))
					-
				(crkb * (blue[idx] & 0xFF))
					+
				 12800000
			 )
				/
			100000;
		cb = (
				(-cbkr * (red[idx] & 0xFF))
					-
				(cbkg * (green[idx] & 0xFF))
					+
				(cbkb * (blue[idx] & 0xFF))
					+
				12800000
			 )
				/
			100000;
		if (transp) {
			yuv_pixel = (((u32)transp[idx] & 0xFF) << 24) |
				(y << 16) | (cb << 8) | cr;
		} else {
			yuv_pixel =
				(0xFF << 24) | (y << 16) | (cb << 8) | cr;
		}
		writel(yuv_pixel, ld->layer_clut_base_virt +
			(idx*LOGICVC_CLUT_REGISTER_SIZE));
		len--;
		idx++;
	}

	return 0;
}

static int fmrfb_set_color_hw(u16 *transp, u16 *red, u16 *green, u16 *blue,
	int len, int idx, struct fb_info *fbi)
{
	struct fmrfb_layer_data *ld = fbi->par;
	struct fmrfb_layer_fix_data *lfdata = &ld->layer_fix;
	u32 pixel;
	int bpp_virt, toff, roff, goff, boff;

	driver_devel("%s\n", __func__);

	if ((fbi->fix.visual == FB_VISUAL_FOURCC) &&
		(fbi->var.grayscale == LOGICVC_PIX_FMT_AYUV)) {
		return fmrfb_set_color_hw_rgb_to_yuv(
			transp, red, green, blue, len, idx, ld);
	}

	bpp_virt = lfdata->bpp_virt;

	toff = fbi->var.transp.offset;
	roff = fbi->var.red.offset;
	goff = fbi->var.green.offset;
	boff = fbi->var.blue.offset;

	if (fbi->fix.visual == FB_VISUAL_PSEUDOCOLOR) {
		u32 clut_value;

		if (idx > (LOGICVC_CLUT_SIZE-1) || len > LOGICVC_CLUT_SIZE)
			return -EINVAL;

		if (lfdata->alpha_mode == LOGICVC_CLUT_16BPP_ALPHA) {
			if (transp) {
				while (len > 0) {
					clut_value =
						((((transp[idx] & 0xFC) >> 2) << toff) |
						(((red[idx] & 0xF8) >> 3) << roff) |
						(((green[idx] & 0xFC) >> 2) << goff) |
						(((blue[idx] & 0xF8) >> 3) << boff));
					writel(clut_value,
						ld->layer_clut_base_virt +
						(idx*LOGICVC_CLUT_REGISTER_SIZE));
					len--;
					idx++;
				}
			} else {
				while (len > 0) {
					clut_value =
						((0x3F << toff) |
						(((red[idx] & 0xF8) >> 3) << roff) |
						(((green[idx] & 0xFC) >> 2) << goff) |
						(((blue[idx] & 0xF8) >> 3) << boff));
					writel(clut_value,
						ld->layer_clut_base_virt +
						(idx*LOGICVC_CLUT_REGISTER_SIZE));
					len--;
					idx++;
				}
			}
		} else if (lfdata->alpha_mode == LOGICVC_CLUT_32BPP_ALPHA) {
			if (transp) {
				while (len > 0) {
					clut_value =
						(((transp[idx] & 0xFF) << toff) |
						((red[idx] & 0xFF) << roff) |
						((green[idx] & 0xFF) << goff) |
						((blue[idx] & 0xFF) << boff));
					writel(clut_value,
						ld->layer_clut_base_virt +
						(idx*LOGICVC_CLUT_REGISTER_SIZE));
					len--;
					idx++;
				}
			} else {
				while (len > 0) {
					clut_value =
						((0xFF << toff) |
						((red[idx] & 0xFF) << roff) |
						((green[idx] & 0xFF) << goff) |
						((blue[idx] & 0xFF) << boff));
					writel(clut_value,
						ld->layer_clut_base_virt +
						(idx*LOGICVC_CLUT_REGISTER_SIZE));
					len--;
					idx++;
				}
			}
		}
	} else if (fbi->fix.visual == FB_VISUAL_TRUECOLOR) {
		if (bpp_virt == 8) {
			if (lfdata->alpha_mode == LOGICVC_LAYER_ALPHA) {
				while (len > 0) {
					pixel = ((((red[idx] & 0xE0) >> 5) << roff) |
						(((green[idx] & 0xE0) >> 5) << goff) |
						(((blue[idx] & 0xC0) >> 6) << boff));
					((u32 *)(fbi->pseudo_palette))[idx] =
						(pixel << 24) | (pixel << 16) | (pixel << 8) | pixel;
					len--;
					idx++;
				}
			} else if (lfdata->alpha_mode == LOGICVC_PIXEL_ALPHA) {
				if (transp) {
					while (len > 0) {
						pixel = ((((transp[idx] & 0xE0) >> 5) << toff) |
							(((red[idx] & 0xE0) >> 5) << roff) |
							(((green[idx] & 0xE0) >> 5) << goff) |
							(((blue[idx] & 0xC0) >> 6) << boff));
						((u32 *)(fbi->pseudo_palette))[idx] =
							(pixel << 16) | pixel;
						len--;
						idx++;
					}
				} else {
					while (len > 0) {
						pixel = ((0x07 << toff) |
							(((red[idx] & 0xE0) >> 5) << roff) |
							(((green[idx] & 0xE0) >> 5) << goff) |
							(((blue[idx] & 0xC0) >> 6) << boff));
						((u32 *)(fbi->pseudo_palette))[idx] =
							(pixel << 16) | pixel;
						len--;
						idx++;
					}
				}
			}
		} else if (bpp_virt == 16) {
			if (lfdata->alpha_mode == LOGICVC_LAYER_ALPHA) {
				while (len > 0) {
					pixel = ((((red[idx] & 0xF8) >> 3) << roff) |
						(((green[idx] & 0xFC) >> 2) << goff) |
						(((blue[idx] & 0xF8) >> 3) << boff));
					((u32 *)(fbi->pseudo_palette))[idx] =
						(pixel << 16) | pixel;
					len--;
					idx++;
				}
			} else if (lfdata->alpha_mode == LOGICVC_PIXEL_ALPHA) {
				if (transp) {
					while (len > 0) {
						((u32 *)(fbi->pseudo_palette))[idx] =
							((((transp[idx] & 0xFC) >> 2) << toff) |
							(((red[idx] & 0xF8) >> 3) << roff) |
							(((green[idx] & 0xFC) >> 2) << goff) |
							(((blue[idx] & 0xF8) >> 3) << boff));
						len--;
						idx++;
					}
				} else {
					while (len > 0) {
						((u32 *)(fbi->pseudo_palette))[idx] =
							((0x3F << toff) |
							(((red[idx] & 0xF8) >> 3) << roff) |
							(((green[idx] & 0xFC) >> 2) << goff) |
							(((blue[idx] & 0xF8) >> 3) << boff));
						len--;
						idx++;
					}
				}
			}
		} else if (bpp_virt == 32) {
			if (lfdata->alpha_mode == LOGICVC_LAYER_ALPHA) {
				while (len > 0) {
					((u32 *)(fbi->pseudo_palette))[idx] =
						(((red[idx] & 0xFF) << roff) |
						((green[idx] & 0xFF) << goff) |
						((blue[idx] & 0xFF) << boff));
					len--;
					idx++;
				}
			} else if (lfdata->alpha_mode == LOGICVC_PIXEL_ALPHA) {
				if (transp) {
					while (len > 0) {
						((u32 *)(fbi->pseudo_palette))[idx] =
							(((transp[idx] & 0xFF) << toff) |
							((red[idx] & 0xFF) << roff) |
							((green[idx] & 0xFF) << goff) |
							((blue[idx] & 0xFF) << boff));
						len--;
						idx++;
					}
				} else {
					while (len > 0) {
						((u32 *)(fbi->pseudo_palette))[idx] =
							((0xFF << toff) |
							((red[idx] & 0xFF) << roff) |
							((green[idx] & 0xFF) << goff) |
							((blue[idx] & 0xFF) << boff));
						len--;
						idx++;
					}
				}
			}
		}
	} else {
		return -EINVAL;
	}

	return 0;
}

static int fmrfb_set_color_reg(unsigned regno, unsigned red, unsigned green,
	unsigned blue, unsigned transp, struct fb_info *fbi)
{
	driver_devel("%s\n", __func__);

	return fmrfb_set_color_hw(
		(u16 *)&transp, (u16 *)&red, (u16 *)&green, (u16 *)&blue,
		1, regno, fbi);
}

static int fmrfb_set_cmap(struct fb_cmap *cmap, struct fb_info *fbi)
{
	driver_devel("%s\n", __func__);

	return fmrfb_set_color_hw(
		cmap->transp, cmap->red, cmap->green, cmap->blue,
		cmap->len, cmap->start, fbi);
}

static void fmrfb_set_pixels(struct fb_info *fbi,
	struct fmrfb_layer_data *ld, int bpp, unsigned int pix)
{
	u32 *vmem;
	u8 *vmem8;
	u16 *vmem16;
	u32 *vmem32;
	int x, y, pix_off;

	driver_devel("%s\n", __func__);

	vmem = ld->fb_virt +
		(fbi->var.xoffset * (fbi->var.bits_per_pixel/4)) +
		(fbi->var.yoffset * fbi->var.xres_virtual *
		(fbi->var.bits_per_pixel/4));

	switch (bpp) {
	case 8:
		vmem8 = (u8 *)vmem;
		for (y = fbi->var.yoffset; y < fbi->var.yres; y++) {
			pix_off = (y * fbi->var.xres_virtual);
			for (x = fbi->var.xoffset; x < fbi->var.xres; x++)
				vmem8[pix_off+x] = pix;
		}
		break;
	case 16:
		vmem16 = (u16 *)vmem;
		for (y = fbi->var.yoffset; y < fbi->var.yres; y++) {
			pix_off = (y * fbi->var.xres_virtual);
			for (x = fbi->var.xoffset; x < fbi->var.xres; x++)
				vmem16[pix_off+x] = pix;
		}
		break;
	case 32:
		vmem32 = (u32 *)vmem;
		for (y = fbi->var.yoffset; y < fbi->var.yres; y++) {
			pix_off = (y * fbi->var.xres_virtual);
			for (x = fbi->var.xoffset; x < fbi->var.xres; x++)
				vmem32[pix_off+x] = pix;
		}
		break;
	}
}

static int fmrfb_blank(int blank_mode, struct fb_info *fbi)
{
	struct fmrfb_layer_data *ld = fbi->par;
	struct fmrfb_layer_fix_data *lfdata = &ld->layer_fix;
	u32 reg;

	driver_devel("%s\n", __func__);

	switch (blank_mode) {
	case FB_BLANK_UNBLANK:
		driver_devel("FB_BLANK_UNBLANK\n");
		reg = readl(ld->reg_base_virt + LOGICVC_SPWRCTRL_ROFF);
		reg |= LOGICVC_V_EN_MSK;
		writel(reg, ld->reg_base_virt + LOGICVC_SPWRCTRL_ROFF);
		mdelay(50);
		break;

	case FB_BLANK_NORMAL:
		driver_devel("FB_BLANK_NORMAL\n");
		switch (lfdata->bpp_virt) {
		case 8:
			switch (lfdata->alpha_mode) {
			case LOGICVC_LAYER_ALPHA:
				fmrfb_set_pixels(fbi, ld, 8, 0x00);
				break;
			case LOGICVC_PIXEL_ALPHA:
				fmrfb_set_pixels(fbi, ld, 16, 0xFF00);
				break;
			case LOGICVC_CLUT_16BPP_ALPHA:
			case LOGICVC_CLUT_32BPP_ALPHA:
				fmrfb_set_color_reg(0, 0, 0, 0, 0xFF, fbi);
				fmrfb_set_pixels(fbi, ld, 8, 0);
				break;
			}
			break;
		case 16:
			switch (lfdata->alpha_mode) {
			case LOGICVC_LAYER_ALPHA:
				fmrfb_set_pixels(fbi, ld, 16, 0x0000);
				break;
			case LOGICVC_PIXEL_ALPHA:
				fmrfb_set_pixels(fbi, ld, 32, 0xFF000000);
				break;
			}
			break;
		case 32:
			fmrfb_set_pixels(fbi, ld, 32, 0xFF000000);
			break;
		}
		break;

	case FB_BLANK_POWERDOWN:
		driver_devel("FB_BLANK_POWERDOWN\n");
		reg = readl(ld->reg_base_virt + LOGICVC_SPWRCTRL_ROFF);
		reg &= ~LOGICVC_V_EN_MSK;
		writel(reg, ld->reg_base_virt + LOGICVC_SPWRCTRL_ROFF);
		mdelay(50);
		break;

	case FB_BLANK_VSYNC_SUSPEND:
	case FB_BLANK_HSYNC_SUSPEND:
	default:
		driver_devel("FB_BLANK_ not supported!\n");
		return -EINVAL;
	}

	return 0;
}

static int fmrfb_pan_display(struct fb_var_screeninfo *var,
	struct fb_info *fbi)
{
	struct fmrfb_layer_data *ld = fbi->par;
	struct fmrfb_common_data *cd = ld->fmrfb_cd;

	driver_devel("%s\n", __func__);

	if (fbi->var.xoffset == var->xoffset &&
		fbi->var.yoffset == var->yoffset)
		return 0;

	if (fbi->var.vmode & FB_VMODE_YWRAP) {
		return -EINVAL;
	} else {
		if (var->xoffset + fbi->var.xres > fbi->var.xres_virtual ||
			var->yoffset + fbi->var.yres > fbi->var.yres_virtual) {
			/* if smaller then physical layer video memory
			   allow panning */
			if ((var->xoffset + fbi->var.xres > ld->layer_fix.width)
					||
				(var->yoffset + fbi->var.yres > ld->layer_fix.height)) {
				return -EINVAL;
			}
		}
	}
	/* YCbCr 4:2:2 layer type can only have even layer xoffset */
	if (ld->layer_fix.layer_type == LOGICVC_YCBCR_LAYER &&
		ld->layer_fix.bpp_virt == 16) {
		var->xoffset &= ~1;
	}

	fbi->var.xoffset = var->xoffset;
	fbi->var.yoffset = var->yoffset;
	/* set layer memory X offset */
	cd->reg_access.fmrfb_set_reg_val(var->xoffset,
		ld->layer_reg_base_virt, LOGICVC_LAYER_HOR_OFF_ROFF,
		ld);
	/* set layer memory Y offset */
	cd->reg_access.fmrfb_set_reg_val(var->yoffset,
		ld->layer_reg_base_virt, LOGICVC_LAYER_VER_OFF_ROFF,
		ld);
	cd->reg_access.fmrfb_set_reg_val((fbi->var.xres-1),
		ld->layer_reg_base_virt, LOGICVC_LAYER_HOR_POS_ROFF,
		ld);
	/* apply changes in logiCVC */
	cd->reg_access.fmrfb_set_reg_val((fbi->var.yres-1),
		ld->layer_reg_base_virt, LOGICVC_LAYER_VER_POS_ROFF,
		ld);

	return 0;
}

void fmrfb_imageblit (struct fb_info *info, const struct fb_image *image)
{
        return;
}

static struct fb_ops fmrfb_ops = {
	.owner = THIS_MODULE,
	.fb_open = fmrfb_open,
	.fb_release = fmrfb_release,
	.fb_check_var = fmrfb_check_var,
	.fb_set_par = fmrfb_set_par,
	.fb_setcolreg = fmrfb_set_color_reg,
	.fb_setcmap = fmrfb_set_cmap,
	.fb_blank = fmrfb_blank,
	.fb_pan_display = fmrfb_pan_display,
	.fb_fillrect = cfb_fillrect,
	.fb_copyarea = cfb_copyarea,
	.fb_imageblit = fmrfb_imageblit,
	.fb_cursor = NULL,
	.fb_sync = NULL,
	.fb_ioctl = fmrfb_ioctl,
	.fb_mmap = NULL,
	.fb_get_caps = NULL,
	.fb_destroy = NULL,
};

/******************************************************************************/

static int fmrfb_find_next_layer(struct fmrfb_layer_fix_data *lfdata,
	int layers, int curr)
{
	unsigned long address, temp_address, loop_address;
	int i, next;

	driver_devel("%s\n", __func__);

	address = lfdata[curr].offset * lfdata[curr].width * lfdata[curr].bpp;
	temp_address = ~0;
	next = -1;

	for (i = 0; i < layers; i++) {
		loop_address =
			lfdata[i].offset * lfdata[i].width * lfdata[i].bpp;
		if (address < loop_address
				&&
			loop_address < temp_address) {
			next = i;
			temp_address = loop_address;
		}
	}

	return next;
}

static void fmrfb_set_yvirt(struct fmrfb_init_data *init_data,
	int layers, int curr)
{
	struct fmrfb_layer_fix_data *lfdata;
	unsigned long vmem_base_addr, vmem_high_addr;
	int next;

	driver_devel("%s\n", __func__);

	lfdata = init_data->lfdata;
	vmem_base_addr = init_data->vmem_base_addr;
	vmem_high_addr = init_data->vmem_high_addr;

	next = fmrfb_find_next_layer(lfdata, layers, curr);

	if (next != -1) {
		lfdata[curr].height =
			((lfdata[next].width * (lfdata[next].bpp/8) *
			lfdata[next].offset)
				-
			(lfdata[curr].width * (lfdata[curr].bpp/8) *
			lfdata[curr].offset))
				/
			(lfdata[curr].width * (lfdata[curr].bpp/8));
	} else { /* last physical logiCVC layer */
		lfdata[curr].height = LOGICVC_MAX_LINES + 1;
		while (1) {
			if (((lfdata[curr].width * (lfdata[curr].bpp/8) *
				lfdata[curr].height)
					+
				(lfdata[curr].width * (lfdata[curr].bpp/8) *
				lfdata[curr].offset))
					<=
				(vmem_high_addr - vmem_base_addr))
				break;
			/* FIXME - magic decrease step */
			lfdata[curr].height -= 64;
		}
	}

	if (lfdata[curr].height >
		(lfdata[curr].buffer_offset * LOGICVC_MAX_LAYER_BUFFERS)) {
		lfdata[curr].height =
			lfdata[curr].buffer_offset * LOGICVC_MAX_LAYER_BUFFERS;
	}

	lfdata[curr].layer_fix_info |=
		((lfdata[curr].height / lfdata[curr].buffer_offset) << 4);
}

static int fmrfb_map(int id, int layers, struct device *dev,
	struct fmrfb_layer_data *ld,
	unsigned long vmem_base_addr,
	unsigned long reg_base_phys, void *reg_base_virt,
	int memmap)
{
	struct fmrfb_layer_fix_data *lfdata = &ld->layer_fix;
	int i;

	driver_devel("%s\n", __func__);

	/* logiCVC register mapping */
	ld->reg_base_phys = reg_base_phys;
	ld->reg_base_virt = reg_base_virt;
	/* check register mappings */
	if (!ld->reg_base_virt) {
		pr_err("Error fmrfb registers mapping\n");
		return -ENOMEM;
	}

	pr_info("AMX vmem_base_addr = %lu \n", vmem_base_addr);
	/* Video memory mapping */
	ld->fb_phys = vmem_base_addr +
		(lfdata->width * (lfdata->bpp/8) * lfdata->offset);
	ld->fb_size =
		lfdata->width * (lfdata->bpp/8) * lfdata->height;

	pr_info("AMX ld->fb_phys = %lu \n", ld->fb_phys);
	pr_info("AMX ld->fb_size = %lu \n", ld->fb_size);

	writel(ld->fb_phys, ld->reg_base_virt + 0x5c);
	writel(ld->fb_phys, ld->reg_base_virt + 0x60);
	writel(ld->fb_phys, ld->reg_base_virt + 0x64);

	if (memmap) {
		if (ld->fmrfb_cd->fmrfb_flags & FMRFB_FLAG_DMA_BUFFER) {
			/* NOT USED FOR NOW! */
			pr_info("AMX  dma_alloc_coherent \n");
			ld->fb_virt = dma_alloc_coherent(NULL,
				PAGE_ALIGN(ld->fb_size),
				&ld->fb_phys, GFP_KERNEL);
		} else {
			pr_info("AMX ioremap_wc \n");
			ld->fb_virt =
				ioremap_wc(ld->fb_phys, ld->fb_size);
		}
		/* check memory mappings */
		if (!ld->fb_virt) {
			pr_err("Error fmrfb vmem mapping\n");
			return -ENOMEM;
		}
	}

	for(i = 0; i < ld->fb_size; i++) {
		*((char *)ld->fb_virt) = 0xff;
	}

	ld->layer_reg_base_virt = ld->reg_base_virt +
		logicvc_layer_reg_offset[id];
	ld->layer_clut_base_virt = ld->reg_base_virt +
		logicvc_clut_reg_offset[id*LOGICVC_CLUT_0_INDEX_OFFSET];
	ld->layer_use_ref = 0;
	ld->layer_ctrl_flags = 0;

	return 0;
}

static void fmrfb_set_fbi_var_screeninfo(struct fb_var_screeninfo *var,
	struct fmrfb_common_data *cd)
{
	driver_devel("%s\n", __func__);

	var->xres = cd->vmode_data_current.fb_vmode.xres;
	var->yres = cd->vmode_data_current.fb_vmode.yres;
	var->pixclock = cd->vmode_data_current.fb_vmode.pixclock;
	var->left_margin = cd->vmode_data_current.fb_vmode.left_margin;
	var->right_margin = cd->vmode_data_current.fb_vmode.right_margin;
	var->upper_margin = cd->vmode_data_current.fb_vmode.upper_margin;
	var->lower_margin = cd->vmode_data_current.fb_vmode.lower_margin;
	var->hsync_len = cd->vmode_data_current.fb_vmode.hsync_len;
	var->vsync_len = cd->vmode_data_current.fb_vmode.vsync_len;
	var->sync = cd->vmode_data_current.fb_vmode.sync;
	var->vmode = cd->vmode_data_current.fb_vmode.vmode;
}

static void fmrfb_fbi_update(struct fb_info *fbi)
{
	struct fb_info **afbi = dev_get_drvdata(fbi->device);
	struct fmrfb_layer_data *ld = fbi->par;
	struct fmrfb_common_data *cd = ld->fmrfb_cd;
	int i, layers, layer_id;

	driver_devel("%s\n", __func__);

	if (!(cd->fmrfb_flags & FMRFB_FLAG_EDID_VMODE) ||
		!(cd->fmrfb_flags & FMRFB_FLAG_EDID_RDY) ||
		!afbi)
		return;

	layers = cd->fmrfb_layers;
	layer_id = ld->layer_fix.layer_fix_info & 0x0F;

	for (i = 0; i < layers; i++) {
		if (i == layer_id)
			continue;
		fmrfb_set_fbi_var_screeninfo(&afbi[i]->var, cd);
		afbi[i]->monspecs = afbi[layer_id]->monspecs;
	}
}

static void fmrfb_set_hw_specifics(struct fb_info *fbi,
	struct fmrfb_layer_data *ld,
	struct fmrfb_layer_fix_data *lfdata,
	unsigned long reg_base_phys)
{
	driver_devel("%s\n", __func__);

	fbi->fix.smem_start = ld->fb_phys;
	fbi->fix.smem_len = ld->fb_size;
	if (lfdata->layer_type == LOGICVC_RGB_LAYER)
		fbi->fix.type = FB_TYPE_PACKED_PIXELS;
	else if (lfdata->layer_type == LOGICVC_YCBCR_LAYER)
		fbi->fix.type = FB_TYPE_FOURCC;
	if ((lfdata->layer_type == LOGICVC_YCBCR_LAYER) ||
		(lfdata->layer_type == LOGICVC_ALPHA_LAYER)) {
		fbi->fix.visual = FB_VISUAL_FOURCC;
	} else if ((lfdata->layer_type == LOGICVC_RGB_LAYER) &&
		(lfdata->bpp == 8) &&
		((lfdata->alpha_mode == LOGICVC_CLUT_16BPP_ALPHA) ||
		(lfdata->alpha_mode == LOGICVC_CLUT_32BPP_ALPHA))) {
		fbi->fix.visual = FB_VISUAL_PSEUDOCOLOR;
	} else {
		/*
			Other logiCVC layer pixel formats:
			- 8 bpp: LAYER or PIXEL alpha
			  It is not true color, RGB triplet is stored in 8 bits.
			- 16 bpp:
			  LAYER alpha: RGB triplet is stored in 16 bits
			  PIXEL alpha: ARGB quadriplet is stored in 32 bits
			- 32 bpp: LAYER or PIXEL alpha
			  True color, RGB triplet or ARGB quadriplet
			  is stored in 32 bits.
		*/
		fbi->fix.visual = FB_VISUAL_TRUECOLOR;
	}
	/* sanity check */
	if ((lfdata->bpp != 8) &&
		((lfdata->alpha_mode == LOGICVC_CLUT_16BPP_ALPHA) ||
		(lfdata->alpha_mode == LOGICVC_CLUT_32BPP_ALPHA))) {
		pr_warn("fmrfb invalid layer alpha!\n");
		lfdata->alpha_mode = LOGICVC_LAYER_ALPHA;
	}
	fbi->fix.xpanstep = 1;
	fbi->fix.ypanstep = 1;
	fbi->fix.ywrapstep = 0;
	fbi->fix.line_length = lfdata->width * (lfdata->bpp/8);
	fbi->fix.mmio_start = reg_base_phys;
	fbi->fix.mmio_len = LOGICVC_REGISTERS_RANGE;
	fbi->fix.accel = FB_ACCEL_NONE;

	fbi->var.xres_virtual = lfdata->width;
	if (lfdata->height <= LOGICVC_MAX_LINES)
		fbi->var.yres_virtual = lfdata->height;
	else
		fbi->var.yres_virtual = LOGICVC_MAX_LINES;
	fbi->var.bits_per_pixel = lfdata->bpp;
	switch (lfdata->layer_type) {
	case LOGICVC_RGB_LAYER:
		fbi->var.grayscale = 0;
		break;
	case LOGICVC_YCBCR_LAYER:
		if (lfdata->bpp == 8) {
			fbi->var.grayscale = LOGICVC_PIX_FMT_AYUV;
		} else if (lfdata->bpp == 16) {
			if (ld->layer_ctrl_flags & LOGICVC_SWAP_RB)
				fbi->var.grayscale = V4L2_PIX_FMT_YVYU;
			else
				fbi->var.grayscale = V4L2_PIX_FMT_VYUY;
		} else if (lfdata->bpp == 32) {
			if (ld->layer_ctrl_flags & LOGICVC_SWAP_RB)
				fbi->var.grayscale = LOGICVC_PIX_FMT_AVUY;
			else
				fbi->var.grayscale = LOGICVC_PIX_FMT_AYUV;
		}
		break;
	case LOGICVC_ALPHA_LAYER:
		/* logiCVC Alpha layer 8bpp */
		fbi->var.grayscale = LOGICVC_PIX_FMT_ALPHA;
		break;
	}

	/*
		Set values according to logiCVC layer data width configuration:
		- layer data width can be 1, 2, 4 bytes
		- layer data width for 16 bpp can be 2 or 4 bytes
	*/
	if (lfdata->alpha_mode == LOGICVC_LAYER_ALPHA) {
		fbi->var.transp.offset = 0;
		fbi->var.transp.length = 0;
	}
	switch (lfdata->bpp_virt) {
	case 8:
		switch (lfdata->alpha_mode) {
		case LOGICVC_PIXEL_ALPHA:
			fbi->var.transp.offset = 8;
			fbi->var.transp.length = 3;

		case LOGICVC_LAYER_ALPHA:
			fbi->var.red.offset = 5;
			fbi->var.red.length = 3;
			fbi->var.green.offset = 2;
			fbi->var.green.length = 3;
			fbi->var.blue.offset = 0;
			fbi->var.blue.length = 2;
			break;

		case LOGICVC_CLUT_16BPP_ALPHA:
			fbi->var.transp.offset = 24;
			fbi->var.transp.length = 6;
			fbi->var.red.offset = 19;
			fbi->var.red.length = 5;
			fbi->var.green.offset = 10;
			fbi->var.green.length = 6;
			fbi->var.blue.offset = 3;
			fbi->var.blue.length = 5;
			break;

		case LOGICVC_CLUT_32BPP_ALPHA:
			fbi->var.transp.offset = 24;
			fbi->var.transp.length = 8;
			fbi->var.red.offset = 16;
			fbi->var.red.length = 8;
			fbi->var.green.offset = 8;
			fbi->var.green.length = 8;
			fbi->var.blue.offset = 0;
			fbi->var.blue.length = 8;
			break;
		}
		break;
	case 16:
		switch (lfdata->alpha_mode) {
		case LOGICVC_PIXEL_ALPHA:
			fbi->var.transp.offset = 24;
			fbi->var.transp.length = 6;

		case LOGICVC_LAYER_ALPHA:
			fbi->var.red.offset = 11;
			fbi->var.red.length = 5;
			fbi->var.green.offset = 5;
			fbi->var.green.length = 6;
			fbi->var.blue.offset = 0;
			fbi->var.blue.length = 5;
			break;
		}
		break;
	case 32:
		switch (lfdata->alpha_mode) {
		case LOGICVC_PIXEL_ALPHA:
			fbi->var.transp.offset = 24;
			fbi->var.transp.length = 8;

		case LOGICVC_LAYER_ALPHA:
			fbi->var.red.offset = 0;
			fbi->var.red.length = 8;
			fbi->var.green.offset = 8;
			fbi->var.green.length = 8;
			fbi->var.blue.offset = 16;
			fbi->var.blue.length = 8;
			break;
		}
		break;
	}
	fbi->var.transp.msb_right = 0;
	fbi->var.red.msb_right = 0;
	fbi->var.green.msb_right = 0;
	fbi->var.blue.msb_right = 0;
	fbi->var.activate = FB_ACTIVATE_NOW;
	fbi->var.height = 0;
	fbi->var.width = 0;
	fbi->var.sync = 0;
	fbi->var.rotate = 0;
}

static int fmrfb_set_timings(struct fb_info *fbi, int bpp)
{
	struct fmrfb_layer_data *ld = fbi->par;
	struct fmrfb_common_data *cd = ld->fmrfb_cd;
	struct fb_var_screeninfo fb_var;
	int rc;

	driver_devel("%s\n", __func__);

	if ((cd->fmrfb_flags & FMRFB_FLAG_VMODE_INIT) &&
		(!(cd->fmrfb_flags & FMRFB_FLAG_EDID_RDY)) &&
		memchr(cd->vmode_data.fb_vmode_name, 'x', 10)) {
		cd->vmode_data_current = cd->vmode_data;
		return 0;
	}

	/* switch-case to default */
	rc = 255;
	if ((cd->fmrfb_flags & FMRFB_FLAG_EDID_VMODE) &&
		(cd->fmrfb_flags & FMRFB_FLAG_EDID_RDY)) {
		if (cd->fmrfb_flags & FMRFB_FLAG_VMODE_INIT) {
		} else {
			rc = fb_find_mode(&fb_var, fbi, fmrfb_mode_option,
				fbi->monspecs.modedb, fbi->monspecs.modedb_len,
				&fmrfb_vmode.fb_vmode, bpp);
			if ((rc != 1) && (rc != 2))
				return -EINVAL;
		}
	} else {
		rc = fb_find_mode(&fb_var, fbi, fmrfb_mode_option, NULL, 0,
			&fmrfb_vmode.fb_vmode, bpp);
	}
#ifdef DEBUG
	switch (rc) {
	case 0:
		pr_err("Error fmrfb video mode\n"
			"using driver default mode %dx%dM-%d@%d\n",
			fmrfb_vmode.fb_vmode.xres,
			fmrfb_vmode.fb_vmode.yres,
			bpp,
			fmrfb_vmode.fb_vmode.refresh);
		break;
	case 1:
		driver_devel("fmrfb video mode %s\n", fmrfb_mode_option);
		break;
	case 2:
		pr_notice("fmrfb video mode %s with ignored refresh rate\n",
			fmrfb_mode_option);
		break;
	case 3:
		pr_notice("fmrfb default video mode %dx%dM-%d@%d\n",
			fmrfb_vmode.fb_vmode.xres,
			fmrfb_vmode.fb_vmode.yres,
			bpp,
			fmrfb_vmode.fb_vmode.refresh);
		break;
	case 4:
		pr_notice("fmrfb video mode fallback\n");
		break;
	default:
		break;
	}
#endif

	cd->vmode_data_current.ctrl_reg = cd->vmode_data.ctrl_reg;
	cd->vmode_data_current.fb_vmode.xres = fb_var.xres;
	cd->vmode_data_current.fb_vmode.yres = fb_var.yres;
	cd->vmode_data_current.fb_vmode.pixclock = fb_var.pixclock;
	cd->vmode_data_current.fb_vmode.left_margin = fb_var.left_margin;
	cd->vmode_data_current.fb_vmode.right_margin = fb_var.right_margin;
	cd->vmode_data_current.fb_vmode.upper_margin = fb_var.upper_margin;
	cd->vmode_data_current.fb_vmode.lower_margin = fb_var.lower_margin;
	cd->vmode_data_current.fb_vmode.hsync_len = fb_var.hsync_len;
	cd->vmode_data_current.fb_vmode.vsync_len = fb_var.vsync_len;
	cd->vmode_data_current.fb_vmode.sync = fb_var.sync;
	cd->vmode_data_current.fb_vmode.vmode = fb_var.vmode;
	cd->vmode_data_current.fb_vmode.refresh =
		DIV_ROUND_CLOSEST(
			(PICOS2KHZ(fb_var.pixclock) * 1000),
			((fb_var.xres + fb_var.left_margin +
			fb_var.right_margin + fb_var.hsync_len)
				*
			(fb_var.yres + fb_var.upper_margin +
			fb_var.lower_margin + fb_var.vsync_len)));
	strcpy(cd->vmode_data_current.fb_vmode_opts_cvt,
		cd->vmode_data.fb_vmode_opts_cvt);
	strcpy(cd->vmode_data_current.fb_vmode_opts_ext,
		cd->vmode_data.fb_vmode_opts_ext);
	sprintf(cd->vmode_data_current.fb_vmode_name,
		"%dx%d%s-%d@%d%s",
		fb_var.xres, fb_var.yres,
		cd->vmode_data_current.fb_vmode_opts_cvt,
		fb_var.bits_per_pixel,
		cd->vmode_data_current.fb_vmode.refresh,
		cd->vmode_data_current.fb_vmode_opts_ext);

	if ((cd->fmrfb_flags & FMRFB_FLAG_EDID_RDY) ||
		!memchr(cd->vmode_data.fb_vmode_name, 'x', 10)) {
		cd->vmode_data = cd->vmode_data_current;
	}

	return 0;
}

static int fmrfb_register_fb(struct fb_info *fbi,
	struct fmrfb_layer_data *ld,
	unsigned long reg_base_phys, int id, int *regfb)
{
	struct fmrfb_common_data *cd = ld->fmrfb_cd;
	struct fmrfb_layer_fix_data *lfdata = &ld->layer_fix;
	int alpha;

	driver_devel("%s\n", __func__);

	fbi->flags = FBINFO_DEFAULT;
	fbi->screen_base = (char __iomem *)ld->fb_virt;
	fbi->screen_size = ld->fb_size;
	fbi->pseudo_palette =
		kzalloc(sizeof(u32) * FMRFB_PSEUDO_PALETTE_SZ, GFP_KERNEL);
	fbi->fbops = &fmrfb_ops;

	sprintf(fbi->fix.id, "Fmr FB%d", id);
	fmrfb_set_hw_specifics(fbi, ld, lfdata, reg_base_phys);
	if (!(cd->fmrfb_flags & FMRFB_FLAG_DEFAULT_VMODE_SET)) {
		fmrfb_set_timings(fbi, fbi->var.bits_per_pixel);
		cd->fmrfb_flags |= FMRFB_FLAG_DEFAULT_VMODE_SET;
	}
	fmrfb_set_fbi_var_screeninfo(&fbi->var, cd);
	fbi->mode = &cd->vmode_data_current.fb_vmode;
	fbi->mode->name = cd->vmode_data_current.fb_vmode_name;

	if (lfdata->alpha_mode == LOGICVC_LAYER_ALPHA)
		alpha = 0;
	else
		alpha = 1;
	if (fb_alloc_cmap(&fbi->cmap, FMRFB_PSEUDO_PALETTE_SZ, alpha))
		return -ENOMEM;

	*regfb = register_framebuffer(fbi);
	if (*regfb) {
		pr_err("Error fmrfb registering fmrfb %d\n", id);
		return -EINVAL;
	}

	pr_info("fmrfb %d registered\n", id);
	/* after fb driver registration, values in struct fb_info
		must not be changed anywhere else except in fmrfb_set_par */

	return 0;
}

static void fmrfb_init_layer_regs(struct fmrfb_layer_data *ld)
{
	struct fmrfb_common_data *cd = ld->fmrfb_cd;
	u32 reg_val;

	switch (ld->layer_fix.bpp_virt) {
	case 8:
		switch (ld->layer_fix.alpha_mode) {
		case LOGICVC_CLUT_16BPP_ALPHA:
			reg_val = TRANSPARENT_COLOR_8BPP_CLUT_16;
			break;
		case LOGICVC_CLUT_32BPP_ALPHA:
			reg_val = TRANSPARENT_COLOR_8BPP_CLUT_24;
			break;
		default:
			reg_val = TRANSPARENT_COLOR_8BPP;
			break;
		}
		break;
	case 16:
		reg_val = TRANSPARENT_COLOR_16BPP;
		break;
	case 32:
		reg_val = TRANSPARENT_COLOR_24BPP;
		break;
	default:
		reg_val = TRANSPARENT_COLOR_24BPP;
		break;
	}
	cd->reg_access.fmrfb_set_reg_val(reg_val,
		ld->layer_reg_base_virt, LOGICVC_LAYER_TRANSP_ROFF,
		ld);

	if (!(cd->fmrfb_flags & LOGICVC_READABLE_REGS))
		cd->reg_access.fmrfb_set_reg_val(0xFF,
			ld->layer_reg_base_virt, LOGICVC_LAYER_ALPHA_ROFF,
			ld);

	reg_val = ld->layer_ctrl_flags;
	cd->reg_access.fmrfb_set_reg_val(reg_val,
		ld->layer_reg_base_virt, LOGICVC_LAYER_CTRL_ROFF,
		ld);
}

static void fmrfb_logicvc_disp_ctrl(struct fb_info *fbi, bool enable)
{
	struct fmrfb_layer_data *ld = fbi->par;
	struct fmrfb_common_data *cd = ld->fmrfb_cd;
	u32 val;

	driver_devel("%s\n", __func__);

	if (enable) {
		val = LOGICVC_EN_VDD_MSK;
		writel(val, ld->reg_base_virt + LOGICVC_SPWRCTRL_ROFF);
		mdelay(cd->power_on_delay);
		val |= LOGICVC_V_EN_MSK;
		writel(val, ld->reg_base_virt + LOGICVC_SPWRCTRL_ROFF);
		mdelay(cd->signal_on_delay);
		val |= LOGICVC_EN_BLIGHT_MSK;
		writel(val, ld->reg_base_virt + LOGICVC_SPWRCTRL_ROFF);
	} else {
		writel(0, ld->reg_base_virt + LOGICVC_SPWRCTRL_ROFF);
	}
}

static void fmrfb_enable_logicvc_layer(struct fb_info *fbi)
{
	struct fmrfb_layer_data *ld = fbi->par;

	driver_devel("%s\n", __func__);

	ld->layer_ctrl_flags |= LOGICVC_LAYER_ON;
	ld->fmrfb_cd->reg_access.fmrfb_set_reg_val(
		ld->layer_ctrl_flags,
		ld->layer_reg_base_virt, LOGICVC_LAYER_CTRL_ROFF,
		ld);
}

static void fmrfb_disable_logicvc_layer(struct fb_info *fbi)
{
	struct fmrfb_layer_data *ld = fbi->par;

	driver_devel("%s\n", __func__);

	ld->layer_ctrl_flags &= (~LOGICVC_LAYER_ON);
	ld->fmrfb_cd->reg_access.fmrfb_set_reg_val(
		ld->layer_ctrl_flags,
		ld->layer_reg_base_virt, LOGICVC_LAYER_CTRL_ROFF,
		ld);
}

#if 0
double clk_find_params(double freq, struct clkmode *best_pick)
{
	double best_error = 2000.0;
	double cur_error;
	double cur_clk_mult;
	double cur_freq;
	u32 cur_div, curfb, cur_clk_div;
	u32 minfb = 0;
	u32 maxfb = 0;

	freq = freq * 5.0;
	best_pick->freq = 0.0;

	for(cur_div = 1; cur_div <= 10; cur_div++) {
		minfb = cur_div * 6;
		maxfb = cur_div * 12;
		if(maxfb > 64) {
			maxfb = 64;
		}

		printk("!!!! %a\n", freq);
		
		cur_clk_mult = (100.0 / (double)cur_div) / freq;

		curfb = minfb;
		while(curfb <= maxfb) {
			cur_clk_div = (u32)((cur_clk_mult * (double)curfb) + 0.5);
			cur_freq = ((100.0 / (double)cur_div) / (double)cur_clk_div) * (double)curfb;
			if(cur_freq > freq) {
				cur_error = cur_freq - freq;
			} else {
				cur_error = freq - cur_freq;
			}
			if(cur_error < best_error) {
				best_error = cur_error;
				best_pick->clk_div = cur_clk_div;
				best_pick->fb_mult = curfb;
				best_pick->main_div = cur_div;
				best_pick->freq = cur_freq;
			}

			curfb++;
		}
	}

	best_pick->freq = best_pick->freq / 5.0;
	best_error = best_error / 5.0;
	return best_error;
}
#endif	

static void fmrfb_enable_logicvc_output(struct fb_info *fbi)
{
	struct fmrfb_layer_data *ld = fbi->par;
	struct fmrfb_common_data *cd = ld->fmrfb_cd;
	int h_pixels, v_lines;
	u32 vtc_reg;

	driver_devel("%s\n", __func__);

	writel(0x03, ld->reg_base_virt);
	writel(cd->vmode_data_current.fb_vmode.xres * 4, ld->reg_base_virt + SHSY_RES_ROFF + 4);
	writel(cd->vmode_data_current.fb_vmode.xres * 4, ld->reg_base_virt + SHSY_RES_ROFF);
	writel(cd->vmode_data_current.fb_vmode.yres, ld->reg_base_virt + SVSY_RES_ROFF);

	driver_devel("%s, reg - 0x%lX, val - 0x%lX\n", __func__, ld->reg_base_virt + SHSY_RES_ROFF + 4, readl(ld->reg_base_virt + SHSY_RES_ROFF + 4));
	driver_devel("%s, reg - 0x%lX, val - 0x%lX\n", __func__, ld->reg_base_virt + SHSY_RES_ROFF, readl(ld->reg_base_virt + SHSY_RES_ROFF));
	driver_devel("%s, reg - 0x%lX, val - 0x%lX\n", __func__, ld->reg_base_virt + SVSY_RES_ROFF, readl(ld->reg_base_virt + SVSY_RES_ROFF));

	vtc_reg = readl(ld->vtc_base_virt);
	writel(vtc_reg | 0x02, ld->vtc_base_virt);

	h_pixels = cd->vmode_data_current.fb_vmode.xres + cd->vmode_data_current.fb_vmode.hsync_len
			+ cd->vmode_data_current.fb_vmode.left_margin + cd->vmode_data_current.fb_vmode.right_margin;
	v_lines = cd->vmode_data_current.fb_vmode.yres + cd->vmode_data_current.fb_vmode.vsync_len
                        + cd->vmode_data_current.fb_vmode.upper_margin + cd->vmode_data_current.fb_vmode.lower_margin;
	// Generator HSIZE
	//writel(h_pixels, ld->vtc_base_virt + 0x70);

    driver_devel("%s, h_pixels = 0x%lX\n", __func__, h_pixels);
	driver_devel("%s, reg - 0x%lX, val - 0x%lX\n", __func__, ld->vtc_base_virt + 0x70, readl(ld->vtc_base_virt + 0x70));

    // Generator VSIZE
	//writel((v_lines << 16) | v_lines, ld->vtc_base_virt + 0x74);

	driver_devel("%s, v_lines = 0x%lX\n", __func__, v_lines);
	driver_devel("%s, reg - 0x%lX, val - 0x%lX\n", __func__, ld->vtc_base_virt + 0x74, readl(ld->vtc_base_virt + 0x74));

    // Generator active size
	//writel((cd->vmode_data_current.fb_vmode.yres << 16) | cd->vmode_data_current.fb_vmode.xres, ld->vtc_base_virt + 0x60);
    
	driver_devel("%s, yres<<16 | xres = 0x%lX\n", __func__, (cd->vmode_data_current.fb_vmode.yres << 16) | cd->vmode_data_current.fb_vmode.xres);
	driver_devel("%s, reg - 0x%lX, val - 0x%lX\n", __func__, ld->vtc_base_virt + 0x60, readl(ld->vtc_base_virt + 0x60));

	// Generator HSYNC
	//writel((cd->vmode_data_current.fb_vmode.xres + cd->vmode_data_current.fb_vmode.left_margin) | ((h_pixels - cd->vmode_data_current.fb_vmode.right_margin) << 16), 
	//		ld->vtc_base_virt + 0x78);
	
	driver_devel("%s, xres+left_margin | right_margin<<16 = 0x%lX\n", __func__, (cd->vmode_data_current.fb_vmode.yres << 16) | cd->vmode_data_current.fb_vmode.xres);
	driver_devel("%s, reg - 0x%lX, val - 0x%lX\n", __func__, ld->vtc_base_virt + 0x78, readl(ld->vtc_base_virt + 0x78));

    // Generator F0_VSYNC_V
	//writel((cd->vmode_data_current.fb_vmode.yres + cd->vmode_data_current.fb_vmode.upper_margin - 1) | ((v_lines - cd->vmode_data_current.fb_vmode.lower_margin - 1) << 16), 
	//		ld->vtc_base_virt + 0x80);

	driver_devel("%s, yres+upper_margin | (lower_margin-1)<<16 = 0x%lX\n", __func__, (cd->vmode_data_current.fb_vmode.yres + cd->vmode_data_current.fb_vmode.upper_margin - 1) | ((v_lines - cd->vmode_data_current.fb_vmode.lower_margin - 1) << 16));
	driver_devel("%s, reg - 0x%lX, val - 0x%lX\n", __func__, ld->vtc_base_virt + 0x80, readl(ld->vtc_base_virt + 0x80));
	
	// Generator F1_VBLANK_V
	//writel((cd->vmode_data_current.fb_vmode.yres + cd->vmode_data_current.fb_vmode.upper_margin - 1) | ((v_lines - cd->vmode_data_current.fb_vmode.lower_margin - 1) << 16), 
	//		ld->vtc_base_virt + 0x8c);

	driver_devel("%s, yres+upper_margin | (v_lines-lower_margin-1)<<16 = 0x%lX\n", __func__, (cd->vmode_data_current.fb_vmode.yres + cd->vmode_data_current.fb_vmode.upper_margin - 1) | ((v_lines - cd->vmode_data_current.fb_vmode.lower_margin - 1) << 16));
	driver_devel("%s, reg - 0x%lX, val - 0x%lX\n", __func__, ld->vtc_base_virt + 0x8c, readl(ld->vtc_base_virt + 0x8c));

	// Generator F0_VSYNC_V
	//writel((cd->vmode_data_current.fb_vmode.xres << 16) | cd->vmode_data_current.fb_vmode.xres, ld->vtc_base_virt + 0x7c);

	driver_devel("%s, xres<<16 | xres = 0x%lX\n", __func__, (cd->vmode_data_current.fb_vmode.xres << 16) | cd->vmode_data_current.fb_vmode.xres);
	driver_devel("%s, reg - 0x%lX, val - 0x%lX\n", __func__, ld->vtc_base_virt + 0x7c, readl(ld->vtc_base_virt + 0x7c));

	// Generator F0_VSYNC_H
	//writel(((cd->vmode_data_current.fb_vmode.xres + cd->vmode_data_current.fb_vmode.left_margin) << 16) | 
    //             (cd->vmode_data_current.fb_vmode.xres + cd->vmode_data_current.fb_vmode.left_margin),
    //                    ld->vtc_base_virt + 0x84); 

	driver_devel("%s, xres+left_margin<<16 | xres+left_margin = 0x%lX\n", __func__, ((cd->vmode_data_current.fb_vmode.xres + cd->vmode_data_current.fb_vmode.left_margin) << 16) | 
                (cd->vmode_data_current.fb_vmode.xres + cd->vmode_data_current.fb_vmode.left_margin));
	driver_devel("%s, reg - 0x%lX, val - 0x%lX\n", __func__, ld->vtc_base_virt + 0x84, readl(ld->vtc_base_virt + 0x84));

	// Generator F1_VBLANK_H
	//writel((cd->vmode_data_current.fb_vmode.xres << 16) | cd->vmode_data_current.fb_vmode.xres, ld->vtc_base_virt + 0x88);

	driver_devel("%s, xres<<16 | xres = 0x%lX\n", __func__, (cd->vmode_data_current.fb_vmode.xres << 16) | cd->vmode_data_current.fb_vmode.xres);
	driver_devel("%s, reg - 0x%lX, val - 0x%lX\n", __func__, ld->vtc_base_virt + 0x88, readl(ld->vtc_base_virt + 0x88));

	// Generator F1_VSYNC_V
	//writel(((cd->vmode_data_current.fb_vmode.xres + cd->vmode_data_current.fb_vmode.left_margin) << 16) | 
    //             (cd->vmode_data_current.fb_vmode.xres + cd->vmode_data_current.fb_vmode.left_margin),
    //                    ld->vtc_base_virt + 0x90);

	driver_devel("%s, xres+left_margin<<16 | xres+left_margin = 0x%lX\n", __func__, ((cd->vmode_data_current.fb_vmode.xres + cd->vmode_data_current.fb_vmode.left_margin) << 16) | 
                (cd->vmode_data_current.fb_vmode.xres + cd->vmode_data_current.fb_vmode.left_margin));
	driver_devel("%s, reg - 0x%lX, val - 0x%lX\n", __func__, ld->vtc_base_virt + 0x90, readl(ld->vtc_base_virt + 0x90));

	vtc_reg = readl(ld->vtc_base_virt);
	writel(vtc_reg | 0x04, ld->vtc_base_virt);

	driver_devel("\n" \
		"logiCVC HW parameters:\n" \
		"    Horizontal Front Porch: %d pixclks\n" \
		"    Horizontal Sync:        %d pixclks\n" \
		"    Horizontal Back Porch:  %d pixclks\n" \
		"    Vertical Front Porch:   %d pixclks\n" \
		"    Vertical Sync:          %d pixclks\n" \
		"    Vertical Back Porch:    %d pixclks\n" \
		"    Pixel Clock:            %d ps\n" \
		"    Horizontal Res:         %d\n" \
		"    Vertical Res:           %d\n" \
		"\n", \
		cd->vmode_data_current.fb_vmode.right_margin,
		cd->vmode_data_current.fb_vmode.hsync_len,
		cd->vmode_data_current.fb_vmode.left_margin,
		cd->vmode_data_current.fb_vmode.lower_margin,
		cd->vmode_data_current.fb_vmode.vsync_len,
		cd->vmode_data_current.fb_vmode.upper_margin,
		cd->vmode_data_current.fb_vmode.pixclock,
		cd->vmode_data_current.fb_vmode.xres,
		cd->vmode_data_current.fb_vmode.yres);
}

static void fmrfb_disable_logicvc_output(struct fb_info *fbi)
{
	struct fb_info **afbi = dev_get_drvdata(fbi->device);
	struct fmrfb_layer_data *ld = fbi->par;
	struct fmrfb_common_data *cd = ld->fmrfb_cd;
	int i;

	driver_devel("%s\n", __func__);

	if (afbi) {
		for (i = 0; i < cd->fmrfb_layers; i++)
			fmrfb_disable_logicvc_layer(afbi[i]);
	}
}

static void fmrfb_start(struct fb_info **afbi, int layers)
{
	struct fmrfb_layer_data *ld;
	int i;

	driver_devel("%s\n", __func__);

	/* turn OFF all layers except already used ones */
	for (i = 0; i < layers; i++) {
		ld = afbi[i]->par;
		if (ld->layer_ctrl_flags & LOGICVC_LAYER_ON)
			continue;
		/* turn off layer */
		fmrfb_disable_logicvc_layer(afbi[i]);
	}
	/* print layer parameters */
	for (i = 0; i < layers; i++) {
		ld = afbi[i]->par;
		driver_devel("logiCVC layer %d\n" \
			"    Registers Base Address:     0x%lX\n" \
			"    Layer Video Memory Address: 0x%lX\n" \
			"    X resolution:               %d\n" \
			"    Y resolution:               %d\n" \
			"    X resolution (virtual):     %d\n" \
			"    Y resolution (virtual):     %d\n" \
			"    Line length (bytes):        %d\n" \
			"    Bits per Pixel:             %d\n" \
			"\n", \
			i,
			(unsigned long)ld->reg_base_phys,
			(unsigned long)ld->fb_phys,
			afbi[i]->var.xres,
			afbi[i]->var.yres,
			afbi[i]->var.xres_virtual,
			afbi[i]->var.yres_virtual,
			afbi[i]->fix.line_length,
			afbi[i]->var.bits_per_pixel);
	}
}

/******************************************************************************/

static int fmrfb_event_notify(struct notifier_block *nb,
	unsigned long event, void *data)
{
	struct fb_event *fbe = data;
	struct fb_info *fbi = fbe->info;
	int ret = 0;

	driver_devel("%s\n", __func__);

	switch (event) {
	case FMRFB_EVENT_FBI_UPDATE:
		fmrfb_fbi_update(fbi);
		break;
	}

	return ret;
}

/******************************************************************************/

static void fmrfb_get_vmode_opts(
	struct fmrfb_init_data *init_data,
	struct fmrfb_common_data *cd)
{
	char cvt_opt[VMODE_OPTS_SZ] = "MR";
	char ext_opt[VMODE_OPTS_SZ] = "im";
	char *s, *opt, *ext, *c, *pco, *peo;

	if (cd->fmrfb_flags & FMRFB_FLAG_EDID_VMODE)
		return;

	s = init_data->vmode_data.fb_vmode_name;
	opt = cd->vmode_data.fb_vmode_opts_cvt;
	ext = cd->vmode_data.fb_vmode_opts_ext;
	pco = cvt_opt;
	peo = ext_opt;

	while (*pco) {
		c = strchr(s, (int)(*pco));
		if (c)
			*opt++ = *c;
		pco++;
	}
	while (*peo) {
		c = strchr(s, (int)(*peo));
		if (c)
			*ext++ = *c;
		peo++;
	}
}

int fmrfb_init_driver(struct fmrfb_init_data *init_data)
{
	struct device *dev;
	struct fb_info **afbi;
	struct fb_info *fbi;
	struct fmrfb_common_data *cd;
	struct fmrfb_layer_data *ld;
	struct resource *reg_res, *irq_res;
	void *reg_base_virt;
	unsigned long reg_base_phys;
	void *vtc_base_virt;
	unsigned long vtc_baseaddr;
	int vtc_size;
	void *clk_base_virt;
	unsigned long clk_baseaddr;
	int clk_size;
	int reg_range, layers, active_layer;
	int i, rc, memmap;
	int regfb[LOGICVC_MAX_LAYERS];

	driver_devel("%s\n", __func__);

	dev = &init_data->pdev->dev;

	reg_res = platform_get_resource(init_data->pdev, IORESOURCE_MEM, 0);
	irq_res = platform_get_resource(init_data->pdev, IORESOURCE_IRQ, 0);
	if ((!reg_res) || (!irq_res)) {
		pr_err("Error fmrfb resources\n");
		return -ENODEV;
	}

	layers = init_data->layers;
	if (layers == 0) {
		pr_err("Error fmrfb zero layers\n");
		return -ENODEV;
	}
	active_layer = init_data->active_layer;
	if (active_layer >= layers) {
		pr_err("Error fmrfb default layer: set to 0\n");
		active_layer = 0;
	}
	vtc_baseaddr = init_data->vtc_baseaddr;
	vtc_size = init_data->vtc_size;	
	if (vtc_baseaddr == 0 || vtc_size == 0) {
		pr_err("Error fmrfb vtc baseaddr\n");
		return -ENODEV;
	}
	clk_baseaddr = init_data->clk_baseaddr;
	clk_size = init_data->clk_size;	
	if (clk_baseaddr == 0 || clk_size == 0) {
		pr_err("Error fmrfb clk baseaddr\n");
		return -ENODEV;
	}

	afbi = kzalloc(sizeof(struct fb_info *) * layers, GFP_KERNEL);
	cd = kzalloc(sizeof(struct fmrfb_common_data), GFP_KERNEL);
	if (!afbi || !cd) {
		pr_err("Error fmrfb allocating internal data\n");
		rc = -ENOMEM;
		goto err_mem;
	}

	BLOCKING_INIT_NOTIFIER_HEAD(&cd->fmrfb_notifier_list);
	cd->fmrfb_nb.notifier_call = fmrfb_event_notify;
	blocking_notifier_chain_register(
		&cd->fmrfb_notifier_list, &cd->fmrfb_nb);

	cd->fmrfb_display_interface_type =
		init_data->display_interface_type;
	cd->fmrfb_layers = layers;
	cd->fmrfb_flags |= FMRFB_FLAG_VMODE_INIT;
	cd->fmrfb_console_layer = active_layer;
	if (init_data->flags & FMRFB_FLAG_EDID_VMODE)
			cd->fmrfb_flags |= FMRFB_FLAG_EDID_VMODE;
	if (init_data->flags & FMRFB_FLAG_EDID_PRINT)
			cd->fmrfb_flags |= FMRFB_FLAG_EDID_PRINT;
	if (init_data->flags & LOGICVC_READABLE_REGS) {
		cd->fmrfb_flags |= LOGICVC_READABLE_REGS;
		cd->reg_access.fmrfb_get_reg_val = fmrfb_get_reg;
		cd->reg_access.fmrfb_set_reg_val = fmrfb_set_reg;
	} else {
		cd->reg_list =
			kzalloc(sizeof(struct fmrfb_registers), GFP_KERNEL);
		cd->reg_access.fmrfb_get_reg_val = fmrfb_get_reg_mem;
		cd->reg_access.fmrfb_set_reg_val = fmrfb_set_reg_mem;
	}

	sprintf(init_data->vmode_data.fb_vmode_name, "%s-%d@%d",
		init_data->vmode_data.fb_vmode_name,
		init_data->lfdata[active_layer].bpp,
		init_data->vmode_data.fb_vmode.refresh);
	if (init_data->vmode_params_set) {
		cd->vmode_data = init_data->vmode_data;
	} else {
		fmrfb_mode_option = init_data->vmode_data.fb_vmode_name;
		cd->vmode_data.ctrl_reg = init_data->vmode_data.ctrl_reg;
		cd->vmode_data.fb_vmode.refresh =
			init_data->vmode_data.fb_vmode.refresh;
	}
	fmrfb_get_vmode_opts(init_data, cd);

	#if 0
	if (init_data->pixclk_src_id) {
		if (fmrfb_hw_pixclk_supported(init_data->pixclk_src_id)) {
			cd->fmrfb_pixclk_src_id = init_data->pixclk_src_id;
			cd->fmrfb_flags |= FMRFB_FLAG_PIXCLK_VALID;
		} else {
			pr_info("fmrfb pixel clock not supported\n");
		}
	} else {
		pr_info("fmrfb external pixel clock\n");
	}
	#endif

	ld = NULL;

	reg_base_phys = reg_res->start;
	reg_range = resource_size(reg_res);
	reg_base_virt = ioremap_nocache(reg_base_phys, reg_range);

	vtc_base_virt = ioremap_nocache(vtc_baseaddr, vtc_size);

	clk_base_virt = ioremap_nocache(clk_baseaddr, clk_size);

	/* load layer parameters for all layers */
	for (i = 0; i < layers; i++)
		regfb[i] = -1;
	memmap = 1;

	/* make /dev/fb0 to be default active layer
	   regardless how logiCVC layers are organized */
	for (i = active_layer; i < layers; i++) {
		if (regfb[i] != -1)
			continue;

		fbi = framebuffer_alloc(sizeof(struct fmrfb_layer_data), dev);
		if (!fbi) {
			pr_err("Error fmrfb allocate info\n");
			rc = -ENOMEM;
			goto err_fb;
		}
		afbi[i] = fbi;
		ld = fbi->par;
		ld->fmrfb_cd = cd;

		fmrfb_set_yvirt(init_data, layers, i);

		ld->layer_fix = init_data->lfdata[i];
		if (!(cd->fmrfb_flags & LOGICVC_READABLE_REGS)) {
			ld->layer_reg_list =
				kzalloc(sizeof(struct fmrfb_layer_registers), GFP_KERNEL);
		}

		ld->vtc_base_virt = vtc_base_virt; 
		ld->clk_base_virt = clk_base_virt; 

		rc = fmrfb_map(i, layers, dev, ld, init_data->vmem_base_addr,
			reg_base_phys, reg_base_virt, memmap);
		if (rc)
			goto err_fb;
		memmap = 0;

		ld->layer_ctrl_flags = init_data->layer_ctrl_flags[i];
		fmrfb_init_layer_regs(ld);

		rc = fmrfb_register_fb(fbi, ld, reg_base_phys, i, &regfb[i]);
		if (rc)
			goto err_fb;

		fbi->monspecs = afbi[cd->fmrfb_console_layer]->monspecs;

		mutex_init(&ld->layer_mutex);

		/* register following layers in HW configuration order */
		if (active_layer > 0) {
			i = -1; /* after for loop increment i will be zero */
			active_layer = -1;
		}

		driver_devel( \
			"    Layer ID %d\n" \
			"    Layer offset %u\n" \
			"    Layer buffer offset %hd\n" \
			"    Layer buffers %d\n" \
			"    Layer width %d pixels\n" \
			"    Layer height %d lines\n" \
			"    Layer bits per pixel %d\n" \
			"    Layer bits per pixel (virtual) %d\n" \
			"    Layer FB size %ld bytes\n", \
			(ld->layer_fix.layer_fix_info & 0x0F),
			ld->layer_fix.offset,
			ld->layer_fix.buffer_offset,
			(ld->layer_fix.layer_fix_info >> 4),
			ld->layer_fix.width,
			ld->layer_fix.height,
			ld->layer_fix.bpp,
			ld->layer_fix.bpp_virt,
			ld->fb_size);
	}

	if (ld) {
		if (!(cd->fmrfb_flags & LOGICVC_READABLE_REGS))
			cd->reg_access.fmrfb_set_reg_val(0xFFFF,
				ld->reg_base_virt, LOGICVC_INT_MASK_ROFF,
				ld);
	} else {
		pr_warn("Warning fmrfb initialization not completed\n");
	}

	cd->fmrfb_bg_layer_bpp = init_data->bg_layer_bpp;
	cd->fmrfb_bg_layer_alpha_mode = init_data->bg_layer_alpha_mode;
	driver_devel("BG layer %dbpp\n", init_data->bg_layer_bpp);

	cd->fmrfb_irq = irq_res->start;
	rc = request_irq(cd->fmrfb_irq, fmrfb_isr,
		IRQF_TRIGGER_HIGH, DEVICE_NAME, dev);
	if (rc) {
		cd->fmrfb_irq = 0;
		goto err_fb;
	}

#if defined(__LITTLE_ENDIAN)
	cd->fmrfb_flags |= FMRFB_FLAG_MEMORY_LE;
#endif
	mutex_init(&cd->irq_mutex);
	init_waitqueue_head(&cd->vsync.wait);
	cd->fmrfb_use_ref = 0;

	dev_set_drvdata(dev, (void *)afbi);

	cd->fmrfb_flags &= ~(FMRFB_FLAG_VMODE_INIT |
		FMRFB_FLAG_DEFAULT_VMODE_SET | FMRFB_FLAG_VMODE_SET);
	fmrfb_mode_option = NULL;

	/* start HW */
	fmrfb_start(afbi, layers);

	return 0;

err_fb:
	if (cd->fmrfb_irq != 0)
		free_irq(cd->fmrfb_irq, dev);
	for (i = layers-1; i >= 0; i--) {
		fbi = afbi[i];
		if (!fbi)
			continue;
		ld = fbi->par;
		if (regfb[i] == 0)
			unregister_framebuffer(fbi);
		else
			regfb[i] = 0;
		if (fbi->cmap.red)
			fb_dealloc_cmap(&fbi->cmap);
		if (ld) {
			if (cd->fmrfb_flags & FMRFB_FLAG_DMA_BUFFER) {
				/* NOT USED FOR NOW! */
				dma_free_coherent(dev,
					PAGE_ALIGN(fbi->fix.smem_len),
					ld->fb_virt, ld->fb_phys);
			} else {
				if (ld->fb_virt)
					iounmap(ld->fb_virt);
			}
			kfree(ld->layer_reg_list);
			kfree(fbi->pseudo_palette);
			framebuffer_release(fbi);
		}
	}
	if (reg_base_virt)
		iounmap(reg_base_virt);

err_mem:
	if (cd) {
		kfree(cd->reg_list);
		kfree(cd);
	}
	kfree(afbi);

	dev_set_drvdata(dev, NULL);

	return rc;
}

int fmrfb_deinit_driver(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fb_info **afbi = dev_get_drvdata(dev);
	struct fb_info *fbi = afbi[0];
	struct fmrfb_layer_data *ld = fbi->par;
	struct fmrfb_common_data *cd = ld->fmrfb_cd;
	void *reg_base_virt = NULL;
	int i;
	bool logicvc_unmap = false;

	driver_devel("%s\n", __func__);

	if (cd->fmrfb_use_ref) {
		pr_err("Error fmrfb in use\n");
		return -EINVAL;
	}

	fmrfb_disable_logicvc_output(fbi);

	free_irq(cd->fmrfb_irq, dev);
	for (i = cd->fmrfb_layers-1; i >= 0; i--) {
		fbi = afbi[i];
		ld = fbi->par;

		if (!logicvc_unmap) {
			reg_base_virt = ld->reg_base_virt;
			logicvc_unmap = true;
		}
		unregister_framebuffer(fbi);
		fb_dealloc_cmap(&fbi->cmap);
		if (cd->fmrfb_flags & FMRFB_FLAG_DMA_BUFFER) {
			dma_free_coherent(dev, PAGE_ALIGN(fbi->fix.smem_len),
				ld->fb_virt, ld->fb_phys);
		} else {
			iounmap(ld->fb_virt);
		}
		if (!(cd->fmrfb_flags & LOGICVC_READABLE_REGS))
			kfree(ld->layer_reg_list);
		kfree(fbi->pseudo_palette);
		framebuffer_release(fbi);
	}

	if (reg_base_virt)
		iounmap(reg_base_virt);

	if (!(cd->fmrfb_flags & LOGICVC_READABLE_REGS))
		kfree(cd->reg_list);
	kfree(cd);
	kfree(afbi);

	dev_set_drvdata(dev, NULL);

	return 0;
}

#ifndef MODULE
int fmrfb_get_params(char *options)
{
	char *this_opt;

	driver_devel("%s\n", __func__);

	if (!options || !*options)
		return 0;

	while ((this_opt = strsep(&options, ",")) != NULL) {
		if (!*this_opt)
			continue;
		fmrfb_mode_option = this_opt;
	}
	return 0;
}
#endif
