obj-y += ../core/

fmrfb_of-objs := fmrfb-of.o
ifeq ($(CONFIG_FB_FMR),m)
fmrfb_of-objs += ../core/fmrfb_core.o
endif
obj-$(CONFIG_FB_FMR) += fmrfb_of.o
