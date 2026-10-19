#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use futures_util::StreamExt;
use serde::Serialize;
use std::{
    fs, io,
    path::{Path, PathBuf},
};
use tauri::{AppHandle, Emitter};

fn e(err: impl std::fmt::Display) -> String {
    err.to_string()
}

fn client() -> reqwest::Client {
    reqwest::Client::builder()
        .user_agent("onion-installer")
        .build()
        .expect("http client")
}

#[derive(Serialize, Clone)]
struct Progress {
    phase: String,
    done: u64,
    total: u64,
    msg: String,
}

fn emit(app: &AppHandle, phase: &str, done: u64, total: u64, msg: &str) {
    let _ = app.emit(
        "progress",
        Progress { phase: phase.into(), done, total, msg: msg.into() },
    );
}

#[derive(Serialize)]
struct Release {
    tag: String,
    name: String,
    prerelease: bool,
    asset: String,
    url: String,
    size: u64,
}

/// GitHub releases of `owner/repo` that carry an `Onion-v*.zip` asset.
#[tauri::command]
async fn list_releases(repo: String) -> Result<Vec<Release>, String> {
    let url = format!("https://api.github.com/repos/{repo}/releases?per_page=30");
    let v: serde_json::Value = client()
        .get(url)
        .send()
        .await
        .map_err(e)?
        .error_for_status()
        .map_err(e)?
        .json()
        .await
        .map_err(e)?;
    let s = |x: &serde_json::Value| x.as_str().unwrap_or("").to_string();
    let mut out = Vec::new();
    for r in v.as_array().cloned().unwrap_or_default() {
        for a in r["assets"].as_array().cloned().unwrap_or_default() {
            let asset = s(&a["name"]);
            if asset.starts_with("Onion-v") && asset.ends_with(".zip") {
                out.push(Release {
                    tag: s(&r["tag_name"]),
                    name: s(&r["name"]),
                    prerelease: r["prerelease"].as_bool().unwrap_or(false),
                    asset,
                    url: s(&a["browser_download_url"]),
                    size: a["size"].as_u64().unwrap_or(0),
                });
            }
        }
    }
    Ok(out)
}

#[derive(Serialize)]
struct SdInfo {
    onion_installed: bool,
    version: Option<String>,
}

#[tauri::command]
fn sd_info(path: String) -> Result<SdInfo, String> {
    let p = Path::new(&path);
    if !p.is_dir() {
        return Err("Pasta não encontrada".into());
    }
    let version = fs::read_to_string(p.join(".tmp_update/onionVersion/version.txt"))
        .ok()
        .map(|s| s.trim().to_string());
    Ok(SdInfo { onion_installed: p.join(".tmp_update").is_dir(), version })
}

/// Install or update: fetch the release zip (or use a local one) and unpack it
/// over the SD card root. Onion's own on-device updater finishes the job on boot.
#[tauri::command]
async fn install(
    app: AppHandle,
    sd: String,
    url: Option<String>,
    file: Option<String>,
) -> Result<String, String> {
    let sd = PathBuf::from(sd);
    if !sd.is_dir() {
        return Err("Pasta do SD não encontrada".into());
    }
    let zip_path = match (url, file) {
        (Some(u), _) if !u.is_empty() => download(&app, &u).await?,
        (_, Some(f)) if !f.is_empty() => PathBuf::from(f),
        _ => return Err("Escolha uma versão ou um arquivo .zip".into()),
    };
    let (app2, sd2) = (app.clone(), sd.clone());
    tauri::async_runtime::spawn_blocking(move || {
        extract(&zip_path, &sd2, &mut |done, total, msg| emit(&app2, "extract", done, total, msg))
    })
    .await
    .map_err(e)??;
    Ok(format!("Onion copiado para {}", sd.display()))
}

async fn download(app: &AppHandle, url: &str) -> Result<PathBuf, String> {
    let resp = client()
        .get(url)
        .send()
        .await
        .map_err(e)?
        .error_for_status()
        .map_err(e)?;
    let total = resp.content_length().unwrap_or(0);
    let path = std::env::temp_dir().join("onion-installer.zip");
    let mut f = fs::File::create(&path).map_err(e)?;
    let mut stream = resp.bytes_stream();
    let mut done = 0u64;
    while let Some(chunk) = stream.next().await {
        let c = chunk.map_err(e)?;
        io::Write::write_all(&mut f, &c).map_err(e)?;
        done += c.len() as u64;
        emit(app, "download", done, total, "");
    }
    Ok(path)
}

