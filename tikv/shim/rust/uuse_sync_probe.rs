// Copyright 2026 TiKV Project Authors. Licensed under Apache-2.0.

//! Shard-118 two-phase sync probe: `write` appends entries with sync=false,
//! calls `RaftEngine::sync()` and exits without running Drop (the TiKV
//! process-exit path); `reopen` mounts the engine in a new process and reports
//! how many entries survived.

use engine_traits::{RaftEngine, RaftEngineReadOnly, RaftLogBatch as RaftLogBatchTrait};
use raft::eraftpb::Entry;
use raft_log_engine::{RaftEngineConfig, RaftLogEngine};

const REGION: u64 = 7;
const BATCHES: u64 = 4;
const PER_BATCH: u64 = 50;

fn engine_config(dir: &str) -> RaftEngineConfig {
    let mut config = RaftEngineConfig::default();
    config.dir = dir.to_string();
    config
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = std::env::args().collect();
    let mode = args.get(1).map(String::as_str).unwrap_or("write");
    let conf = args
        .get(2)
        .cloned()
        .unwrap_or_else(|| "/root/code/config/uuse_nvme.conf".to_string());
    let dir = args
        .get(3)
        .cloned()
        .unwrap_or_else(|| "/raft-engine-sync-probe".to_string());

    if mode == "write" {
        // Keep the creating filesystem alive across engine creation: the shim
        // engine is a process-wide singleton, so the new directory is visible
        // to the recovery without an intermediate engine stop.
        use raft_log_engine::uuse_file_system::UuseFileSystem;
        let fs = UuseFileSystem::new(&conf)?;
        fs.create_dirs(&dir)?;
        let engine = RaftLogEngine::new_with_uuse(engine_config(&dir), None, None, &conf)?;
        for batch_no in 0..BATCHES {
            let entries: Vec<Entry> = (0..PER_BATCH)
                .map(|i| {
                    let index = batch_no * PER_BATCH + i + 1;
                    Entry {
                        index,
                        term: 1,
                        data: format!("e{}", index).into_bytes().into(),
                        ..Default::default()
                    }
                })
                .collect();
            let mut batch = engine.log_batch(16);
            batch.append(REGION, None, entries)?;
            // Normal-write path: no per-write sync.
            engine.consume(&mut batch, false)?;
        }
        engine.sync()?;
        println!("WRITE_DONE");
        std::process::exit(0); // skip Drop / engine stop, like process exit
    }

    let engine = RaftLogEngine::new_with_uuse(engine_config(&dir), None, None, &conf)?;
    let region: u64 = args.get(4).and_then(|s| s.parse().ok()).unwrap_or(REGION);
    if mode == "state" {
        let state = engine.get_raft_state(region)?;
        println!("STATE region={} state={:?}", region, state);
        return Ok(());
    }
    let mut entries: Vec<Entry> = Vec::new();
    let start: u64 = args.get(5).and_then(|s| s.parse().ok()).unwrap_or(1);
    // Bounded scan: enough for the probe's 200 entries and TiKV region logs.
    let count = match engine.fetch_entries_to(region, start, 1_000_000, None, &mut entries) {
        Ok(count) => count,
        Err(e) => {
            println!("REOPEN region={} start={} fetch_error={:?}", region, start, e);
            return Ok(());
        }
    };
    let first = entries.iter().map(|e| e.index).min().unwrap_or(0);
    let max_index = entries.iter().map(|e| e.index).max().unwrap_or(0);
    println!(
        "REOPEN region={} start={} fetched={} first_index={} max_index={}",
        region, start, count, first, max_index
    );
    Ok(())
}
