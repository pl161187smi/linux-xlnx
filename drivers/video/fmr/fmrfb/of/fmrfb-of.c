/* fmrfb-of.c */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/errno.h>
#include <linux/of.h>
#include "../core/fmrfb.h"
#include <linux/of_gpio.h>

#if 1
static int pwr_en_gpio = ~0;
static int bl_en_gpio = ~0;

static void fmrfb_set_ctrl_reg(struct fmrfb_init_data *init_data,
	unsigned long pix_data_invert, unsigned long pix_clk_act_high)
{
	u32 sync = init_data->vmode_data.fb_vmode.sync;
	u32 ctrl = CTRL_REG_INIT;

	driver_devel("%s\n", __func__);

	/* FB_SYNC_HOR_HIGH_ACT */
	if (!(sync & (1<<0)))
		ctrl &= (~(1<<1));
	/* FB_SYNC_VERT_HIGH_ACT */
	if (!(sync & (1<<1)))
		ctrl &= (~(1<<3));
	if (pix_data_invert)
		ctrl |= LOGICVC_PIX_DATA_INVERT;
	if (pix_clk_act_high)
		ctrl |= LOGICVC_PIX_ACT_HIGH;

	init_data->vmode_data.ctrl_reg = ctrl;
}

static int fmrfb_parse_hw_info(struct device_node *np,
	struct fmrfb_init_data *init_data)
{
	u32 const *prop;
	int size;

	driver_devel("%s\n", __func__);

	prop = of_get_property(np, "fmr,display-interface", &size);
	if (!prop) {
		pr_err("Error fmrfb getting display interface\n");
		return -EINVAL;
	}
	init_data->display_interface_type = be32_to_cpup(prop) << 4;

	prop = of_get_property(np, "fmr,display-color-space", &size);
	if (!prop) {
		pr_err("Error fmrfb getting display color space\n");
		return -EINVAL;
	}
	init_data->display_interface_type |= be32_to_cpup(prop);

	prop = of_get_property(np, "fmr,readable-regs", &size);
	if (!prop) {
		pr_warn("fmrfb registers not readable\n");
	} else {
		if (be32_to_cpup(prop))
			init_data->flags |= LOGICVC_READABLE_REGS;
	}

	prop = of_get_property(np, "fmr,vtc-baseaddr", &size);
	if (!prop) {
		pr_warn("Error fmrfb getting vtc baseaddr\n");
	} else {
		if (be32_to_cpup(prop))
			init_data->vtc_baseaddr = be32_to_cpup(prop);
	}

	prop = of_get_property(np, "fmr,vtc-size", &size);
	if (!prop) {
		pr_warn("Error fmrfb getting vtc size\n");
	} else {
		if (be32_to_cpup(prop))
			init_data->vtc_size = be32_to_cpup(prop);
	}

	prop = of_get_property(np, "fmr,clk-baseaddr", &size);
	if (!prop) {
		pr_warn("Error fmrfb getting clk baseaddr\n");
	} else {
		if (be32_to_cpup(prop))
			init_data->clk_baseaddr = be32_to_cpup(prop);
	}

	prop = of_get_property(np, "fmr,clk-size", &size);
	if (!prop) {
		pr_warn("Error fmrfb getting clk size\n");
	} else {
		if (be32_to_cpup(prop))
			init_data->clk_size = be32_to_cpup(prop);
	}

	return 0;
}

static int fmrfb_parse_vram_info(struct device_node *np,
	unsigned long *vmem_base_addr, unsigned long *vmem_high_addr)
{
	u32 const *prop;
	int size;

	driver_devel("%s\n", __func__);

	prop = of_get_property(np, "fmr,vmem-baseaddr", &size);
	if (!prop) {
		pr_err("Error fmrfb getting VRAM address begin\n");
		return -EINVAL;
	}
	*vmem_base_addr = be32_to_cpup(prop);

	prop = of_get_property(np, "fmr,vmem-highaddr", &size);
	if (!prop) {
		pr_err("Error fmrfb getting VRAM address end\n");
		return -EINVAL;
	}
	*vmem_high_addr = be32_to_cpup(prop);

	return 0;
}

static int fmrfb_parse_layer_info(struct device_node *np,
	struct fmrfb_init_data *init_data)
{
	u32 const *prop;
	unsigned int layers, bg_bpp, bg_alpha_mode;
	int size;
	char bg_layer_name[25];

	driver_devel("%s\n", __func__);

	prop = of_get_property(np, "fmr,num-of-layers", &size);
	if (!prop) {
		pr_err("Error getting number of layers\n");
		return -EINVAL;
	}
	layers = be32_to_cpup(prop);

