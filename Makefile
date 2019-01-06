KERN_SRC=../../linux-xlnx

obj-m += si5326.o

all:
	make -C $(KERN_SRC) ARCH=arm M=`pwd` modules

zynq:
	./make.zynq

clean:
	rm -f *.ko *.o *.mod.* modules.order Module.symvers


