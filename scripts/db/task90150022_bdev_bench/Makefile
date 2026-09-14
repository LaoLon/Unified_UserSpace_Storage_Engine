SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../spdk)

include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = task90150022_bdev_bench
C_SRCS := task90150022_bdev_bench.c

CFLAGS += -O2 -g -Wall -Wextra -Wno-unused-parameter

SPDK_LIB_LIST = $(ALL_MODULES_LIST) event event_bdev

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
