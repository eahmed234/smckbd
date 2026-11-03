# SPDX-License-Identifier: GPL-2.0-or-later

obj-m      += smckbd.o
KVER       ?= `uname -r`
KBASE      ?= /lib/modules/$(KVER)
KBUILD_DIR ?= $(KBASE)/build

# CachyOS/Arch kernels are LLVM-built; kbuild defaults to gcc.
CLANG_KERNEL := $(shell grep -qs '^CONFIG_CC_IS_CLANG=y' \
		$(KBUILD_DIR)/include/config/auto.conf && echo 1)
ifeq ($(CLANG_KERNEL),1)
TOOLCHAIN ?= LLVM=1
endif

all:
	$(MAKE) -C $(KBUILD_DIR) M=$(CURDIR) $(TOOLCHAIN)

# Userspace helper for the F5/F6 illumination keys.
keys: smckbd-keys

smckbd-keys: smckbd-keys.c
	$(CC) -O2 -Wall -Wextra -o $@ $<

clean:
	$(MAKE) -C $(KBUILD_DIR) M=$(CURDIR) clean
	rm -f smckbd-keys
