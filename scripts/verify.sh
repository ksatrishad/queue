#!/usr/bin/env bash
set -euo pipefail

cargo check --all-targets
cargo clippy --all-targets --all-features
cargo test --all-targets
cargo build --release
