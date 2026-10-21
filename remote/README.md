# Onion Remote (Tauri)

Desktop app that reaches a Miyoo Mini running this fork over SSH, on the local
network or from anywhere through a relay, and gives you a file manager for the
SD card, the live screen with button input, and a one-shot terminal.

Everything goes through one SSH session to the device's dropbear: SFTP for
directory listings, `cat` for uploads/downloads, a framebuffer dump for the
screen, writes to `/dev/input/event0` for buttons.

## How the device becomes reachable

The Miyoo sits behind NAT, so it opens the connection: with **Tweaks → Network →
Allow remote access** on, `remote_tunnel.sh` keeps a reverse SSH tunnel
(`dbclient -R`) to a relay you choose, binding the relay's
`127.0.0.1:<tunnelport>` to the device's SSH port. The app logs in to the relay,
opens that port through the relay's SSH and runs a second SSH session to the
Miyoo inside it (end-to-end encrypted, the relay only sees ciphertext).

The toggle also turns SSH on. Its state is remembered like the other network
services and the tunnel restarts on boot and after Wi-Fi comes back.

### Relay requirements

Any host reachable from the Internet with an SSH server: a small VPS, a home
server with port 22 forwarded, etc. Nothing to install beyond `sshd`.

- a user for the device (password login; `onion` below) — the pairing code
  carries this password and it is stored in clear on the SD card, so use a
  dedicated account with nothing else on it
- `AllowTcpForwarding yes` (Debian/Ubuntu default; **Alpine ships `no`**)
- `GatewayPorts no` (the default): the tunnel port stays on the relay's
  loopback, so only someone who can log in to the relay reaches the Miyoo
- the device must be able to open an outbound TCP connection to the relay's
  SSH port (22 by default)

Anyone with the relay credentials reaches the Miyoo's SSH, which is root with
no password unless *SSH → Enable authentication* (onion/onion) is on.

### Pairing code

```
user:password@relay-host[:ssh-port][/tunnel-port]
```

Defaults: ssh-port 22, tunnel-port 2222. The password cannot contain `/`. Use a
different tunnel port per device if several share one relay.

### Pairing

1. On the Miyoo: Tweaks → Network → SSH on, Wi-Fi on. Note the IP shown there.
2. In the app, connect with that IP. The *Parear* row appears: type the pairing
   code and click *Gravar no Miyoo*. This writes
   `.tmp_update/config/remote_relay`, switches *Allow remote access* on and
   starts the tunnel.
3. From now on connect with the pairing code itself, from any network.

Without the app: create `.tmp_update/config/remote_relay` on the SD card with
the pairing code as its only line, then turn *Allow remote access* on in Tweaks.
The Tweaks submenu shows the relay (password hidden) and whether the tunnel
process is up.

## Features and limits

- **Files**: browse, upload (several files), download, delete (files or whole
  folders), create folder. Transfers stream through `cat`, so speed is the
  SSH link's.
- **Screen**: the visible framebuffer page (640x480, rotated like Onion's own
  screenshots) is dumped, gzip'd when the device has `gzip`, and shown as PNG;
  "ao vivo" repeats it every 1-5 s. It is a snapshot stream, not video.
- **Input**: on-screen buttons and keyboard (arrows, Z=A, X=B, A=X, S=Y, Q=L1,
  W=R1, Enter=START, Shift=SELECT, Esc=MENU) inject presses into the same
  event device the hardware uses. Hold time is 120 ms.
- **Terminal**: runs one command at a time (no PTY), shows its output.
- With *Disable services in game* on (Onion's default), dropbear is killed while
  a game runs, so the app cannot connect during games; the tunnel reconnects by
  itself afterwards.
- Host keys are not verified (relay auth is by password, the device is only
  reachable through the relay or the LAN).
- The screen capture reads `/sys/class/graphics/fb0/pan` to find the visible
  page; without it the first page is shown.

## Build

Requires Rust and the Tauri CLI (`cargo install tauri-cli --version "^2"`).

```bash
cd remote/src-tauri
cargo tauri dev      # run
cargo tauri build    # installers under target/release/bundle/
cargo test           # parsing, shell quoting, key injection bytes, frame to PNG
```

### End-to-end test against a fake Miyoo

`fixtures/` holds two containers: a relay (`sshd`) and a fake device
(dropbear like Onion runs it, sftp-server, the real `remote_tunnel.sh` from
`static/`). From the repo root:

```bash
docker network create onion-test
docker build -t onion-fake-relay remote/fixtures/relay
docker build -t onion-fake-device -f remote/fixtures/device/Dockerfile .
docker run -d --name onion-relay --network onion-test -p 2200:22 onion-fake-relay
docker run -d --name onion-device --network onion-test -p 2201:22 -e RELAY_CODE='onion:relaypass@onion-relay:22/2222' onion-fake-device
cd remote/src-tauri
ONION_REMOTE_TEST='onion:relaypass@127.0.0.1:2200/2222' ONION_REMOTE_TEST_LAN='127.0.0.1:2201' cargo test -- --ignored
```

The app itself can then be pointed at `onion:relaypass@127.0.0.1:2200/2222`
(relay) or `127.0.0.1:2201` (LAN); the fake has no framebuffer, so only the
screen fails. Not yet run against real hardware.
