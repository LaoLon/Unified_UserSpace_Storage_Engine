#include <string>

#include "uuse/config.hpp"
#include "test_support.hpp"

namespace {

int expectInvalid(const std::string &text, const char *label) {
    auto parsed = uuse::parseConfig(text, label);
    if (parsed.ok()) {
        std::fprintf(stderr, "FAIL %s: expected InvalidArgument but parse succeeded\n", label);
        return 1;
    }
    if (parsed.status() != uuse::Status::InvalidArgument) {
        std::fprintf(stderr, "FAIL %s: expected InvalidArgument, got %s\n", label,
                     uuse::statusName(parsed.status()));
        return 1;
    }
    return 0;
}

} // namespace

int main() {
    const std::string base = validConfigText("/tmp/uuse-parser-test.lock");

    /* 1. valid in-memory config parses with the expected values */
    {
        auto parsed = uuse::parseConfig(base, "memory");
        UUSE_TEST_CHECK(parsed.ok());
        const auto &config = parsed.value();
        UUSE_TEST_CHECK(config.schemaVersion == 1);
        UUSE_TEST_CHECK(config.instanceName == "uuse-test");
        UUSE_TEST_CHECK(config.metadataDevices.size() == 1);
        UUSE_TEST_CHECK(config.dataDevices.size() == 1);
        UUSE_TEST_CHECK(config.devices.size() == 1);
        UUSE_TEST_CHECK(config.devices[0].bdevName == "Nvme0n1");
        UUSE_TEST_CHECK(config.devices[0].metadata && config.devices[0].data);
        UUSE_TEST_CHECK(config.metadataReplication == 1);
        UUSE_TEST_CHECK(config.preallocExtents == 64);
        UUSE_TEST_CHECK(config.maxExtentsPerFile == 65536);
        UUSE_TEST_CHECK(config.checkpointTriggerPercent == 50);
        UUSE_TEST_CHECK(config.maxGroupCommitWaitUs == 200);
        UUSE_TEST_CHECK(config.traceSample == 1);
        UUSE_TEST_CHECK(config.processLockPath == "/tmp/uuse-parser-test.lock");
        UUSE_TEST_CHECK(config.numa.policy == "local-first");
        UUSE_TEST_CHECK(config.numa.deviceNuma == "auto");
        UUSE_TEST_CHECK(config.numa.dmaNuma == "auto");
        UUSE_TEST_CHECK(!config.numa.allowRemoteDma);
    }

    /* 2. the shipped single-disk baseline parses */
    {
        auto parsed = uuse::parseConfigFile(UUSE_DEFAULT_CONF_PATH);
        UUSE_TEST_CHECK(parsed.ok());
        UUSE_TEST_CHECK(parsed.value().devices.size() == 1);
        UUSE_TEST_CHECK(parsed.value().devices[0].pciBdf == UUSE_TEST_PCI_BDF);
    }

    /* 3. unknown key */
    {
        std::string text = base;
        UUSE_TEST_CHECK(replaceAll(text, "schema_version = 1\n", "schema_version = 1\nbogus = 1\n"));
        UUSE_TEST_CHECK(expectInvalid(text, "unknown-key") == 0);
    }

    /* 4. unknown section */
    {
        std::string text = base + "\n[unknown]\nkey = 1\n";
        UUSE_TEST_CHECK(expectInvalid(text, "unknown-section") == 0);
    }

    /* 5. duplicate device section */
    {
        std::string text = base +
                           "\n[device.nvme0]\nbdev_name = Nvme9n1\npci_bdf = 0000:0d:00.0\n"
                           "role = data\nweight = 1\nrequired = true\n";
        UUSE_TEST_CHECK(expectInvalid(text, "duplicate-device") == 0);
    }

    /* 6. unknown device reference */
    {
        std::string text = base;
        UUSE_TEST_CHECK(replaceAll(text, "data_devices = nvme0", "data_devices = nvmeX"));
        UUSE_TEST_CHECK(expectInvalid(text, "unknown-device-ref") == 0);
    }

    /* 7. role mismatch */
    {
        std::string text = base;
        UUSE_TEST_CHECK(replaceAll(text, "role = metadata,data", "role = metadata"));
        UUSE_TEST_CHECK(expectInvalid(text, "role-mismatch") == 0);
    }

    /* 8. replication above device count */
    {
        std::string text = base;
        UUSE_TEST_CHECK(replaceAll(text, "metadata_replication = 1", "metadata_replication = 2"));
        UUSE_TEST_CHECK(expectInvalid(text, "replication") == 0);
    }

    /* 9. unsupported schema version */
    {
        std::string text = base;
        UUSE_TEST_CHECK(replaceAll(text, "schema_version = 1", "schema_version = 2"));
        UUSE_TEST_CHECK(expectInvalid(text, "schema") == 0);
    }

    /* 10. duplicate bdev name */
    {
        std::string text = base +
                           "\n[device.nvme1]\nbdev_name = Nvme0n1\npci_bdf = 0000:0d:00.0\n"
                           "role = data\nweight = 1\nrequired = true\n";
        UUSE_TEST_CHECK(expectInvalid(text, "duplicate-bdev") == 0);
    }

    /* 11. missing pci_bdf for uio */
    {
        std::string text = base;
        UUSE_TEST_CHECK(replaceAll(text, std::string("pci_bdf = ") + UUSE_TEST_PCI_BDF + "\n", ""));
        UUSE_TEST_CHECK(expectInvalid(text, "missing-bdf") == 0);
    }

    /* 12. missing required key */
    {
        std::string text = base;
        UUSE_TEST_CHECK(replaceAll(text, "ring_depth = 1024\n", ""));
        UUSE_TEST_CHECK(expectInvalid(text, "missing-key") == 0);
    }

    /* 13. invalid checkpoint thresholds */
    {
        std::string text = base;
        UUSE_TEST_CHECK(replaceAll(text, "checkpoint_trigger_percent = 50",
                                  "checkpoint_trigger_percent = 90"));
        UUSE_TEST_CHECK(expectInvalid(text, "checkpoint-threshold") == 0);
    }

    /* 14. malformed line */
    {
        std::string text = base + "\nnot-a-key-value\n";
        UUSE_TEST_CHECK(expectInvalid(text, "malformed-line") == 0);
    }

    /* 15. metadata replica list must be complete and duplicate-free */
    {
        std::string text = base;
        UUSE_TEST_CHECK(replaceAll(text, "metadata_devices = nvme0",
                                  "metadata_devices = nvme0,nvme0"));
        UUSE_TEST_CHECK(expectInvalid(text, "duplicate-metadata") == 0);
    }

    /* 16. a complete two-device metadata/data set is accepted by the schema. */
    {
        std::string text = base;
        UUSE_TEST_CHECK(replaceAll(text, "metadata_devices = nvme0\n",
                                  "metadata_devices = nvme0,nvme1\n"));
        UUSE_TEST_CHECK(replaceAll(text, "data_devices = nvme0\n",
                                  "data_devices = nvme0,nvme1\n"));
        UUSE_TEST_CHECK(replaceAll(text, "metadata_replication = 1",
                                  "metadata_replication = 2"));
        text += "\n[device.nvme1]\n"
                "bdev_name = Nvme1n1\n"
                "pci_bdf = 0000:0c:00.0\n"
                "role = metadata,data\n"
                "weight = 1\n"
                "required = true\n";
        auto parsed = uuse::parseConfig(text, "two-device-schema");
        UUSE_TEST_CHECK(parsed.ok());
        UUSE_TEST_CHECK(parsed.value().devices.size() == 2);
        UUSE_TEST_CHECK(parsed.value().metadataReplication == 2);
    }

    /* 17. two configured devices must not alias the same physical controller. */
    {
        std::string text = base;
        UUSE_TEST_CHECK(replaceAll(text, "metadata_devices = nvme0\n",
                                  "metadata_devices = nvme0,nvme1\n"));
        UUSE_TEST_CHECK(replaceAll(text, "data_devices = nvme0\n",
                                  "data_devices = nvme0,nvme1\n"));
        UUSE_TEST_CHECK(replaceAll(text, "metadata_replication = 1",
                                  "metadata_replication = 2"));
        text += "\n[device.nvme1]\n"
                "bdev_name = Nvme1n1\n"
                "pci_bdf = " UUSE_TEST_PCI_BDF "\n"
                "role = metadata,data\n"
                "weight = 1\n"
                "required = true\n";
        UUSE_TEST_CHECK(expectInvalid(text, "duplicate-pci-bdf") == 0);
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
        auto parsed = uuse::parseConfig(text, "numa-policy");
        UUSE_TEST_CHECK(parsed.ok());
        UUSE_TEST_CHECK(parsed.value().numa.policy == "local-only");
        UUSE_TEST_CHECK(parsed.value().numa.deviceNuma == "1");
        UUSE_TEST_CHECK(parsed.value().numa.polling == "interrupt");
    }

    /* 19. NUMA policy typos fail closed instead of changing placement silently. */
    {
        std::string text = base + "\n[numa]\npolicy = remote\n";
        UUSE_TEST_CHECK(expectInvalid(text, "numa-policy") == 0);
    }

    /* 20. Explicit role CPU overlap is rejected before runtime startup. */
    {
        std::string text = base;
        UUSE_TEST_CHECK(replaceAll(text, "data_cpus = auto", "data_cpus = 1\n"
                                                       "frontend_cpus = 1"));
        UUSE_TEST_CHECK(expectInvalid(text, "cpu-role-overlap") == 0);
    }

    /* 21. metadata_cpus is required; its first entry derives metadataCpu. */
    {
        std::string text = base;
        UUSE_TEST_CHECK(replaceAll(text, "data_cpus = auto",
                                  "data_cpus = 1\nfrontend_cpus = auto"));
        auto parsed = uuse::parseConfig(text, "cpu-role-lists");
        UUSE_TEST_CHECK(parsed.ok());
        UUSE_TEST_CHECK(parsed.value().metadataCpus == "0");
        UUSE_TEST_CHECK(parsed.value().metadataCpu == 0);
        UUSE_TEST_CHECK(parsed.value().frontendCpus == "auto");
    }

    /* 22. worker_direct_reads defaults on and can be disabled explicitly. */
    {
        auto defaulted = uuse::parseConfig(base, "rocksdb-default");
        UUSE_TEST_CHECK(defaulted.ok());
        UUSE_TEST_CHECK(defaulted.value().rocksdbWorkerDirectReads);

        std::string text = base + "\n[rocksdb]\nworker_direct_reads = false\n";
        auto parsed = uuse::parseConfig(text, "rocksdb-off");
        UUSE_TEST_CHECK(parsed.ok());
        UUSE_TEST_CHECK(!parsed.value().rocksdbWorkerDirectReads);
    }

    /* 23. Missing or auto metadata_cpus is rejected (no scalar fallback). */
    {
        std::string missing = base;
        UUSE_TEST_CHECK(replaceAll(missing, "metadata_cpus = 0\n", ""));
        UUSE_TEST_CHECK(expectInvalid(missing, "metadata-cpus-missing") == 0);

        std::string autoList = base;
        UUSE_TEST_CHECK(replaceAll(autoList, "metadata_cpus = 0", "metadata_cpus = auto"));
        UUSE_TEST_CHECK(expectInvalid(autoList, "metadata-cpus-auto") == 0);
    }

    std::printf("config_parser_test: ok\n");
    return 0;
}
