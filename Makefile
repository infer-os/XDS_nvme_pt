KSRC ?= /lib/modules/$(shell uname -r)/build

# Kernel flavor that selects compat.h: vanilla (default), velinux, ubuntu or
# openeuler. The flavor also applies to recursive module builds, so pass it on
# the command line or through the environment.
KERNEL_FLAVOR ?= vanilla
export KERNEL_FLAVOR

ifeq ($(KERNEL_FLAVOR),vanilla)
# Vanilla default: no compat flavor macro.
else ifeq ($(KERNEL_FLAVOR),velinux)
ccflags-y += -DP2P_COMPAT_VELINUX
else ifeq ($(KERNEL_FLAVOR),ubuntu)
ccflags-y += -DP2P_COMPAT_UBUNTU
else ifeq ($(KERNEL_FLAVOR),openeuler)
ccflags-y += -DP2P_COMPAT_OPENEULER
else
$(error unknown KERNEL_FLAVOR "$(KERNEL_FLAVOR)", expected vanilla, velinux, ubuntu or openeuler)
endif

.PHONY: all mod mod_vanilla mod_velinux mod_ubuntu mod_openeuler lib binding clean test

all: mod lib

mod:
	$(MAKE) M=$(shell pwd) -C $(KSRC) modules

mod_vanilla:
	$(MAKE) mod KERNEL_FLAVOR=vanilla

mod_velinux:
	$(MAKE) mod KERNEL_FLAVOR=velinux

mod_ubuntu:
	$(MAKE) mod KERNEL_FLAVOR=ubuntu

mod_openeuler:
	$(MAKE) mod KERNEL_FLAVOR=openeuler

lib:
	$(MAKE) -C file_p2p

binding:
	$(MAKE) -C file_p2p binding

obj-m := stub.o
obj-m += p2p_dev.o

p2p_dev-objs := dev.o topo.o debugfs.o mem.o

test:
	$(MAKE) -C test

clean:
	rm -rf *.o *.ko *.mod.c *.mod.o *.mod modules.* Module.* .*.ko.cmd .*.mod.o.cmd .*.o.cmd
	rm -rf .*.mod.cmd .tmp_versions/
	$(MAKE) -C file_p2p clean
	$(MAKE) -C test clean
