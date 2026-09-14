SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../spdk)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = task90150022_blob_bench
C_SRCS := task90150022_blob_bench.c
SPDK_LIB_LIST = $(ALL_MODULES_LIST) event event_bdev

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
