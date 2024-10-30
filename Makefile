obj-m := p128.o
p128-objs := p128_drv.o hvc_p128.o hvc_p128_asm.o

MY_CFLAGS += -g -DDEBUG
ccflags-y += ${MY_CFLAGS}

SRC := $(shell pwd)

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC)

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) modules_install

clean:
	rm -f *.o *~ core .depend .*.cmd *.ko *.mod.c
	rm -f Module.markers Module.symvers modules.order
	rm -rf .tmp_versions Modules.symvers
