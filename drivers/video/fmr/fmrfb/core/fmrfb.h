/*
 * Xylon logiCVC frame buffer driver internal data structures
 *
 * Author: Xylon d.o.o.
 * e-mail: davor.joja@logicbricks.com
 *
 * 2013 Xylon d.o.o.
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2.  This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

#ifndef __FMR_FB_DATA_H__
#define __FMR_FB_DATA_H__


#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/fb.h>
#include <linux/fmrfb.h>
#include "logicvc.h"


#define DRIVER_NAME "fmrfb"
#define DEVICE_NAME "logicvc"
#define DRIVER_DESCRIPTION "Fmr logiCVC frame buffer driver"
#define DRIVER_VERSION "0.1"

/* FmrFB driver flags */
#define FMRFB_FLAG_RESERVED_0x01     LOGICVC_READABLE_REGS
#define FMRFB_FLAG_DMA_BUFFER        0x02
#define FMRFB_FLAG_MEMORY_LE         0x04
#define FMRFB_FLAG_PIXCLK_VALID      0x08
#define FMRFB_FLAG_VMODE_INIT        0x10
#define FMRFB_FLAG_EDID_VMODE        0x20
#define FMRFB_FLAG_EDID_PRINT        0x40
#define FMRFB_FLAG_DEFAULT_VMODE_SET 0x80
#define FMRFB_FLAG_VMODE_SET         0x100
/*
	Following flags must be updated in fmrfb miscellaneous
	header files for every functionality specifically
*/
#define FMRFB_FLAG_MISC_ADV7511 0x1000
#define FMRFB_FLAG_ADV7511_SKIP 0x2000
#define FMRFB_FLAG_EDID_RDY     0x4000
#define FMRFB_EDID_SIZE         256
#define FMRFB_EDID_WAIT_TOUT    60


#ifdef DEBUG
#define driver_devel(format, ...) pr_info(format, ## __VA_ARGS__);
#else
#define driver_devel(format, ...) pr_info(format, ## __VA_ARGS__);
//#define driver_devel(format, ...)
#endif

struct fmrfb_layer_data;

#define VMODE_NAME_SZ (20+1)
#define VMODE_OPTS_SZ (2+1)
struct fmrfb_vmode_data {
	u32 ctrl_reg;
	struct fb_videomode fb_vmode;
	char fb_vmode_name[VMODE_NAME_SZ];
	char fb_vmode_opts_cvt[VMODE_OPTS_SZ];
	char fb_vmode_opts_ext[VMODE_OPTS_SZ];
};

struct fmrfb_registers {
	u32 ctrl_reg;
	u32 dtype_reg;
	u32 bg_reg;
	u32 unused_reg[3];
	u32 int_mask_reg;
};

struct fmrfb_layer_registers {
	u32 hoff_reg;
	u32 voff_reg;
	u32 hpos_reg;
	u32 vpos_reg;
	u32 width_reg;
	u32 height_reg;
	u32 alpha_reg;
	u32 ctrl_reg;
	u32 trans_reg;
};

struct fmrfb_register_access {
	u32 (*fmrfb_get_reg_val)
		(void *reg_base_virt, unsigned long offset,
		 struct fmrfb_layer_data *layer_data);
	void (*fmrfb_set_reg_val)
		(u32 value, void *reg_base_virt, unsigned long offset,
		 struct fmrfb_layer_data *layer_data);
};

struct fmrfb_layer_fix_data {
	unsigned int offset;
	unsigned short buffer_offset;
	unsigned short width;
	unsigned short height;
	unsigned char bpp;
	unsigned char bpp_virt;
	unsigned char layer_type;
	unsigned char alpha_mode;
	/* higher 4 bits: number of layer buffers, lower 4 bits: layer ID */
	unsigned char layer_fix_info;
};

struct fmrfb_sync {
	wait_queue_head_t wait;
	unsigned int cnt;
};

struct fmrfb_common_data {
	struct mutex irq_mutex;
	struct fmrfb_register_access reg_access;
	struct fmrfb_registers *reg_list;
	struct fmrfb_sync vsync;
	struct fmrfb_vmode_data vmode_data;
	struct fmrfb_vmode_data vmode_data_current;
	struct blocking_notifier_head fmrfb_notifier_list;
	struct notifier_block fmrfb_nb;
	/* Delay after applying display power and
		before applying display signals */
	unsigned int power_on_delay;
	/* Delay after applying display signal and
		before applying display backlight power supply */
	unsigned int signal_on_delay;
	unsigned long fmrfb_flags;
	unsigned char fmrfb_pixclk_src_id;
	unsigned char fmrfb_layers;
	unsigned char fmrfb_irq;
	unsigned char fmrfb_use_ref;
	unsigned char fmrfb_console_layer;
	unsigned char fmrfb_bg_layer_bpp;
	unsigned char fmrfb_bg_layer_alpha_mode;
	/* higher 4 bits: display interface
	   lower 4 bits: display color space */
	unsigned char fmrfb_display_interface_type;
#if defined(CONFIG_FB_FMR_MISC)
	struct fmrfb_misc_data *fmrfb_misc;
#endif
};

struct fmrfb_layer_data {
	struct fmrfb_common_data *fmrfb_cd;
	struct mutex layer_mutex;
	dma_addr_t reg_base_phys;
	dma_addr_t fb_phys;
	void *reg_base_virt;
	void *vtc_base_virt;
	void *clk_base_virt;
	void *fb_virt;
	unsigned long fb_size;
	void *layer_reg_base_virt;
	void *layer_clut_base_virt;
	struct fmrfb_layer_fix_data layer_fix;
	struct fmrfb_layer_registers *layer_reg_list;
	unsigned char layer_ctrl_flags;
	unsigned char layer_use_ref;
};

struct fmrfb_init_data {
	struct platform_device *pdev;
	struct fmrfb_vmode_data vmode_data;
	struct fmrfb_layer_fix_data lfdata[LOGICVC_MAX_LAYERS];
	unsigned long vmem_base_addr;
	unsigned long vmem_high_addr;
	unsigned long vtc_baseaddr;
	int vtc_size;
	unsigned long clk_baseaddr;
	int clk_size;
	unsigned char pixclk_src_id;
	unsigned char layer_ctrl_flags[LOGICVC_MAX_LAYERS];
	unsigned char layers;
	unsigned char active_layer;
	unsigned char bg_layer_bpp;
	unsigned char bg_layer_alpha_mode;
	unsigned char display_interface_type;
	unsigned short flags;
	bool vmode_params_set;
};


/* fmrfb core pixel clock interface functions */
extern bool fmrfb_hw_pixclk_supported(int);
extern int fmrfb_hw_pixclk_set(int, unsigned long);

/* fmrfb core interface functions */
extern int fmrfb_get_params(char *options);
extern int fmrfb_init_driver(struct fmrfb_init_data *init_data);
extern int fmrfb_deinit_driver(struct platform_device *pdev);
extern int fmrfb_ioctl(struct fb_info *fbi,
	unsigned int cmd, unsigned long arg);

#endif /* __FMR_FB_DATA_H__ */
