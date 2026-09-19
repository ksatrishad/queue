#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

if [[ -x "${HOME}/.cargo/bin/rustup" ]]; then
    RUSTUP="${HOME}/.cargo/bin/rustup"
elif command -v rustup >/dev/null 2>&1; then
    RUSTUP="$(command -v rustup)"
else
    echo "error: rustup was not found; run ./scripts/bootstrap-rocky9.sh first" >&2
    exit 1
fi

TOOLCHAIN="$(${RUSTUP} show active-toolchain | awk '{print $1}')"

if ! "${RUSTUP}" component list --toolchain "${TOOLCHAIN}" --installed | grep -q '^rustfmt'; then
    echo "error: rustfmt is missing for ${TOOLCHAIN}" >&2
    echo "run: ${RUSTUP} component add rustfmt --toolchain ${TOOLCHAIN}" >&2
    exit 1
fi

if ! "${RUSTUP}" component list --toolchain "${TOOLCHAIN}" --installed | grep -q '^clippy'; then
    echo "error: clippy is missing for ${TOOLCHAIN}" >&2
    echo "run: ${RUSTUP} component add clippy --toolchain ${TOOLCHAIN}" >&2
    exit 1
fi

cargo_run() {
    "${RUSTUP}" run "${TOOLCHAIN}" cargo "$@"
}

cargo_run fmt --all -- --check
cargo_run check --all-targets
cargo_run clippy --all-targets --all-features -- -D warnings
cargo_run test --all-targets
cargo_run build --release
