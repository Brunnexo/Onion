//! SSH side of Onion Remote: reach the Miyoo directly (LAN) or through the
//! relay the device keeps a reverse tunnel to, then list/transfer files, grab
//! the framebuffer and inject button presses, all over that one connection.

use russh::client::{self, AuthResult, Handle};
use russh::ChannelMsg;
use russh_sftp::client::SftpSession;
use serde::Serialize;
use std::io::Read;
use std::sync::Arc;
use std::time::Duration;
use tokio::io::AsyncReadExt;

pub const W: usize = 640;
pub const H: usize = 480;
const FRAME: usize = W * H * 4;

pub fn e(err: impl std::fmt::Display) -> String {
    err.to_string()
}

/// Host keys are not checked: the relay is trusted by its password and the
/// device is only reachable through the relay or the local network.
pub struct Accept;

impl client::Handler for Accept {
    type Error = russh::Error;

    async fn check_server_key(&mut self, _: &russh::keys::PublicKeyOrCertificate) -> Result<bool, Self::Error> {
        Ok(true)
    }
}

/// Pairing code: `user:password@host[:port][/tunnelport]`, the same string the
/// device keeps in `.tmp_update/config/remote_relay`.
#[derive(Debug, PartialEq, Clone)]
pub struct Relay {
    pub user: String,
    pub pass: String,
    pub host: String,
    pub port: u16,
    pub tunnel_port: u16,
}

#[derive(Debug, PartialEq, Clone)]
pub enum Target {
    Lan { host: String, port: u16 },
    Relay(Relay),
}

fn port(s: &str, what: &str) -> Result<u16, String> {
    s.parse().map_err(|_| format!("{what} inválida: {s}"))
}

pub fn parse_relay(code: &str) -> Result<Relay, String> {
    let code = code.trim();
    let (code, tunnel_port) = match code.rsplit_once('/') {
        Some((c, t)) => (c, port(t, "porta do túnel")?),
        None => (code, 2222),
    };
    let (auth, hp) = code
        .rsplit_once('@')
        .ok_or("use usuário:senha@host[:porta][/porta-do-túnel]")?;
    let (user, pass) = auth.split_once(':').unwrap_or((auth, ""));
    let (host, port) = match hp.rsplit_once(':') {
        Some((h, p)) => (h, port(p, "porta do relay")?),
        None => (hp, 22),
    };
    if user.is_empty() || host.is_empty() {
        return Err("use usuário:senha@host[:porta][/porta-do-túnel]".into());
    }
    Ok(Relay { user: user.into(), pass: pass.into(), host: host.into(), port, tunnel_port })
}

/// `user:...@host` is a relay pairing code, anything else is a LAN `host[:port]`.
pub fn parse_target(s: &str) -> Result<Target, String> {
    let s = s.trim();
    if s.is_empty() {
        return Err("informe o IP do Miyoo ou o código de pareamento".into());
    }
    if s.contains('@') {
        return parse_relay(s).map(Target::Relay);
    }
    Ok(match s.rsplit_once(':') {
        Some((h, p)) => Target::Lan { host: h.into(), port: port(p, "porta")? },
        None => Target::Lan { host: s.into(), port: 22 },
    })
}

pub struct Device {
    /// Kept open: the device session runs inside one of its channels.
    _relay: Option<Handle<Accept>>,
    pub ssh: Handle<Accept>,
    pub sftp: SftpSession,
}

fn config() -> Arc<client::Config> {
    Arc::new(client::Config { keepalive_interval: Some(Duration::from_secs(15)), ..Default::default() })
}

/// A fresh SSH session to the device: direct, or through a channel of the
/// authenticated relay session.
async fn open(target: &Target, relay: &mut Option<Handle<Accept>>) -> Result<Handle<Accept>, String> {
    match target {
        Target::Lan { host, port } => client::connect(config(), (host.as_str(), *port), Accept)
            .await
            .map_err(|err| format!("sem resposta de {host}:{port} ({err}); o SSH está ligado no Tweaks?")),
        Target::Relay(r) => {
            let relay = relay.as_mut().ok_or("relay não conectado")?;
            let ch = relay
                .channel_open_direct_tcpip("127.0.0.1", r.tunnel_port as u32, "127.0.0.1", 0)
                .await
                .map_err(|_| {
                    format!(
                        "túnel ausente na porta {} do relay: o Miyoo está ligado, com Wi-Fi e \"Permitir acesso remoto\" ativo?",
                        r.tunnel_port
                    )
                })?;
            client::connect_stream(config(), ch.into_stream(), Accept).await.map_err(e)
        }
    }
}