	bg_bpp = 0;
	bg_alpha_mode = 0;
	prop = of_get_property(np, "fmr,use-background", &size);
	if (!prop) {
		pr_warn("fmrfb no BG layer\n");
	} else {
		if (be32_to_cpup(prop) == 1) {
			layers--;

			sprintf(bg_layer_name, "fmr,layer-%d-data-width", layers);
			prop = of_get_property(np, bg_layer_name, &size);
			if (!prop)
				bg_bpp = 16;
			else
				bg_bpp = be32_to_cpup(prop);
			if (bg_bpp == 24)
				bg_bpp = 32;

			sprintf(bg_layer_name, "fmr,layer-%d-alpha-mode", layers);
			prop = of_get_property(np, bg_layer_name, &size);
			if (!prop)
				bg_alpha_mode = LOGICVC_LAYER_ALPHA;
			else
				bg_alpha_mode = be32_to_cpup(prop);
		} else {
			pr_debug("fmrfb no BG layer\n");
		}
	}

	init_data->layers = (unsigned char)layers;
	init_data->bg_layer_bpp = (unsigned char)bg_bpp;
	init_data->bg_layer_alpha_mode = (unsigned char)bg_alpha_mode;

	return 0;
}

static int fmrfb_parse_vmode_info(struct device_node *np,
	struct fmrfb_init_data *init_data)
{
	struct device_node *dn, *vmode_np;
	u32 const *prop;
	char *c;
	unsigned long pix_data_invert, pix_clk_act_high;
	int size, tmp;

	driver_devel("%s\n", __func__);

	vmode_np = NULL;
	init_data->vmode_data.fb_vmode.refresh = 60;
	init_data->active_layer = 0;
	init_data->vmode_params_set = false;

	prop = of_get_property(np, "pixel-clock-source", &size);
	if (!prop) {
		pr_info("No pixel clock source\n");
		init_data->pixclk_src_id = 0;
	} else {
		tmp = be32_to_cpup(prop);
		init_data->pixclk_src_id = (u16)tmp;
	}
	pix_data_invert = 0;
	prop = of_get_property(np, "pixel-data-invert", &size);
	if (!prop)
		pr_err("Error getting pixel data invert\n");
	else
		pix_data_invert = be32_to_cpup(prop);
	pix_clk_act_high = 0;
	prop = of_get_property(np, "pixel-clock-active-high", &size);
	if (!prop)
		pr_err("Error getting pixel active edge\n");
	else
		pix_clk_act_high = be32_to_cpup(prop);

	prop = of_get_property(np, "pixel-component-format", &size);
	if (prop) {
		if (!strcmp("ABGR", (char *)prop)) {
			prop = of_get_property(np, "pixel-component-layer", &size);
			if (prop) {
				while (size > 0) {
					tmp = be32_to_cpup(prop);
					init_data->layer_ctrl_flags[tmp] = LOGICVC_SWAP_RB;
					prop++;
					size -= sizeof(prop);
				}
			}
		}
	}

	prop = of_get_property(np, "active-layer", &size);
	if (prop) {
		tmp = be32_to_cpup(prop);
		init_data->active_layer = (unsigned char)tmp;
	} else {
		pr_info("fmrfb setting default layer to %d\n",
			init_data->active_layer);
	}

	dn = of_get_child_by_name(np, "edid");
	if (dn) {
		prop = of_get_property(dn, "preffered-videomode", &size);
		if (prop) {
			tmp = be32_to_cpup(prop);
			if (tmp)
				init_data->flags |= FMRFB_FLAG_EDID_VMODE;
		}
		prop = of_get_property(dn, "display-data", &size);
		if (prop) {
			tmp = be32_to_cpup(prop);
			if (tmp)
				init_data->flags |= FMRFB_FLAG_EDID_PRINT;
		}
	}

	of_node_put(dn);

