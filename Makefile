PROJECT_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD_DIR ?= $(PROJECT_ROOT)/build
CXX ?= g++
SPDK_ROOT_DIR ?= $(PROJECT_ROOT)/spdk

# HFS 2.0 build: C++20 core. SPDK is auto-detected; without it the runtime
# layer returns NotSupported and the SPDK-dependent tests are skipped.
CXXFLAGS ?= -std=c++20 -O2 -g
CXXFLAGS += -Wall -Wextra -Wno-unused-parameter -fno-exceptions -fno-rtti \
            -fvisibility=hidden -fvisibility-inlines-hidden -fPIC -DHFS_CORE_ABI_MAJOR=2
INCLUDES := -I$(PROJECT_ROOT)/include -I$(PROJECT_ROOT)/src

HFS_WITH_SPDK := $(if $(and $(wildcard $(SPDK_ROOT_DIR)/include/spdk/init.h),$(wildcard $(SPDK_ROOT_DIR)/build/lib)),1,0)
ifeq ($(HFS_WITH_SPDK),1)
CXXFLAGS += -DHFS_WITH_SPDK=1
INCLUDES += -I$(SPDK_ROOT_DIR)/include
endif

CORE_SRC := $(sort $(wildcard $(PROJECT_ROOT)/src/common/*.cpp) \
                   $(wildcard $(PROJECT_ROOT)/src/config/*.cpp) \
                   $(wildcard $(PROJECT_ROOT)/src/core/*.cpp) \
                   $(wildcard $(PROJECT_ROOT)/src/metadata/*.cpp) \
                   $(wildcard $(PROJECT_ROOT)/src/runtime/*.cpp) \
                   $(wildcard $(PROJECT_ROOT)/src/storage/*.cpp))
CORE_OBJ := $(patsubst $(PROJECT_ROOT)/src/%.cpp,$(BUILD_DIR)/obj/%.o,$(CORE_SRC))
LIB := $(BUILD_DIR)/lib/libhfs_core.so
HFSCTL := $(BUILD_DIR)/bin/hfsctl
TEST_SRC := $(sort $(wildcard $(PROJECT_ROOT)/tests/*_test.cpp))
ifeq ($(HFS_WITH_SPDK),0)
TEST_SRC := $(filter-out $(PROJECT_ROOT)/tests/engine_lifecycle_test.cpp \
                         $(PROJECT_ROOT)/tests/spdk_runtime_test.cpp \
                         $(PROJECT_ROOT)/tests/format_mount_test.cpp \
                         $(PROJECT_ROOT)/tests/journal_device_test.cpp \
                         $(PROJECT_ROOT)/tests/data_path_device_test.cpp \
                         $(PROJECT_ROOT)/tests/file_api_test.cpp,$(TEST_SRC))
endif
TEST_BIN := $(patsubst $(PROJECT_ROOT)/tests/%.cpp,$(BUILD_DIR)/bin/%,$(TEST_SRC))
LIB_LINK := -L$(BUILD_DIR)/lib -lhfs_core -Wl,-rpath,$(BUILD_DIR)/lib -lpthread

ifeq ($(HFS_WITH_SPDK),1)
SPDK_LIB_LIST := bdev_malloc bdev_null bdev_nvme bdev_passthru bdev_lvol bdev_raid \
                 bdev_error bdev_gpt bdev_split bdev_delay bdev_zone_block blob_bdev \
                 lvol blob nvme bdev_aio bdev_ftl ftl bdev_virtio virtio vfio_user \
                 accel_error accel_ioat ioat accel_ae4dma ae4dma scheduler_dynamic \
                 env_dpdk scheduler_dpdk_governor scheduler_gscheduler sock_posix \
                 keyring_file keyring_linux fsdev_aio fsdev event event_bdev bdev \
                 notify event_accel accel dma event_sock sock event_iobuf \
                 event_keyring keyring event_vmd init vmd thread trace rpc jsonrpc \
                 json util log
SPDK_STATIC_LIBS := $(addprefix $(SPDK_ROOT_DIR)/build/lib/libspdk_,$(addsuffix .a,$(SPDK_LIB_LIST)))
DPDK_STATIC_LIBS := $(wildcard $(SPDK_ROOT_DIR)/dpdk/build/lib/librte_*.a)
ISAL_LIBS := $(SPDK_ROOT_DIR)/isa-l/.libs/libisal.a \
             $(SPDK_ROOT_DIR)/isa-l-crypto/.libs/libisal_crypto.a
SPDK_LINK := -Wl,--whole-archive -Wl,--no-as-needed $(SPDK_STATIC_LIBS) \
             -Wl,--no-whole-archive -Wl,--whole-archive $(DPDK_STATIC_LIBS) \
             -Wl,--no-whole-archive $(ISAL_LIBS) \
             -lnuma -ldl -lrt -luuid -lssl -lcrypto -lm -llz4 -lfuse3 -lkeyutils -laio
else
SPDK_LINK :=
endif

.PHONY: all app lib test clean

all: app

app: $(HFSCTL)

lib: $(LIB)

$(LIB): $(CORE_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) -shared -o $@ $^ $(SPDK_LINK) -lpthread $(LDFLAGS)

$(HFSCTL): $(PROJECT_ROOT)/tools/hfsctl.cpp $(LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< $(LIB_LINK) $(LDFLAGS)

$(BUILD_DIR)/obj/%.o: $(PROJECT_ROOT)/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -MF $(@:.o=.d) -c $< -o $@

$(BUILD_DIR)/bin/%_test: $(PROJECT_ROOT)/tests/%_test.cpp $(LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -I$(PROJECT_ROOT)/tests \
		-DHFS_DEFAULT_CONF_PATH='"$(PROJECT_ROOT)/config/hfs_nvme.conf"' \
		-o $@ $< $(LIB_LINK) $(LDFLAGS)

# Data-plane structure test: compiled with internal sources so it needs no
# SPDK and can also be built with ASAN/TSAN.
DATA_PLANE_TEST_SRC := $(PROJECT_ROOT)/tests/data_plane_test.cpp \
                       $(PROJECT_ROOT)/src/common/log.cpp \
                       $(PROJECT_ROOT)/src/common/stats.cpp \
                       $(PROJECT_ROOT)/src/common/trace.cpp \
                       $(PROJECT_ROOT)/src/core/actor.cpp \
                       $(PROJECT_ROOT)/src/core/frame_pool.cpp \
                       $(PROJECT_ROOT)/src/core/operation.cpp \
                       $(PROJECT_ROOT)/src/core/shard_ring.cpp
$(BUILD_DIR)/bin/data_plane_test: $(DATA_PLANE_TEST_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -I$(PROJECT_ROOT)/tests -o $@ $^ -lpthread $(LDFLAGS)

SCHEDULER_TEST_SRC := $(PROJECT_ROOT)/tests/scheduler_test.cpp \
                      $(PROJECT_ROOT)/src/common/log.cpp \
                      $(PROJECT_ROOT)/src/common/stats.cpp \
                      $(PROJECT_ROOT)/src/common/trace.cpp \
                      $(PROJECT_ROOT)/src/core/scheduler.cpp
$(BUILD_DIR)/bin/scheduler_test: $(SCHEDULER_TEST_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -I$(PROJECT_ROOT)/tests -o $@ $^ -lpthread $(LDFLAGS)

# Extent planner / DMA arena / parent completion structure test.
IO_PLANNER_TEST_SRC := $(PROJECT_ROOT)/tests/io_planner_test.cpp \
                       $(PROJECT_ROOT)/src/common/log.cpp \
                       $(PROJECT_ROOT)/src/common/stats.cpp \
                       $(PROJECT_ROOT)/src/common/trace.cpp \
                       $(PROJECT_ROOT)/src/core/dma_arena.cpp \
                       $(PROJECT_ROOT)/src/core/extent_map.cpp \
                       $(PROJECT_ROOT)/src/core/io_planner.cpp \
                       $(PROJECT_ROOT)/src/core/parent_completion.cpp
$(BUILD_DIR)/bin/io_planner_test: $(IO_PLANNER_TEST_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -I$(PROJECT_ROOT)/tests -o $@ $^ -lpthread $(LDFLAGS)

# Device-backed data-path test: internal sources plus the SPDK runtime closure.
ifeq ($(HFS_WITH_SPDK),1)
$(BUILD_DIR)/bin/data_path_device_test: $(PROJECT_ROOT)/tests/data_path_device_test.cpp $(CORE_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -I$(PROJECT_ROOT)/tests -o $@ $^ $(SPDK_LINK) -lpthread $(LDFLAGS)
endif

# Namespace/inode/directory-index structure test.
NAMESPACE_TEST_SRC := $(PROJECT_ROOT)/tests/namespace_test.cpp \
                      $(PROJECT_ROOT)/src/metadata/namespace.cpp
$(BUILD_DIR)/bin/namespace_test: $(NAMESPACE_TEST_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -I$(PROJECT_ROOT)/tests -o $@ $^ -lpthread $(LDFLAGS)

test: $(TEST_BIN)
	@for test_bin in $(TEST_BIN); do echo "== $$test_bin"; $$test_bin || exit 1; done
	@echo "all tests passed"

clean:
	rm -rf $(BUILD_DIR)

DEP := $(CORE_OBJ:.o=.d)
-include $(DEP)
