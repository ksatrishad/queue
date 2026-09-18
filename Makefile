.PHONY: build test check clippy fmt release clean

build:
	cargo build

test:
	cargo test --all-targets

check:
	cargo check --all-targets
	cargo test --all-targets

clippy:
	cargo clippy --all-targets --all-features

fmt:
	cargo fmt --all

release:
	cargo build --release

clean:
	cargo clean
