PROJECT_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
SPDK_ROOT_DIR ?= $(PROJECT_ROOT)/spdk
BUILD_DIR ?= $(PROJECT_ROOT)/build
APP := $(BUILD_DIR)/bin/spdk-hfs
LIB := $(BUILD_DIR)/lib/libhfs.so
CORE_SRC := $(PROJECT_ROOT)/src/hfs.c $(PROJECT_ROOT)/src/hfs_mount.c $(PROJECT_ROOT)/src/hfs_file.c $(PROJECT_ROOT)/src/hfs_log.c
APP_SRC := $(CORE_SRC) $(PROJECT_ROOT)/src/hfs_main.c
LIB_SRC := $(CORE_SRC)
APP_OBJ := $(patsubst $(PROJECT_ROOT)/src/%.c,$(BUILD_DIR)/obj/%.o,$(APP_SRC))
LIB_OBJ := $(patsubst $(PROJECT_ROOT)/src/%.c,$(BUILD_DIR)/obj/%.o,$(LIB_SRC))
OBJ := $(APP_OBJ)
DEP := $(OBJ:.o=.d)
CONFIG ?= $(PROJECT_ROOT)/config/hfs.json

ifeq ($(wildcard $(SPDK_ROOT_DIR)/mk/spdk.common.mk),)
$(error SPDK submodule is missing. Run 'git submodule update --init --recursive')
endif

include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

SPDK_LIB_LIST := $(ALL_MODULES_LIST) event event_bdev
CFLAGS += -DHFS_DEFAULT_CONFIG=\"$(CONFIG)\"

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk

# spdk.app.mk supplies the canonical linker command; keep project objects and
# output directories outside the SPDK source tree.
OBJS := $(APP_OBJ)
$(APP): $(OBJS) | $(BUILD_DIR)/bin

$(LIB): $(LIB_OBJ) | $(BUILD_DIR)/lib
	$(CC) -o $@ -shared $(CFLAGS) $(LDFLAGS) \
		-Wl,-soname,libhfs.so \
		$(LIB_OBJ) $(SPDK_LIB_LINKER_ARGS) $(ENV_LDFLAGS) $(SYS_LIBS)

.PHONY: all app lib spdk spdk-configure check-spdk compile-commands run clean distclean

all: app

app: check-spdk $(APP)

lib: check-spdk $(LIB)

spdk-configure:
	@test -f $(SPDK_ROOT_DIR)/CONFIG || (cd $(SPDK_ROOT_DIR) && ./configure)

spdk: spdk-configure
	$(MAKE) -C $(SPDK_ROOT_DIR)

check-spdk:
	@test -f $(SPDK_ROOT_DIR)/CONFIG || { echo "SPDK is not configured. Follow README.md, then run 'make spdk'." >&2; exit 1; }
	@test -d $(SPDK_ROOT_DIR)/build/lib || { echo "SPDK is not built. Run 'make spdk' first." >&2; exit 1; }

$(BUILD_DIR)/obj $(BUILD_DIR)/bin $(BUILD_DIR)/lib:
	mkdir -p $@

$(BUILD_DIR)/obj/%.o: $(PROJECT_ROOT)/src/%.c | $(BUILD_DIR)/obj
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(PROJECT_ROOT)/src -MMD -MP -MF $(@:.o=.d) -c $< -o $@

compile-commands: check-spdk
	@command -v bear >/dev/null || { echo "bear is required" >&2; exit 1; }
	@rm -f $(PROJECT_ROOT)/compile_commands.json
	bear --output $(PROJECT_ROOT)/compile_commands.json -- $(MAKE) clean app

HUGEPAGES_TOTAL := $(shell cat /proc/sys/vm/nr_hugepages 2>/dev/null || echo 0)

run: all
	@if [ "$(HUGEPAGES_TOTAL)" = "0" ]; then \
		echo "No hugepages configured, allocating 1024 x 2MB..."; \
		echo 1024 > /proc/sys/vm/nr_hugepages; \
	fi
	$(APP)

distclean: clean
	$(MAKE) -C $(SPDK_ROOT_DIR) clean

# Override spdk.app.mk's clean (it only removes files, cannot remove the
# build directory). SPDK is only cleaned by distclean.
clean:
	rm -rf $(BUILD_DIR)

-include $(DEP)
