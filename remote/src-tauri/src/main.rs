#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod ssh;

use serde::Serialize;
use ssh::{e, q, Device};
use tauri::{AppHandle, Emitter, State};
use tokio::sync::Mutex;

type Conn = Mutex<Option<Device>>;

#[derive(Serialize, Clone)]
struct Progress {
    done: u64,
    total: u64,
    msg: String,
}

fn emit(app: &AppHandle, done: u64, total: u64, msg: &str) {
    let _ = app.emit("progress", Progress { done, total, msg: msg.into() });
}

async fn with_dev<T>(
    conn: &Conn,
    f: impl AsyncFnOnce(&Device) -> Result<T, String>,
) -> Result<T, String> {
    let guard = conn.lock().await;
    let dev = guard.as_ref().ok_or("não conectado")?;
    f(dev).await
}

#[derive(Serialize)]
struct Connected {
    relay: bool,
    version: String,
}

/// `code` is the Miyoo's LAN IP (`host[:port]`) or a relay pairing code.
#[tauri::command]
async fn connect(conn: State<'_, Conn>, code: String) -> Result<Connected, String> {
    let target = ssh::parse_target(&code)?;
    let dev = ssh::connect(&target).await?;
    let (_, out) = ssh::run(&dev.ssh, "cat /mnt/SDCARD/.tmp_update/onionVersion/version.txt 2>/dev/null").await?;
    let version = String::from_utf8_lossy(&out).trim().to_string();
    *conn.lock().await = Some(dev);
    Ok(Connected { relay: matches!(target, ssh::Target::Relay(_)), version })
}

#[tauri::command]
async fn disconnect(conn: State<'_, Conn>) -> Result<(), String> {
    if let Some(dev) = conn.lock().await.take() {
        let _ = dev.ssh.disconnect(russh::Disconnect::ByApplication, "", "").await;
    }
    Ok(())
}

#[tauri::command]
async fn list_dir(conn: State<'_, Conn>, path: String) -> Result<Vec<ssh::Entry>, String> {
    with_dev(&conn, async |d| ssh::list_dir(&d.sftp, &path).await).await
}

#[tauri::command]
async fn mkdir(conn: State<'_, Conn>, path: String) -> Result<(), String> {
    with_dev(&conn, async |d| d.sftp.create_dir(path).await.map_err(e)).await
}

#[tauri::command]
async fn remove(conn: State<'_, Conn>, path: String) -> Result<(), String> {
    with_dev(&conn, async |d| {
        let (status, out) = ssh::run(&d.ssh, &format!("rm -rf {}", q(&path))).await?;
        if status == 0 { Ok(()) } else { Err(String::from_utf8_lossy(&out).trim().to_string()) }
    })
    .await
}

/// Stream `remote` (of `size` bytes, for progress) into the local file `local`.
#[tauri::command]
async fn download(app: AppHandle, conn: State<'_, Conn>, remote: String, local: String, size: u64) -> Result<(), String> {
    with_dev(&conn, async |d| {
        // ponytail: blocking writes of 32-256 KiB chunks to a local disk, fine for this UI
        let mut f = std::fs::File::create(&local).map_err(e)?;
        let mut done = 0u64;
        let mut err = None;
        let status = ssh::exec(&d.ssh, &format!("cat {}", q(&remote)), None, |_| {}, |chunk| {
            if err.is_none() {
                err = std::io::Write::write_all(&mut f, chunk).err().map(e);
                done += chunk.len() as u64;
                emit(&app, done, size, &remote);
            }
        })
        .await?;
        if let Some(x) = err {
            return Err(x);
        }
        if status != 0 {
            return Err(format!("cat saiu com status {status}"));
        }
        Ok(())
    })
    .await
}

/// Stream the local file `local` into `remote` on the device.
#[tauri::command]
async fn upload(app: AppHandle, conn: State<'_, Conn>, local: String, remote: String) -> Result<(), String> {
    with_dev(&conn, async |d| {
        let mut f = tokio::fs::File::open(&local).await.map_err(e)?;
        let total = f.metadata().await.map_err(e)?.len();
        let mut out = Vec::new();
        let status = ssh::exec(
            &d.ssh,
            &format!("cat > {} && sync", q(&remote)),
            Some(&mut f),
            |done| emit(&app, done, total, &remote),
            |chunk| out.extend_from_slice(chunk),
        )
        .await?;
        if status != 0 {
            return Err(String::from_utf8_lossy(&out).trim().to_string());
        }
        Ok(())
    })
    .await
}

/// Current screen as PNG bytes.
#[tauri::command]
async fn screen(conn: State<'_, Conn>) -> Result<tauri::ipc::Response, String> {
    with_dev(&conn, async |d| {
        let (_, raw) = ssh::run(&d.ssh, ssh::SCREEN_CMD).await?;
        Ok(tauri::ipc::Response::new(ssh::frame_to_png(&raw)?))
    })
    .await
}

#[tauri::command]
async fn press(conn: State<'_, Conn>, key: String, hold_ms: u32) -> Result<(), String> {
    let code = ssh::key_code(&key).ok_or(format!("botão desconhecido: {key}"))?;
    with_dev(&conn, async |d| ssh::run(&d.ssh, &ssh::key_cmd(code, hold_ms)).await.map(|_| ())).await
}

/// Terminal: run a shell command, return its output.
#[tauri::command]
async fn run(conn: State<'_, Conn>, cmd: String) -> Result<String, String> {
    with_dev(&conn, async |d| {
        let (status, out) = ssh::run(&d.ssh, &cmd).await?;
        let mut s = String::from_utf8_lossy(&out).to_string();
        if status != 0 {
            s.push_str(&format!("\n[status {status}]"));
        }
        Ok(s)
    })
    .await
}

/// Write the relay pairing code on the device (over the current connection,
/// normally LAN), switch "Allow remote access" + SSH on and (re)start the tunnel.
#[tauri::command]
async fn pair(conn: State<'_, Conn>, code: String) -> Result<(), String> {
    let relay = ssh::parse_relay(&code)?;
    let line = format!("{}:{}@{}:{}/{}", relay.user, relay.pass, relay.host, relay.port, relay.tunnel_port);
    let cfg = "/mnt/SDCARD/.tmp_update/config";
    let cmd = format!(
        "printf '%s\\n' {} > {cfg}/remote_relay && touch {cfg}/.remoteState {cfg}/.sshState && sync && \
         pkill -9 -f remote_tunnel.sh; killall -9 dbclient 2>/dev/null; \
         /mnt/SDCARD/.tmp_update/script/network/update_networking.sh remote toggle",
        q(&line)
    );
    with_dev(&conn, async |d| {
        let (status, out) = ssh::run(&d.ssh, &cmd).await?;
        if status != 0 {
            return Err(String::from_utf8_lossy(&out).trim().to_string());
        }
        Ok(())
    })
    .await
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(Conn::new(None))
        .invoke_handler(tauri::generate_handler![
            connect, disconnect, list_dir, mkdir, remove, download, upload, screen, press, run, pair
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
