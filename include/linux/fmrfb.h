/*
 * Xylon logiCVC frame buffer driver IOCTL parameters
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

#ifndef __FMR_FB_H__
#define __FMR_FB_H__


#include <linux/types.h>


struct fmrfb_layer_color {
    __u32 raw_rgb;
    __u8 use_raw;
    __u8 r;
    __u8 g;
    __u8 b;
};

struct fmrfb_layer_pos_size {
    __u16 x;
    __u16 y;
    __u16 width;
    __u16 height;
};

struct fmrfb_hw_access {
    __u32 offset;
    __u32 value;
};

/* FmrFB events */
#define FMRFB_EVENT_FBI_UPDATE 0x01

/* FmrFB IOCTL's */
#define FMRFB_IOW(num, dtype)  _IOW('x', num, dtype)
#define FMRFB_IOR(num, dtype)  _IOR('x', num, dtype)
#define FMRFB_IOWR(num, dtype) _IOWR('x', num, dtype)
#define FMRFB_IO(num)          _IO('x', num)

#define FMRFB_GET_LAYER_IDX           FMRFB_IOR(30, unsigned int)
#define FMRFB_GET_LAYER_ALPHA         FMRFB_IOR(31, unsigned int)
#define FMRFB_SET_LAYER_ALPHA         FMRFB_IOW(32, unsigned int)
#define FMRFB_LAYER_COLOR_TRANSP      FMRFB_IOW(33, unsigned int)
#define FMRFB_GET_LAYER_COLOR_TRANSP \
    FMRFB_IOR(34, struct fmrfb_layer_color)
#define FMRFB_SET_LAYER_COLOR_TRANSP \
    FMRFB_IOW(35, struct fmrfb_layer_color)
#define FMRFB_GET_LAYER_SIZE_POS \
    FMRFB_IOR(36, struct fmrfb_layer_pos_size)
#define FMRFB_SET_LAYER_SIZE_POS \
    FMRFB_IOW(37, struct fmrfb_layer_pos_size)
#define FMRFB_GET_LAYER_BUFFER        FMRFB_IOR(38, unsigned int)
#define FMRFB_SET_LAYER_BUFFER        FMRFB_IOW(39, unsigned int)
#define FMRFB_GET_LAYER_BUFFER_OFFSET FMRFB_IOR(40, unsigned int)
#define FMRFB_GET_LAYER_BUFFERS_NUM   FMRFB_IOR(41, unsigned int)
#define FMRFB_GET_BACKGROUND_COLOR \
    FMRFB_IOR(42, struct fmrfb_layer_color)
#define FMRFB_SET_BACKGROUND_COLOR \
    FMRFB_IOW(43, struct fmrfb_layer_color)
#define FMRFB_LAYER_EXT_BUFF_SWITCH   FMRFB_IOW(43, unsigned int)
#define FMRFB_READ_HW_REG \
    FMRFB_IOR(44, struct fmrfb_hw_access)
#define FMRFB_WRITE_HW_REG \
    FMRFB_IOW(45, struct fmrfb_hw_access)
#define FMRFB_WAIT_EDID               FMRFB_IOW(46, unsigned int)
#define FMRFB_GET_EDID                FMRFB_IOR(47, char)

#endif /* __FMR_FB_H__ */