	prop = of_get_property(np, "videomode", &size);
	if (prop) {
		if (strlen((char *)prop) <= VMODE_NAME_SZ) {
			dn = NULL;
			dn = of_find_node_by_name(NULL, "fmr-video-params");
			if (dn) {
				strcpy(init_data->vmode_data.fb_vmode_name,
					(char *)prop);
				vmode_np = of_find_node_by_name(dn,
					init_data->vmode_data.fb_vmode_name);
				c = strchr((char *)prop, '_');
				if (c)
					*c = 0;
				strcpy(init_data->vmode_data.fb_vmode_name, (char *)prop);
			} else {
				strcpy(init_data->vmode_data.fb_vmode_name, (char *)prop);
			}
			of_node_put(dn);
		} else {
			pr_err("Error videomode name to long\n");
		}
		if (vmode_np) {
			pwr_en_gpio = of_get_named_gpio_flags(vmode_np, "pwr_en", 0, NULL);
			if (gpio_is_valid(pwr_en_gpio)) {		
				if (gpio_request(pwr_en_gpio, "power en")) {
					printk("request lcd power en gpio[%d] err\n", pwr_en_gpio);
					pwr_en_gpio = ~0;
				}else {
					gpio_direction_output(pwr_en_gpio, 1);
					gpio_export(pwr_en_gpio, 0);					
				}
			}
			bl_en_gpio = of_get_named_gpio_flags(vmode_np, "bl_en", 0, NULL);
			if (gpio_is_valid(bl_en_gpio)) {		
				if (gpio_request(bl_en_gpio, "bl en")) {
					printk("request lcd bl en gpio[%d] err\n", bl_en_gpio);
					bl_en_gpio = ~0;
				}else {
					gpio_direction_output(bl_en_gpio, 1);
					gpio_export(bl_en_gpio, 0);						
				}
			}
			
			prop = of_get_property(vmode_np, "refresh", &size);
			if (!prop)
				pr_err("Error getting refresh rate\n");
			else
				init_data->vmode_data.fb_vmode.refresh =
					be32_to_cpup(prop);

			prop = of_get_property(vmode_np, "xres", &size);
			if (!prop)
				pr_err("Error getting xres\n");
			else
				init_data->vmode_data.fb_vmode.xres =
					be32_to_cpup(prop);

			prop = of_get_property(vmode_np, "yres", &size);
			if (!prop)
				pr_err("Error getting yres\n");
			else
				init_data->vmode_data.fb_vmode.yres =
					be32_to_cpup(prop);

			prop = of_get_property(vmode_np, "pixclock-khz", &size);
			if (!prop)
				pr_err("Error getting pixclock-khz\n");
			else
				init_data->vmode_data.fb_vmode.pixclock =
					KHZ2PICOS(be32_to_cpup(prop));

			prop = of_get_property(vmode_np, "left-margin", &size);
			if (!prop)
				pr_err("Error getting left-margin\n");
			else
				init_data->vmode_data.fb_vmode.left_margin =
					be32_to_cpup(prop);

			prop = of_get_property(vmode_np, "right-margin", &size);
			if (!prop)
				pr_err("Error getting right-margin\n");
			else
				init_data->vmode_data.fb_vmode.right_margin =
					be32_to_cpup(prop);

			prop = of_get_property(vmode_np, "upper-margin", &size);
			if (!prop)
				pr_err("Error getting upper-margin\n");
			else
				init_data->vmode_data.fb_vmode.upper_margin =
					be32_to_cpup(prop);

			prop = of_get_property(vmode_np, "lower-margin", &size);
			if (!prop)
				pr_err("Error getting lower-margin\n");
			else
				init_data->vmode_data.fb_vmode.lower_margin =
					be32_to_cpup(prop);

			prop = of_get_property(vmode_np, "hsync-len", &size);
			if (!prop)
				pr_err("Error getting hsync-len\n");
			else
				init_data->vmode_data.fb_vmode.hsync_len =
					be32_to_cpup(prop);

			prop = of_get_property(vmode_np, "vsync-len", &size);
			if (!prop)
				pr_err("Error getting vsync-len\n");
			else
				init_data->vmode_data.fb_vmode.vsync_len =
					be32_to_cpup(prop);

			prop = of_get_property(vmode_np, "sync", &size);
			if (!prop)
				pr_err("Error getting sync\n");
			else
				init_data->vmode_data.fb_vmode.sync =
					be32_to_cpup(prop);

			prop = of_get_property(vmode_np, "vmode", &size);
			if (!prop)
				pr_err("Error getting vmode\n");
			else
				init_data->vmode_data.fb_vmode.vmode =
					be32_to_cpup(prop);

			init_data->vmode_params_set = true;
		}
	} else {
		pr_info("fmrfb using default driver video mode\n");
	}

	fmrfb_set_ctrl_reg(init_data, pix_data_invert, pix_clk_act_high);

	return 0;
}