pub async fn connect(target: &Target) -> Result<Device, String> {
    let mut relay = None;
    if let Target::Relay(r) = target {
        let mut h = client::connect(config(), (r.host.as_str(), r.port), Accept)
            .await
            .map_err(|err| format!("relay {}:{} não responde ({err})", r.host, r.port))?;
        if !matches!(h.authenticate_password(&r.user, &r.pass).await.map_err(e)?, AuthResult::Success) {
            return Err("o relay recusou usuário/senha".into());
        }
        relay = Some(h);
    }
    // Onion's dropbear: root with a blank password (auth method "none", `dropbear -B`),
    // or onion/onion with "Enable authentication". Dropbear drops a connection that
    // changes user name after a failure, so each pair gets a fresh session.
    let mut ssh = None;
    for (user, pass) in [("root", ""), ("onion", "onion")] {
        let mut s = open(target, &mut relay).await?;
        let res = if pass.is_empty() { s.authenticate_none(user).await } else { s.authenticate_password(user, pass).await };
        if matches!(res.map_err(e)?, AuthResult::Success) {
            ssh = Some(s);
            break;
        }
    }
    let ssh = ssh.ok_or("o Miyoo recusou o login SSH (root sem senha ou onion/onion)")?;
    let ch = ssh.channel_open_session().await.map_err(e)?;
    ch.request_subsystem(true, "sftp").await.map_err(e)?;
    let sftp = SftpSession::new(ch.into_stream()).await.map_err(|err| format!("SFTP indisponível: {err}"))?;
    Ok(Device { _relay: relay, ssh, sftp })
}

/// Single-quote `s` for the device's shell.
pub fn q(s: &str) -> String {
    format!("'{}'", s.replace('\'', "'\\''"))
}

/// Run `cmd` on the device. `stdin` is streamed first (its progress reported
/// through `sent`), stdout/stderr chunks go to `out`. Returns the exit status.
pub async fn exec(
    ssh: &Handle<Accept>,
    cmd: &str,
    stdin: Option<&mut tokio::fs::File>,
    mut sent: impl FnMut(u64),
    mut out: impl FnMut(&[u8]),
) -> Result<u32, String> {
    let mut ch = ssh.channel_open_session().await.map_err(e)?;
    ch.exec(true, cmd).await.map_err(e)?;
    if let Some(f) = stdin {
        let mut buf = vec![0u8; 256 * 1024];
        let mut done = 0u64;
        loop {
            let n = f.read(&mut buf).await.map_err(e)?;
            if n == 0 {
                break;
            }
            ch.data_bytes(buf[..n].to_vec()).await.map_err(e)?;
            done += n as u64;
            sent(done);
        }
    }
    ch.eof().await.map_err(e)?;
    let mut status = 0;
    while let Some(msg) = ch.wait().await {
        match msg {
            ChannelMsg::Data { data } | ChannelMsg::ExtendedData { data, .. } => out(&data),
            ChannelMsg::ExitStatus { exit_status } => status = exit_status,
            _ => {}
        }
    }
    Ok(status)
}

/// `exec` collecting the output.
pub async fn run(ssh: &Handle<Accept>, cmd: &str) -> Result<(u32, Vec<u8>), String> {
    let mut out = Vec::new();
    let status = exec(ssh, cmd, None, |_| {}, |d| out.extend_from_slice(d)).await?;
    Ok((status, out))
}

#[derive(Serialize)]
pub struct Entry {
    pub name: String,
    pub dir: bool,
    pub size: u64,
}

pub async fn list_dir(sftp: &SftpSession, path: &str) -> Result<Vec<Entry>, String> {
    let mut entries: Vec<Entry> = sftp
        .read_dir(path)
        .await
        .map_err(e)?
        .filter(|d| d.file_name() != "." && d.file_name() != "..")
        .map(|d| Entry { name: d.file_name(), dir: d.file_type().is_dir(), size: d.metadata().len() })
        .collect();
    entries.sort_by(|a, b| b.dir.cmp(&a.dir).then(a.name.to_lowercase().cmp(&b.name.to_lowercase())));
    Ok(entries)
}

/// Visible framebuffer page, gzip'd when the device has gzip (raw otherwise).
pub const SCREEN_CMD: &str = "y=$(cut -d, -f2 /sys/class/graphics/fb0/pan 2>/dev/null); \
dd if=/dev/fb0 bs=2560 skip=${y:-0} count=480 2>/dev/null | (gzip -1 2>/dev/null || cat)";

