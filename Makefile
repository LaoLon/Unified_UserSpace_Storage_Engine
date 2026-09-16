PROJECT_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD_DIR ?= $(PROJECT_ROOT)/build
CXX ?= g++

# Shard-68 skeleton build: pure C++20 core, no SPDK yet (Shard-69 adds the
# runtime and SPDK linkage). src_legacy_frozen/ is never part of this build.
CXXFLAGS ?= -std=c++20 -O2 -g
CXXFLAGS += -Wall -Wextra -Wno-unused-parameter -fno-exceptions -fno-rtti \
            -fvisibility=hidden -fvisibility-inlines-hidden -fPIC -DHFS_CORE_ABI_MAJOR=2
INCLUDES := -I$(PROJECT_ROOT)/include -I$(PROJECT_ROOT)/src

CORE_SRC := $(sort $(wildcard $(PROJECT_ROOT)/src/common/*.cpp) \
                   $(wildcard $(PROJECT_ROOT)/src/config/*.cpp) \
                   $(wildcard $(PROJECT_ROOT)/src/core/*.cpp))
CORE_OBJ := $(patsubst $(PROJECT_ROOT)/src/%.cpp,$(BUILD_DIR)/obj/%.o,$(CORE_SRC))
LIB := $(BUILD_DIR)/lib/libhfs_core.so
HFSCTL := $(BUILD_DIR)/bin/hfsctl
TEST_SRC := $(sort $(wildcard $(PROJECT_ROOT)/tests/*_test.cpp))
TEST_BIN := $(patsubst $(PROJECT_ROOT)/tests/%.cpp,$(BUILD_DIR)/bin/%,$(TEST_SRC))
LIB_LINK := -L$(BUILD_DIR)/lib -lhfs_core -Wl,-rpath,$(BUILD_DIR)/lib

.PHONY: all app lib test clean

all: app

app: $(HFSCTL)

lib: $(LIB)

$(LIB): $(CORE_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) -shared -o $@ $^ $(LDFLAGS)

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

test: $(TEST_BIN)
	@for test_bin in $(TEST_BIN); do echo "== $$test_bin"; $$test_bin || exit 1; done
	@echo "all tests passed"

clean:
	rm -rf $(BUILD_DIR)

DEP := $(CORE_OBJ:.o=.d)
-include $(DEP)
