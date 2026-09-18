#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -eq 0 ]]; then
    DNF=(dnf)
else
    DNF=(sudo dnf)
fi

"${DNF[@]}" install -y \
    gcc \
    glibc-devel \
    binutils \
    make \
    git \
    curl \
    ca-certificates

if [[ -f "${HOME}/.cargo/env" ]]; then
    # shellcheck disable=SC1090
    source "${HOME}/.cargo/env"
fi

if ! command -v rustup >/dev/null 2>&1; then
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
        | sh -s -- -y --profile minimal --default-toolchain 1.98.1
fi

# rustup installs Cargo under ~/.cargo/bin.
# shellcheck disable=SC1090
source "${HOME}/.cargo/env"

rustup toolchain install 1.98.1 --profile minimal --component rustfmt --component clippy
rustup target add x86_64-unknown-linux-gnu --toolchain 1.98.1
rustup default 1.98.1

printf '\nRust build environment ready:\n'
rustc --version
cargo --version