/// Framebuffer dump (BGRA, rotated 180° like Onion's own screenshots) to PNG.
pub fn frame_to_png(raw: &[u8]) -> Result<Vec<u8>, String> {
    let data;
    let raw = if raw.starts_with(&[0x1f, 0x8b]) {
        let mut v = Vec::with_capacity(FRAME);
        flate2::read::GzDecoder::new(raw).read_to_end(&mut v).map_err(e)?;
        data = v;
        &data[..]
    } else {
        raw
    };
    if raw.len() != FRAME {
        return Err(format!("captura inesperada: {} bytes (esperado {FRAME})", raw.len()));
    }
    let mut rgb = Vec::with_capacity(W * H * 3);
    for px in raw.chunks_exact(4).rev() {
        rgb.extend_from_slice(&[px[2], px[1], px[0]]);
    }
    let mut png = Vec::new();
    let mut enc = png::Encoder::new(&mut png, W as u32, H as u32);
    enc.set_color(png::ColorType::Rgb);
    enc.set_depth(png::BitDepth::Eight);
    enc.set_compression(png::Compression::Fast);
    let mut w = enc.write_header().map_err(e)?;
    w.write_image_data(&rgb).map_err(e)?;
    w.finish().map_err(e)?;
    Ok(png)
}

/// Linux input key codes of the Miyoo buttons (src/common/system/keymap_hw.h).
pub fn key_code(name: &str) -> Option<u16> {
    Some(match name {
        "UP" => 103,
        "DOWN" => 108,
        "LEFT" => 105,
        "RIGHT" => 106,
        "A" => 57,
        "B" => 29,
        "X" => 42,
        "Y" => 56,
        "L1" => 18,
        "R1" => 20,
        "L2" => 15,
        "R2" => 14,
        "SELECT" => 97,
        "START" => 28,
        "MENU" => 1,
        "POWER" => 116,
        "VOLUP" => 115,
        "VOLDOWN" => 114,
        _ => return None,
    })
}

/// One `struct input_event` (32-bit ARM: 8-byte timeval + type + code + value)
/// as printf octal escapes.
fn input_event(kind: u16, code: u16, value: u32) -> String {
    let mut b = vec![0u8; 8];
    b.extend_from_slice(&kind.to_le_bytes());
    b.extend_from_slice(&code.to_le_bytes());
    b.extend_from_slice(&value.to_le_bytes());
    b.iter().map(|x| format!("\\{x:03o}")).collect()
}

