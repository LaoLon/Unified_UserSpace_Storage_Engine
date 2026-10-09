// Copyright 2026 TiKV Project Authors. Licensed under Apache-2.0.

//! Links the UUSE C ABI shim (`tikv/shim/`, Shard-116) into raft_log_engine.
//! The shim and libuuse_core.so live in the UUSE build tree (env override:
//! `UUSE_CORE_BUILD`).

fn main() {
    let build = std::env::var("UUSE_CORE_BUILD")
        .unwrap_or_else(|_| "/root/code/build-uuse".to_string());
    println!("cargo:rustc-link-search=native={}", build);
    println!("cargo:rustc-link-lib=dylib=uuse_tikv_shim");
    println!("cargo:rerun-if-env-changed=UUSE_CORE_BUILD");
}
