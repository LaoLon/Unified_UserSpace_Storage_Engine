// Copyright 2026 TiKV Project Authors. Licensed under Apache-2.0.

//! Shard-116 production-path smoke: the real raft-engine wrapper
//! (`RaftLogEngine::new_with_uuse` + `ManagedFileSystem`) on the UUSE volume.
//! Requires a formatted UUSE volume, uio binding, hugepages=1024 and
//! LD_LIBRARY_PATH pointing at the UUSE build tree.

use engine_traits::{RaftEngine, RaftEngineReadOnly, RaftLogBatch as RaftLogBatchTrait};
use raft::eraftpb::Entry;
use raft_log_engine::{RaftEngineConfig, RaftLogEngine};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = std::env::args().collect();
    let conf = args
        .get(1)
        .cloned()
        .unwrap_or_else(|| "/root/code/config/uuse_nvme.conf".to_string());
    let dir = args
        .get(2)
        .cloned()
        .unwrap_or_else(|| "/raft-engine-prod-smoke".to_string());

    // Pre-create the namespace directory with a short-lived UUSE filesystem.
    {
        use raft_log_engine::uuse_file_system::UuseFileSystem;
        let fs = UuseFileSystem::new(&conf)?;
        fs.create_dirs(&dir)?;
    }

    let mut config = RaftEngineConfig::default();
    config.dir = dir.clone();
    let engine = RaftLogEngine::new_with_uuse(config, None, None, &conf)?;

    let entries = vec![
        Entry {
            index: 1,
            term: 1,
            data: b"p1".to_vec().into(),
            ..Default::default()
        },
        Entry {
            index: 2,
            term: 1,
            data: b"p2".to_vec().into(),
            ..Default::default()
        },
    ];
    let mut batch = engine.log_batch(16);
    batch.append(7, None, entries)?;
    let written = engine.consume(&mut batch, true)?;
    engine.sync()?;

    let mut fetched: Vec<Entry> = Vec::new();
    let count = engine.fetch_entries_to(7, 1, 3, None, &mut fetched)?;
    if count != 2 || fetched[0].data.as_ref() != &b"p1"[..] {
        return Err(format!("fetch mismatch: count={} entries={:?}", count, fetched).into());
    }
    drop(engine);
    println!(
        "RAFT_ENGINE_PROD_SMOKE_PASS written={} fetched={}",
        written, count
    );
    Ok(())
}
