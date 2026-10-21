# Onion Installer (Tauri)

Desktop installer: pick the SD card root, pick an Onion release (GitHub) or a
local `Onion-v*.zip`, and the app unpacks it over the card, relabels the
volume after the version (`ONION 4-4-0`) and offers to eject it. Onion's own
on-device installer/updater finishes the job on the next boot.

## Build

Requires Rust and the Tauri CLI (`cargo install tauri-cli --version "^2"`).

```bash
cd installer/src-tauri
cargo tauri dev      # run
cargo tauri build    # installers under target/release/bundle/
cargo test           # extract / sd_info self-check
```

## Headless use

```bash
onion-installer --sd F:\ --file Onion-v4.4.0.zip
onion-installer --sd F:\ --url https://github.com/OnionUI/Onion/releases/download/v4.3.1-1/Onion-v4.3.1-1.zip
```
