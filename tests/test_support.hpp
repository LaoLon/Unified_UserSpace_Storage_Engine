#pragma once

#include <cstdio>
#include <string>

#define HFS_TEST_CHECK(condition)                                                        \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);    \
            return 1;                                                                    \
        }                                                                                \
    } while (0)

/* Device tests use the configured BDF; structure tests do not access a device. */
#ifndef HFS_TEST_PCI_BDF
#define HFS_TEST_PCI_BDF ""
#endif

/* A schema-complete single-device config; lockPath keeps tests isolated. */
inline std::string validConfigText(const std::string &lockPath,
                                   const std::string &instance = "hfs2-test",
                                   unsigned int dmaArenaMb = 32) {
    return "[hfs]\n"
           "schema_version = 1\n"
           "instance_name = " + instance + "\n"
           "volume_uuid = auto\n"
           "process_lock = exclusive\n"
           "process_lock_path = " + lockPath + "\n"
           "\n"
           "[spdk]\n"
           "env_driver = uio\n"
           "hugepages = 1024\n"
           "\n"
           "[storage]\n"
           "metadata_devices = nvme0\n"
           "data_devices = nvme0\n"
           "placement = capacity_weighted\n"
           "metadata_replication = 1\n"
           "require_all_devices = true\n"
           "metadata_block_size = 4096\n"
           "journal_mb = 64\n"
           "checkpoint_mb = 128\n"
           "extent_mb = 1\n"
           "prealloc_extents = 64\n"
           "max_extents_per_file = 65536\n"
           "\n"
           "[device.nvme0]\n"
           "bdev_name = Nvme0n1\n"
           "pci_bdf = " HFS_TEST_PCI_BDF "\n"
           "role = metadata,data\n"
           "weight = 1\n"
           "required = true\n"
           "\n"
           "[runtime]\n"
           "metadata_cpus = 0\n"
           "data_reactors = 0\n"
           "max_data_reactors = 4\n"
           "data_cpus = auto\n"
           "ring_depth = 1024\n"
           "client_depth = 32\n"
           "\n"
           "[limits]\n"
           "dma_arena_mb = " + std::to_string(dmaArenaMb) + "\n"
           "operation_pool = 4096\n"
           "coroutine_frame_pool = 4096\n"
           "metadata_wait_timeout_ms = 5000\n"
           "backend_retry_limit = 3\n"
           "\n"
           "[metadata]\n"
           "max_inodes = 1000000\n"
           "max_namespace_bytes = 1073741824\n"
           "checkpoint_trigger_percent = 50\n"
           "checkpoint_force_percent = 80\n"
           "max_group_commit_wait_us = 200\n"
           "\n"
           "[observability]\n"
           "trace = false\n"
           "trace_sample = 1\n"
           "trace_dir = /mnt/hfs-data\n"
           "stats = false\n";
}

inline bool replaceAll(std::string &text, const std::string &from, const std::string &to) {
    const std::size_t position = text.find(from);
    if (position == std::string::npos) {
        return false;
    }
    text.replace(position, from.size(), to);
    return true;
}