/// Press and release a button through the same event device the hardware uses,
/// each followed by a SYN_REPORT so readers see it right away.
pub fn key_cmd(code: u16, hold_ms: u32) -> String {
    let dev = "/dev/input/event0";
    let press = input_event(1, code, 1) + &input_event(0, 0, 0);
    let release = input_event(1, code, 0) + &input_event(0, 0, 0);
    format!(
        "printf '{press}' > {dev}; usleep {} 2>/dev/null || sleep 0.1; printf '{release}' > {dev}",
        hold_ms.max(1) * 1000
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_pairing_codes_and_lan_targets() {
        assert_eq!(
            parse_relay("onion:s3cret@relay.example.com:2200/2223").unwrap(),
            Relay { user: "onion".into(), pass: "s3cret".into(), host: "relay.example.com".into(), port: 2200, tunnel_port: 2223 }
        );
        let r = parse_relay(" onion:p@ss:word@1.2.3.4 ").unwrap();
        assert_eq!((r.pass.as_str(), r.host.as_str(), r.port, r.tunnel_port), ("p@ss:word", "1.2.3.4", 22, 2222));
        assert!(parse_relay("relay.example.com").is_err());
        assert!(parse_relay("u:p@h:abc").is_err());
        assert_eq!(parse_target("192.168.0.20").unwrap(), Target::Lan { host: "192.168.0.20".into(), port: 22 });
        assert_eq!(parse_target("miyoo:2222").unwrap(), Target::Lan { host: "miyoo".into(), port: 2222 });
        assert!(matches!(parse_target("u:p@h").unwrap(), Target::Relay(_)));
        assert!(parse_target("  ").is_err());
    }

    #[test]
    fn shell_quoting() {
        assert_eq!(q("/mnt/SDCARD/Roms/it's.zip"), "'/mnt/SDCARD/Roms/it'\\''s.zip'");
    }

    #[test]
    fn key_command_is_press_syn_release_syn() {
        let cmd = key_cmd(57, 100);
        // EV_KEY(1) KEY_SPACE(57=0o71) value 1, then SYN
        assert!(cmd.starts_with("printf '\\000\\000\\000\\000\\000\\000\\000\\000\\001\\000\\071\\000\\001\\000\\000\\000\\000\\000\\000\\000\\000\\000\\000\\000\\000\\000\\000\\000\\000\\000\\000\\000' > /dev/input/event0; usleep 100000"));
        assert!(cmd.ends_with("\\001\\000\\071\\000\\000\\000\\000\\000\\000\\000\\000\\000\\000\\000\\000\\000\\000\\000\\000\\000\\000\\000\\000\\000' > /dev/input/event0"));
        assert_eq!(key_code("MENU"), Some(1));
        assert_eq!(key_code("nope"), None);
    }

    /// Real SSH round trip through a relay. Needs the fake relay + device
    /// containers from the README (`cargo test -- --ignored`), with
    /// ONION_REMOTE_TEST set to their pairing code and, optionally,
    /// ONION_REMOTE_TEST_LAN to the device's direct `host:port`.
    #[tokio::test]
    #[ignore]
    async fn end_to_end_through_relay() {
        let code = std::env::var("ONION_REMOTE_TEST").expect("ONION_REMOTE_TEST=user:pass@relay:port/tunnelport");
        let dev = connect(&parse_target(&code).unwrap()).await.unwrap();
        if let Ok(lan) = std::env::var("ONION_REMOTE_TEST_LAN") {
            connect(&parse_target(&lan).unwrap()).await.unwrap();
        }
        let roms = list_dir(&dev.sftp, "/mnt/SDCARD/Roms").await.unwrap();
        assert!(roms.iter().any(|x| x.name == "GB" && x.dir));

        let tmp = std::env::temp_dir().join("onion-remote-test.bin");
        let payload: Vec<u8> = (0..200_000u32).map(|i| (i % 251) as u8).collect();
        std::fs::write(&tmp, &payload).unwrap();
        let mut f = tokio::fs::File::open(&tmp).await.unwrap();
        let remote = "/mnt/SDCARD/Roms/GB/up load's.bin";
        let mut sent = 0;
        let status = exec(&dev.ssh, &format!("cat > {}", q(remote)), Some(&mut f), |d| sent = d, |_| {}).await.unwrap();
        assert_eq!((status, sent), (0, payload.len() as u64));
        let entry = list_dir(&dev.sftp, "/mnt/SDCARD/Roms/GB").await.unwrap().into_iter().find(|x| x.name == "up load's.bin").unwrap();
        assert_eq!((entry.dir, entry.size), (false, payload.len() as u64));

        let (status, back) = run(&dev.ssh, &format!("cat {}", q(remote))).await.unwrap();
        assert_eq!(status, 0);
        assert_eq!(back, payload);

        dev.sftp.create_dir("/mnt/SDCARD/Roms/GB/new dir").await.unwrap();
        assert_eq!(run(&dev.ssh, &format!("rm -rf {} {}", q(remote), q("/mnt/SDCARD/Roms/GB/new dir"))).await.unwrap().0, 0);
        let names: Vec<String> = list_dir(&dev.sftp, "/mnt/SDCARD/Roms/GB").await.unwrap().into_iter().map(|x| x.name).collect();
        assert_eq!(names, vec!["Tetris (World).gb"]);

        let (_, out) = run(&dev.ssh, "cat /mnt/SDCARD/.tmp_update/onionVersion/version.txt").await.unwrap();
        assert_eq!(String::from_utf8_lossy(&out).trim(), "v4.4.0-fake");
        assert!(run(&dev.ssh, "exit 3").await.unwrap().0 == 3);
        assert!(frame_to_png(&run(&dev.ssh, SCREEN_CMD).await.unwrap().1).is_err()); // no framebuffer in the fake
    }

    #[test]
    fn frame_is_rotated_swapped_and_gunzipped() {
        // top-left pixel blue (BGRA), rest black; after 180° it lands bottom-right.
        let mut raw = vec![0u8; FRAME];
        raw[..4].copy_from_slice(&[255, 0, 0, 255]);
        let png_bytes = frame_to_png(&raw).unwrap();
        let mut dec = png::Decoder::new(std::io::Cursor::new(&png_bytes)).read_info().unwrap();
        let mut buf = vec![0; dec.output_buffer_size().unwrap()];
        let info = dec.next_frame(&mut buf).unwrap();
        assert_eq!((info.width, info.height), (640, 480));
        assert_eq!(&buf[buf.len() - 3..], &[0, 0, 255]);
        assert_eq!(&buf[..3], &[0, 0, 0]);

        let mut gz = flate2::write::GzEncoder::new(Vec::new(), flate2::Compression::fast());
        std::io::Write::write_all(&mut gz, &raw).unwrap();
        assert_eq!(frame_to_png(&gz.finish().unwrap()).unwrap(), png_bytes);
        assert!(frame_to_png(&raw[..10]).is_err());
    }
}
