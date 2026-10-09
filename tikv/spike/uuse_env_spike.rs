//! Shard-116 spike: open a DB through a crocksdb Env whose FileSystem
//! redirects every path under a base directory (proves the Env(fs) chain).
use std::sync::Arc;

use rocksdb::{DBOptions, Env, FlushOptions, Writable, DB};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = std::env::args().collect();
    let base = args
        .get(1)
        .cloned()
        .unwrap_or_else(|| "/build/uuse-spike-root".to_string());
    let db_path = args
        .get(2)
        .cloned()
        .unwrap_or_else(|| "/build/uuse-spike-db".to_string());

    let env = Arc::new(Env::uuse_spike(&base)?);
    let mut opts = DBOptions::new();
    opts.create_if_missing(true);
    opts.set_env(env.clone());

    let db = DB::open(opts, &db_path)?;
    db.put(b"k1", b"v1")?;
    match db.get(b"k1")? {
        Some(value) => assert_eq!(value.to_vec(), b"v1".to_vec()),
        None => panic!("missing key after put"),
    }
    db.flush(&FlushOptions::default())?;
    drop(db);

    let mapped = format!("{}{}", base, db_path);
    println!("spike ok: mapped dir {}", mapped);
    let mut names: Vec<String> = std::fs::read_dir(&mapped)?
        .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
        .collect();
    names.sort();
    println!("files: {:?}", names);
    Ok(())
}
