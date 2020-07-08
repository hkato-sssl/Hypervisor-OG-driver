KERNEL_SRC = ~/xilinx/linux-xlnx

obj-m := p128.o
p128-objs := p128_drv.o hvc_p128.o hvc_p128_asm.o

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) clean
