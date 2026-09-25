#include <string>

#include "hfs/config.hpp"
#include "test_support.hpp"

namespace {

int expectInvalid(const std::string &text, const char *label) {
    auto parsed = hfs::parseConfig(text, label);
    if (parsed.ok()) {
        std::fprintf(stderr, "FAIL %s: expected InvalidArgument but parse succeeded\n", label);
        return 1;
    }
    if (parsed.status() != hfs::Status::InvalidArgument) {
        std::fprintf(stderr, "FAIL %s: expected InvalidArgument, got %s\n", label,
                     hfs::statusName(parsed.status()));
        return 1;
    }
    return 0;
}

} // namespace

int main() {
    const std::string base = validConfigText("/tmp/hfs2-parser-test.lock");

    /* 1. valid in-memory config parses with the expected values */
    {
        auto parsed = hfs::parseConfig(base, "memory");
        HFS_TEST_CHECK(parsed.ok());
        const auto &config = parsed.value();
        HFS_TEST_CHECK(config.schemaVersion == 1);
        HFS_TEST_CHECK(config.instanceName == "hfs2-test");
        HFS_TEST_CHECK(config.metadataDevices.size() == 1);
        HFS_TEST_CHECK(config.dataDevices.size() == 1);
        HFS_TEST_CHECK(config.devices.size() == 1);
        HFS_TEST_CHECK(config.devices[0].bdevName == "Nvme0n1");
        HFS_TEST_CHECK(config.devices[0].metadata && config.devices[0].data);
        HFS_TEST_CHECK(config.metadataReplication == 1);
        HFS_TEST_CHECK(config.preallocExtents == 64);
        HFS_TEST_CHECK(config.maxExtentsPerFile == 65536);
        HFS_TEST_CHECK(config.checkpointTriggerPercent == 50);
        HFS_TEST_CHECK(config.maxGroupCommitWaitUs == 200);
        HFS_TEST_CHECK(config.traceSample == 1);
        HFS_TEST_CHECK(config.processLockPath == "/tmp/hfs2-parser-test.lock");
        HFS_TEST_CHECK(config.numa.policy == "local-first");
        HFS_TEST_CHECK(config.numa.deviceNuma == "auto");
        HFS_TEST_CHECK(config.numa.dmaNuma == "auto");
        HFS_TEST_CHECK(!config.numa.allowRemoteDma);
    }

    /* 2. the shipped single-disk baseline parses */
    {
        auto parsed = hfs::parseConfigFile(HFS_DEFAULT_CONF_PATH);
        HFS_TEST_CHECK(parsed.ok());
        HFS_TEST_CHECK(parsed.value().devices.size() == 1);
        HFS_TEST_CHECK(parsed.value().devices[0].pciBdf == HFS_TEST_PCI_BDF);
    }

    /* 3. unknown key */
    {
        std::string text = base;
        HFS_TEST_CHECK(replaceAll(text, "schema_version = 1\n", "schema_version = 1\nbogus = 1\n"));
        HFS_TEST_CHECK(expectInvalid(text, "unknown-key") == 0);
    }

    /* 4. unknown section */
    {
        std::string text = base + "\n[unknown]\nkey = 1\n";
        HFS_TEST_CHECK(expectInvalid(text, "unknown-section") == 0);
    }

    /* 5. duplicate device section */
    {
        std::string text = base +
                           "\n[device.nvme0]\nbdev_name = Nvme9n1\npci_bdf = 0000:0d:00.0\n"
                           "role = data\nweight = 1\nrequired = true\n";
        HFS_TEST_CHECK(expectInvalid(text, "duplicate-device") == 0);
    }

    /* 6. unknown device reference */
    {
        std::string text = base;
        HFS_TEST_CHECK(replaceAll(text, "data_devices = nvme0", "data_devices = nvmeX"));
        HFS_TEST_CHECK(expectInvalid(text, "unknown-device-ref") == 0);
    }

    /* 7. role mismatch */
    {
        std::string text = base;
        HFS_TEST_CHECK(replaceAll(text, "role = metadata,data", "role = metadata"));
        HFS_TEST_CHECK(expectInvalid(text, "role-mismatch") == 0);
    }

    /* 8. replication above device count */
    {
        std::string text = base;
        HFS_TEST_CHECK(replaceAll(text, "metadata_replication = 1", "metadata_replication = 2"));
        HFS_TEST_CHECK(expectInvalid(text, "replication") == 0);
    }

    /* 9. unsupported schema version */
    {
        std::string text = base;
        HFS_TEST_CHECK(replaceAll(text, "schema_version = 1", "schema_version = 2"));
        HFS_TEST_CHECK(expectInvalid(text, "schema") == 0);
    }

    /* 10. duplicate bdev name */
    {
        std::string text = base +
                           "\n[device.nvme1]\nbdev_name = Nvme0n1\npci_bdf = 0000:0d:00.0\n"
                           "role = data\nweight = 1\nrequired = true\n";
        HFS_TEST_CHECK(expectInvalid(text, "duplicate-bdev") == 0);
    }

    /* 11. missing pci_bdf for uio */
    {
        std::string text = base;
        HFS_TEST_CHECK(replaceAll(text, std::string("pci_bdf = ") + HFS_TEST_PCI_BDF + "\n", ""));
        HFS_TEST_CHECK(expectInvalid(text, "missing-bdf") == 0);
    }

    /* 12. missing required key */
    {
        std::string text = base;
        HFS_TEST_CHECK(replaceAll(text, "ring_depth = 1024\n", ""));
        HFS_TEST_CHECK(expectInvalid(text, "missing-key") == 0);
    }

    /* 13. invalid checkpoint thresholds */
    {
        std::string text = base;
        HFS_TEST_CHECK(replaceAll(text, "checkpoint_trigger_percent = 50",
                                  "checkpoint_trigger_percent = 90"));
        HFS_TEST_CHECK(expectInvalid(text, "checkpoint-threshold") == 0);
    }

    /* 14. malformed line */
    {
        std::string text = base + "\nnot-a-key-value\n";
        HFS_TEST_CHECK(expectInvalid(text, "malformed-line") == 0);
    }

    /* 15. metadata replica list must be complete and duplicate-free */
    {
        std::string text = base;
        HFS_TEST_CHECK(replaceAll(text, "metadata_devices = nvme0",
                                  "metadata_devices = nvme0,nvme0"));
        HFS_TEST_CHECK(expectInvalid(text, "duplicate-metadata") == 0);
    }

    /* 16. a complete two-device metadata/data set is accepted by the schema. */
    {
        std::string text = base;
        HFS_TEST_CHECK(replaceAll(text, "metadata_devices = nvme0\n",
                                  "metadata_devices = nvme0,nvme1\n"));
        HFS_TEST_CHECK(replaceAll(text, "data_devices = nvme0\n",
                                  "data_devices = nvme0,nvme1\n"));
        HFS_TEST_CHECK(replaceAll(text, "metadata_replication = 1",
                                  "metadata_replication = 2"));
        text += "\n[device.nvme1]\n"
                "bdev_name = Nvme1n1\n"
                "pci_bdf = 0000:0c:00.0\n"
                "role = metadata,data\n"
                "weight = 1\n"
                "required = true\n";
        auto parsed = hfs::parseConfig(text, "two-device-schema");
        HFS_TEST_CHECK(parsed.ok());
        HFS_TEST_CHECK(parsed.value().devices.size() == 2);
        HFS_TEST_CHECK(parsed.value().metadataReplication == 2);
    }

    /* 17. two configured devices must not alias the same physical controller. */
    {
        std::string text = base;
        HFS_TEST_CHECK(replaceAll(text, "metadata_devices = nvme0\n",
                                  "metadata_devices = nvme0,nvme1\n"));
        HFS_TEST_CHECK(replaceAll(text, "data_devices = nvme0\n",
                                  "data_devices = nvme0,nvme1\n"));
        HFS_TEST_CHECK(replaceAll(text, "metadata_replication = 1",
                                  "metadata_replication = 2"));
        text += "\n[device.nvme1]\n"
                "bdev_name = Nvme1n1\n"
                "pci_bdf = " HFS_TEST_PCI_BDF "\n"
                "role = metadata,data\n"
                "weight = 1\n"
                "required = true\n";
        HFS_TEST_CHECK(expectInvalid(text, "duplicate-pci-bdf") == 0);
    }

    /* 18. NUMA policy is optional but validates explicit placement strategy. */
    {
        std::string text = base +
                           "\n[numa]\n"
                           "policy = local-only\n"
                           "device_numa = 1\n"
                           "dma_numa = 1\n"
                           "frontend_numa = device-local\n"
                           "allow_remote_dma = false\n"
                           "remote_fallback = disabled\n"
                           "polling = interrupt\n";
        auto parsed = hfs::parseConfig(text, "numa-policy");
        HFS_TEST_CHECK(parsed.ok());
        HFS_TEST_CHECK(parsed.value().numa.policy == "local-only");
        HFS_TEST_CHECK(parsed.value().numa.deviceNuma == "1");
        HFS_TEST_CHECK(parsed.value().numa.polling == "interrupt");
    }

    /* 19. NUMA policy typos fail closed instead of changing placement silently. */
    {
        std::string text = base + "\n[numa]\npolicy = remote\n";
        HFS_TEST_CHECK(expectInvalid(text, "numa-policy") == 0);
    }

    /* 20. Explicit role CPU overlap is rejected before runtime startup. */
    {
        std::string text = base;
        HFS_TEST_CHECK(replaceAll(text, "data_cpus = auto", "data_cpus = 1\n"
                                                       "frontend_cpus = 1"));
        HFS_TEST_CHECK(expectInvalid(text, "cpu-role-overlap") == 0);
    }

    /* 21. metadata_cpus is required; its first entry derives metadataCpu. */
    {
        std::string text = base;
        HFS_TEST_CHECK(replaceAll(text, "data_cpus = auto",
                                  "data_cpus = 1\nfrontend_cpus = auto"));
        auto parsed = hfs::parseConfig(text, "cpu-role-lists");
        HFS_TEST_CHECK(parsed.ok());
        HFS_TEST_CHECK(parsed.value().metadataCpus == "0");
        HFS_TEST_CHECK(parsed.value().metadataCpu == 0);
        HFS_TEST_CHECK(parsed.value().frontendCpus == "auto");
    }

    /* 22. worker_direct_reads defaults on and can be disabled explicitly. */
    {
        auto defaulted = hfs::parseConfig(base, "rocksdb-default");
        HFS_TEST_CHECK(defaulted.ok());
        HFS_TEST_CHECK(defaulted.value().rocksdbWorkerDirectReads);

        std::string text = base + "\n[rocksdb]\nworker_direct_reads = false\n";
        auto parsed = hfs::parseConfig(text, "rocksdb-off");
        HFS_TEST_CHECK(parsed.ok());
        HFS_TEST_CHECK(!parsed.value().rocksdbWorkerDirectReads);
    }

    /* 23. Missing or auto metadata_cpus is rejected (no scalar fallback). */
    {
        std::string missing = base;
        HFS_TEST_CHECK(replaceAll(missing, "metadata_cpus = 0\n", ""));
        HFS_TEST_CHECK(expectInvalid(missing, "metadata-cpus-missing") == 0);

        std::string autoList = base;
        HFS_TEST_CHECK(replaceAll(autoList, "metadata_cpus = 0", "metadata_cpus = auto"));
        HFS_TEST_CHECK(expectInvalid(autoList, "metadata-cpus-auto") == 0);
    }

    std::printf("config_parser_test: ok\n");
    return 0;
}
