DEFAULTGXX:=aarch64-linux-gnu-g++
DEFAULTGCC:=aarch64-linux-gnu-gcc
toolchain?=clang
ARCHFLAGS:=-fPIC -fno-omit-frame-pointer -I$(BUILD)/$(ARCH)/osdeps/usr/include -march=armv8.2-a -mcpu=neoverse-n1 --target=aarch64-unknown-linux-gnu

# Valgrind has illegal instructions on aarch64
VALGRIND:=
VALGRINDFLAGS:=

#PORT_FS_BASE?=/home/$(USER)/64_TX1/Linux_for_Tegra_64_tx1/rootfs/
#PORT_LIBRARY_DIRS := \
	$(BUILD)/$(ARCH)/osdeps/usr/lib/aarch64-linux-gnu \
	$(BUILD)/$(ARCH)/osdeps/usr/lib \
	$(BUILD)/$(ARCH)/osdeps/lib/aarch64-linux-gnu \
	$(BUILD)/$(ARCH)/osdeps/usr/lib/lapack \
	$(BUILD)/$(ARCH)/osdeps/usr/lib/libblas \
	/usr/local/cuda-8.0/targets/aarch64-linux/lib

#PORT_LINK_FLAGS:=$(foreach dir,$(PORT_LIBRARY_DIRS), -L$(dir) -Wl,--rpath,$(dir)) 	-L/usr/local/cuda-8.0/targets/aarch64-linux/lib/stubs

#CUDA_ARCH_INCLUDE_DIR:=/usr/local/cuda-8.0/targets/aarch64-linux/include

#TENSORFLOW_ARCH_CUDA_FLAGS:=-m64

TCMALLOC_ENABLED:=0
#port:=ubuntu1404
