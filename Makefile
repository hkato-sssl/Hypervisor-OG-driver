KERNEL_SRC = ~/xilinx/linux-xlnx

obj-m := hvc_p128.o hvc_p128_asm.o
sample-objs :=

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) clean
