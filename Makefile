obj-m := fft_driver.o

all:
	make -C $(KERN_SRC) ARCH=arm CROSS_COMPILE=$(CROSS_COMPILE) M=`pwd` modules
clean:
	make -C $(KERN_SRC) ARCH=arm CROSS_COMPILE=$(CROSS_COMPILE) M=`pwd` clean