static int fmrfb_parse_layer_params(struct device_node *np,
	int id, struct fmrfb_layer_fix_data *lfdata)
{
	u32 const *prop;
	int size;
	char layer_property_name[25];

	driver_devel("%s\n", __func__);

	sprintf(layer_property_name, "fmr,layer-%d-offset", id);
	prop = of_get_property(np, layer_property_name, &size);
	if (!prop) {
		pr_err("Error getting layer offset\n");
		return -EINVAL;
	} else {
		lfdata->offset = be32_to_cpup(prop);
	}

	sprintf(layer_property_name, "fmr,buffer-%d-offset", id);
	prop = of_get_property(np, layer_property_name, &size);
	if (!prop) {
		pr_err("Error getting buffer offset\n");
		return -EINVAL;
	} else {
		lfdata->buffer_offset = be32_to_cpup(prop);
	}

	prop = of_get_property(np, "fmr,row-stride", &size);
	if (!prop)
		lfdata->width = 1024;
	else
		lfdata->width = be32_to_cpup(prop);

	sprintf(layer_property_name, "fmr,layer-%d-type", id);
	prop = of_get_property(np, layer_property_name, &size);
	if (!prop) {
		pr_err("Error getting layer type\n");
		return -EINVAL;
	} else {
		lfdata->layer_type = be32_to_cpup(prop);
	}

	sprintf(layer_property_name, "fmr,layer-%d-alpha-mode", id);
	prop = of_get_property(np, layer_property_name, &size);
	if (!prop) {
		pr_err("Error getting layer alpha mode\n");
		return -EINVAL;
	} else {
		lfdata->alpha_mode = be32_to_cpup(prop);
		/* If logiCVC layer is Alpha layer, override DT value */
		if (lfdata->layer_type == LOGICVC_ALPHA_LAYER)
			lfdata->alpha_mode = LOGICVC_LAYER_ALPHA;
	}

	sprintf(layer_property_name, "fmr,layer-%d-data-width", id);
	prop = of_get_property(np, layer_property_name, &size);
	if (!prop)
		lfdata->bpp = 16;
	else
		lfdata->bpp = be32_to_cpup(prop);
	if (lfdata->bpp == 24)
		lfdata->bpp = 32;

	lfdata->bpp_virt = lfdata->bpp;

	switch (lfdata->bpp) {
	case 8:
		if (lfdata->alpha_mode == LOGICVC_PIXEL_ALPHA)
			lfdata->bpp = 16;
		break;
	case 16:
		if (lfdata->alpha_mode == LOGICVC_PIXEL_ALPHA)
			lfdata->bpp = 32;
		break;
	}

	lfdata->layer_fix_info = id;

	return 0;
}
#endif

static int fmrfb_of_probe(struct platform_device *pdev)
{
	struct fmrfb_init_data init_data;
	int i, rc;

	driver_devel("%s\n", __func__);

	memset(&init_data, 0, sizeof(struct fmrfb_init_data));

	init_data.pdev = pdev;

	rc = fmrfb_parse_hw_info(pdev->dev.of_node, &init_data);
	if (rc)
		return rc;
	rc = fmrfb_parse_vram_info(pdev->dev.of_node,
		&init_data.vmem_base_addr, &init_data.vmem_high_addr);
	if (rc)
		return rc;
	rc = fmrfb_parse_layer_info(pdev->dev.of_node, &init_data);
	if (rc)
		return rc;
	/* if Device-Tree contains video mode options do not use
	   kernel command line video mode options */
	fmrfb_parse_vmode_info(pdev->dev.of_node, &init_data);

	for (i = 0; i < init_data.layers; i++) {
		rc = fmrfb_parse_layer_params(pdev->dev.of_node, i,
			&init_data.lfdata[i]);
		if (rc)
			return rc;
	}

	return fmrfb_init_driver(&init_data);
}

static int fmrfb_of_remove(struct platform_device *pdev)
{
	driver_devel("%s\n", __func__);

	return fmrfb_deinit_driver(pdev);
}


static struct of_device_id fmrfb_of_match[] = {
	{ .compatible = "fmr,logicvc-1.00.a" },
	{/* end of table */},
};
MODULE_DEVICE_TABLE(of, fmrfb_of_match);


static struct platform_driver fmrfb_of_driver = {
	.probe = fmrfb_of_probe,
	.remove = fmrfb_of_remove,
	.driver = {
		.owner = THIS_MODULE,
		.name = DEVICE_NAME,
		.of_match_table = fmrfb_of_match,
	},
};


static int fmrfb_of_init(void)
{
#ifndef MODULE
	char *option = NULL;
	/*
	 *  For kernel boot options (in 'video=xxxfb:<options>' format)
	 */
	if (fb_get_options(DRIVER_NAME, &option))
		return -ENODEV;
	/* Set internal module parameters */
	fmrfb_get_params(option);
#endif
	if (platform_driver_register(&fmrfb_of_driver)) {
		pr_err("Error fmrfb driver registration\n");
		return -ENODEV;
	}

	return 0;
}

static void __exit fmrfb_of_exit(void)
{
	platform_driver_unregister(&fmrfb_of_driver);
}

#ifndef MODULE
late_initcall(fmrfb_of_init);
#else
module_init(fmrfb_of_init);
module_exit(fmrfb_of_exit);
#endif

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);
