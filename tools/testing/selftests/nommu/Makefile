# SPDX-License-Identifier: GPL-2.0
# Makefile for NOMMU selftests
# Build and invoke this SWMMU test directly in the guest, not via the generic
# collection runner until its NOMMU timeout handling is available.

TEST_GEN_PROGS += nommu_swmmu_test
TEST_GEN_PROGS += nommu_swmmu_expand_test
TEST_GEN_PROGS += nommu_swmmu_direct_test
# Exercise raw instruction bytes and RX faults without helper interception.
TEST_GEN_PROGS += nommu_zpoline_test
TEST_GEN_PROGS += nommu_zpoline_native_test
TEST_GEN_PROGS += nommu_shared_file_test

$(OUTPUT)/nommu_swmmu_direct_test: CFLAGS += \
	-I$(top_srcdir)/include/uapi $(KHDR_INCLUDES) $(TOOLS_INCLUDES)

$(OUTPUT)/nommu_swmmu_test: nommu_swmmu_test.c nommu_swmmu_user.c
$(OUTPUT)/nommu_swmmu_expand_test: nommu_swmmu_expand_test.c nommu_swmmu_user.c
$(OUTPUT)/nommu_swmmu_test: CFLAGS += \
	-I$(top_srcdir)/include/uapi $(KHDR_INCLUDES) $(TOOLS_INCLUDES)
$(OUTPUT)/nommu_swmmu_expand_test: CFLAGS += \
	-I$(top_srcdir)/include/uapi $(KHDR_INCLUDES) $(TOOLS_INCLUDES)


# gcc-plugins
NOMMU_SELFTEST_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

SWMMU_SRCTREE := $(strip $(top_srcdir))
ifeq ($(SWMMU_SRCTREE),)
SWMMU_SRCTREE := $(abspath $(NOMMU_SELFTEST_DIR)/../../../..)
endif

SWMMU_BUILD_PLUGIN ?= 0
SWMMU_HOSTCXX ?= g++

SWMMU_PLUGIN_SOURCE := $(SWMMU_SRCTREE)/scripts/gcc-plugins/swmmu_plugin.c
SWMMU_PLUGIN_HEADERS := $(wildcard $(SWMMU_SRCTREE)/scripts/gcc-plugins/*.h)

ifeq ($(SWMMU_BUILD_PLUGIN),1)

SWMMU_PLUGIN ?= $(OUTPUT)/swmmu_plugin.so
SWMMU_GCC_PLUGIN_DIR := \
	$(shell $(SWMMU_HOSTCXX) -print-file-name=plugin)
SWMMU_PLUGIN_INCLUDE := \
	$(SWMMU_GCC_PLUGIN_DIR)/include

$(SWMMU_PLUGIN): $(SWMMU_PLUGIN_SOURCE) $(SWMMU_PLUGIN_HEADERS)
	@test -d "$(SWMMU_PLUGIN_INCLUDE)" || { \
		echo "error: GCC plugin headers not found:" \
			"$(SWMMU_PLUGIN_INCLUDE)" >&2; \
		exit 1; \
	}
	@mkdir -p "$(dir $@)"
	$(SWMMU_HOSTCXX) \
		-shared \
		-fPIC \
		-fno-rtti \
		-fno-exceptions \
		-fno-lto \
		-std=gnu++11 \
		-I"$(SWMMU_PLUGIN_INCLUDE)" \
		-I"$(SWMMU_SRCTREE)/scripts/gcc-plugins" \
		-o "$@" \
		"$(SWMMU_PLUGIN_SOURCE)"

# Build the plugin first without adding the shared object to $^; lib.mk passes
# $^ to the linker, where the compiler plugin is not a link input.
$(OUTPUT)/nommu_swmmu_test $(OUTPUT)/nommu_swmmu_expand_test: | $(SWMMU_PLUGIN)

endif

ifneq ($(strip $(SWMMU_PLUGIN)),)
$(OUTPUT)/nommu_swmmu_test $(OUTPUT)/nommu_swmmu_expand_test: CFLAGS += -fplugin=$(SWMMU_PLUGIN)
endif

SWMMU_ALL_ACCESS ?= 0
ifeq ($(SWMMU_ALL_ACCESS),1)
$(OUTPUT)/nommu_swmmu_test $(OUTPUT)/nommu_swmmu_expand_test: CFLAGS += -fplugin-arg-swmmu_plugin-all-access=1
endif

SWMMU_RETURN_VALUE_LOAD ?= 0
ifeq ($(SWMMU_RETURN_VALUE_LOAD),1)
$(OUTPUT)/nommu_swmmu_test $(OUTPUT)/nommu_swmmu_expand_test: CFLAGS += \
	-fplugin-arg-swmmu_plugin-return-value-load=1
endif

.PHONY: swmmu-plugin

swmmu-plugin:
ifeq ($(SWMMU_BUILD_PLUGIN),1)
	$(MAKE) $(SWMMU_PLUGIN)
else
	@echo "set SWMMU_BUILD_PLUGIN=1 to build the SWMMU GCC plugin"
	@false
endif

include ../lib.mk