fn extract(
    zip_path: &Path,
    sd: &Path,
    progress: &mut dyn FnMut(u64, u64, &str),
) -> Result<(), String> {
    let f = fs::File::open(zip_path).map_err(e)?;
    let mut z = zip::ZipArchive::new(f).map_err(e)?;
    let total = z.len() as u64;
    for i in 0..z.len() {
        let mut entry = z.by_index(i).map_err(e)?;
        // enclosed_name() rejects "../" and absolute paths (zip-slip): skip those
        let mut msg = String::new();
        if let Some(rel) = entry.enclosed_name() {
            let out = sd.join(&rel);
            if entry.is_dir() {
                fs::create_dir_all(&out).map_err(e)?;
            } else {
                if let Some(parent) = out.parent() {
                    fs::create_dir_all(parent).map_err(e)?;
                }
                let mut o = fs::File::create(&out).map_err(|x| format!("{}: {x}", out.display()))?;
                io::copy(&mut entry, &mut o).map_err(e)?;
            }
            msg = rel.display().to_string();
        }
        if i % 25 == 0 || i as u64 + 1 == total {
            progress(i as u64 + 1, total, &msg);
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    #[test]
    fn extracts_zip_over_sd_and_skips_zip_slip() {
        let tmp = std::env::temp_dir().join(format!("onion-inst-test-{}", std::process::id()));
        let sd = tmp.join("sd");
        fs::create_dir_all(&sd).unwrap();
        fs::write(sd.join("keep.txt"), "user file").unwrap();

        let zip_path = tmp.join("Onion-v0.0.0.zip");
        let mut w = zip::ZipWriter::new(fs::File::create(&zip_path).unwrap());
        let o = zip::write::SimpleFileOptions::default();
        w.add_directory("miyoo/app/.tmp_update/", o).unwrap();
        w.start_file("miyoo/app/.tmp_update/onion.pak", o).unwrap();
        w.write_all(b"pak").unwrap();
        w.start_file(".tmp_update/onionVersion/version.txt", o).unwrap();
        w.write_all(b"v9.9.9\n").unwrap();
        w.start_file("../evil.txt", o).unwrap();
        w.write_all(b"nope").unwrap();
        w.finish().unwrap();

        let mut last = (0, 0);
        extract(&zip_path, &sd, &mut |d, t, _| last = (d, t)).unwrap();

        assert_eq!(fs::read(sd.join("miyoo/app/.tmp_update/onion.pak")).unwrap(), b"pak");
        assert_eq!(fs::read_to_string(sd.join("keep.txt")).unwrap(), "user file");
        assert!(!tmp.join("evil.txt").exists());
        assert_eq!(last, (4, 4));

        let info = sd_info(sd.to_string_lossy().into()).unwrap();
        assert!(info.onion_installed);
        assert_eq!(info.version.as_deref(), Some("v9.9.9"));

        fs::remove_dir_all(&tmp).unwrap();
    }
}

/// Headless mode: `onion-installer --sd <dir> (--file <zip> | --url <zip-url>)`.
/// Same download/extract path as the GUI, progress on stderr.
fn cli(args: &[String]) -> Result<(), String> {
    let opt = |k: &str| args.iter().position(|a| a == k).map(|i| args[i + 1].clone());
    let sd = PathBuf::from(opt("--sd").ok_or("--sd <dir> obrigatório")?);
    if !sd.is_dir() {
        return Err("Pasta do SD não encontrada".into());
    }
    let zip_path = match (opt("--url"), opt("--file")) {
        (Some(u), _) => {
            let rt = tokio::runtime::Runtime::new().map_err(e)?;
            rt.block_on(async {
                let resp = client().get(&u).send().await.map_err(e)?.error_for_status().map_err(e)?;
                let path = std::env::temp_dir().join("onion-installer.zip");
                let mut f = fs::File::create(&path).map_err(e)?;
                let mut stream = resp.bytes_stream();
                while let Some(chunk) = stream.next().await {
                    io::Write::write_all(&mut f, &chunk.map_err(e)?).map_err(e)?;
                }
                Ok::<_, String>(path)
            })?
        }
        (None, Some(f)) => PathBuf::from(f),
        _ => return Err("--file <zip> ou --url <url> obrigatório".into()),
    };
    extract(&zip_path, &sd, &mut |d, t, m| eprintln!("[{d}/{t}] {m}"))?;
    println!("Onion copiado para {}", sd.display());
    Ok(())
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.iter().any(|a| a == "--sd") {
        if let Err(err) = cli(&args) {
            eprintln!("erro: {err}");
            std::process::exit(1);
        }
        return;
    }
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .invoke_handler(tauri::generate_handler![list_releases, sd_info, install])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
