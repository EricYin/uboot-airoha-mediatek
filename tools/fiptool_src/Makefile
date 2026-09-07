#
# Copyright (c) 2014-2026, Arm Limited and Contributors.
#
# SPDX-License-Identifier: BSD-3-Clause
#

PROJECT     := fiptool
CC          ?= cc
STATIC      ?= 0
DEBUG       ?= 0
PLAT        ?=

SRC_DIR     := .
TOOLS_SHARE  := include/tools_share

SOURCES     := fiptool.c tbbr_config.c
INCLUDES    := -I. -I$(TOOLS_SHARE) -Iinclude
CPPFLAGS    += -D_GNU_SOURCE -D_XOPEN_SOURCE=700
CFLAGS      += -Wall -Werror -pedantic -std=c99

ifeq ($(DEBUG),1)
	CFLAGS    += -g -O0 -DDEBUG
else
	CFLAGS    += -O2
endif

ifeq ($(STATIC),1)
	LDFLAGS   += -static
endif

LDLIBS      += -lcrypto

ifeq ($(OS),Windows_NT)
	SOURCES   += win_posix.c
endif

ifneq ($(strip $(ARCH_AIROHA)),)
  HOSTCCFLAGS += -DCONFIG_AIROHA
endif

ifneq ($(PLAT),)
	CPPFLAGS  += -DPLAT_DEF_FIP_UUID
	PLATFORM_HEADER_DIR :=

	ifeq ($(PLAT),juno)
		SOURCES += plat_fiptool/arm/board/juno/plat_def_uuid_config.c
		PLATFORM_HEADER_DIR := plat_fiptool/arm/board/juno
		ifeq ($(ETHOSN_NPU_TZMP1),1)
			CPPFLAGS += -DETHOSN_NPU_TZMP1
		endif
	else ifeq ($(PLAT),tc)
		SOURCES += plat_fiptool/arm/board/tc/plat_def_uuid_config.c
		PLATFORM_HEADER_DIR := plat_fiptool/arm/board/tc
	else ifeq ($(PLAT),nxp)
		SOURCES += plat_fiptool/nxp/plat_def_uuid_config.c
		PLATFORM_HEADER_DIR := plat_fiptool/nxp
		CPPFLAGS += -DPLAT_DEF_OID
	else ifeq ($(PLAT),s32g274ardb2)
		SOURCES += plat_fiptool/nxp/s32/s32g274ardb2/plat_def_uuid_config.c
		PLATFORM_HEADER_DIR := plat_fiptool/nxp/s32/s32g274ardb2
	else ifeq ($(PLAT),st)
		SOURCES += plat_fiptool/st/plat_def_uuid_config.c
		PLATFORM_HEADER_DIR := plat_fiptool/st
	else
		$(error Unsupported PLAT='$(PLAT)'. Use juno, tc, nxp, s32g274ardb2, or st)
	endif

	INCLUDES += -I$(PLATFORM_HEADER_DIR)
endif

TARGET := $(PROJECT)

.PHONY: all clean distclean

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

clean:
	rm -f $(TARGET)

distclean: clean
