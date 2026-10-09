// Copyright 2026 TiKV Project Authors. Licensed under Apache-2.0.

//! Shard-116 smoke: run a raft-engine instance whose FileSystem is the UUSE
//! C ABI shim. Requires a formatted UUSE volume, the device bound to
//! uio_pci_generic, hugepages=1024 and LD_LIBRARY_PATH pointing at the UUSE
//! build tree.

use std::sync::Arc;

use raft::eraftpb::Entry;
use raft_engine::{Config as EngineConfig, LogBatch, MessageExt};

use raft_log_engine::uuse_file_system::UuseFileSystem;

struct SmokeExt;

impl MessageExt for SmokeExt {
    type Entry = Entry;

    fn index(e: &Self::Entry) -> u64 {
        e.index
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = std::env::args().collect();
    let conf = args
        .get(1)
        .cloned()
        .unwrap_or_else(|| "/root/code/config/uuse_nvme.conf".to_string());
    let dir = args
        .get(2)
        .cloned()
        .unwrap_or_else(|| "/raft-engine-smoke".to_string());

    let fs = Arc::new(UuseFileSystem::new(&conf)?);
    fs.create_dirs(&dir)?;

    let mut config = EngineConfig::default();
    config.dir = dir.clone();
    let engine = raft_engine::Engine::open_with_file_system(config, fs.clone())?;

    let mut batch = LogBatch::default();
    let entries = vec![
        Entry {
            index: 1,
            term: 1,
            data: b"e1".to_vec().into(),
            ..Default::default()
        },
        Entry {
            index: 2,
            term: 1,
            data: b"e2".to_vec().into(),
            ..Default::default()
        },
    ];
    batch.add_entries::<SmokeExt>(42, &entries)?;
    let written = engine.write(&mut batch, true)?;
    engine.sync()?;

    let mut fetched: Vec<Entry> = Vec::new();
    let count = engine.fetch_entries_to::<SmokeExt>(42, 1, 3, None, &mut fetched)?;
    if count != 2 {
        return Err(format!("fetch count mismatch: {}", count).into());
    }
    if fetched[0].data.as_ref() != &b"e1"[..] || fetched[1].data.as_ref() != &b"e2"[..] {
        return Err(format!("fetch data mismatch: {:?}", fetched).into());
    }

    drop(engine);
    drop(fs);
    println!("RAFT_ENGINE_SMOKE_PASS written={} fetched={}", written, count);
    Ok(())
}
