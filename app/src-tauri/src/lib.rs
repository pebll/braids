// lib.rs
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

//! The braids app shell: connects the web UI to braidsd (daemon.rs), plus
//! the platform bits the UI cannot do itself (tray, badge, temp files).

mod daemon;
mod linkpreview;
mod voice;

use std::fs;
use std::io::Write;
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

use serde_json::Value;
use tauri::image::Image;
use tauri::menu::{Menu, MenuItem};
use tauri::tray::{MouseButton, MouseButtonState, TrayIcon, TrayIconBuilder, TrayIconEvent};
use tauri::{AppHandle, Manager, State, WindowEvent};

use daemon::{ApiError, Daemon};

struct AppState {
    daemon: Daemon,
    close_to_tray: AtomicBool,
    temp_counter: AtomicU64,
    tray_level: AtomicU64, // colored strands shown in the tray icon
}

/// The tray's id. On Linux the icon images are files named after it
/// ($XDG_RUNTIME_DIR/tray-icon/tray-icon-<id>-<n>.png, n counting from 0 at
/// every start). With a fixed id every start reused the same file, which
/// GNOME's indicator extension may keep cached from a moment it was missing
/// (an empty tray item until the next login); "main" is also shared with
/// other Tauri apps. So: "braids-<pid>", and old braids files are removed.
fn tray_id() -> &'static str {
    static ID: std::sync::OnceLock<String> = std::sync::OnceLock::new();
    ID.get_or_init(|| format!("braids-{}", std::process::id()))
}

/// Icon files of earlier braids runs (killed, so they could not clean up).
fn remove_old_tray_files() {
    let dir = std::env::var_os("XDG_RUNTIME_DIR").map(PathBuf::from).unwrap_or_else(std::env::temp_dir).join("tray-icon");
    let own = format!("tray-icon-{}-", tray_id());
    if let Ok(entries) = fs::read_dir(dir) {
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().into_owned();
            // only braids' own: one braids runs at a time (single instance)
            if name.starts_with("tray-icon-braids-") && !name.starts_with(&own) {
                let _ = fs::remove_file(entry.path());
            }
        }
    }
}

/// Tray icon for a number of colored strands: 0 monochrome (a template image
/// on macOS, white for the usual dark Linux panels), 1 / 2 / 3 for 1, 2 and
/// 3+ unread chats (app/scripts/tray-icons.sh).
fn tray_icon(level: u64) -> tauri::Result<Image<'static>> {
    let bytes: &'static [u8] = match level {
        0 if cfg!(target_os = "macos") => include_bytes!("../icons/tray-black.png"),
        0 => include_bytes!("../icons/tray-white.png"),
        1 => include_bytes!("../icons/tray-unread-1.png"),
        2 => include_bytes!("../icons/tray-unread-2.png"),
        _ => include_bytes!("../icons/tray-unread-3.png"),
    };
    Image::from_bytes(bytes)
}

#[tauri::command]
async fn api_call(state: State<'_, AppState>, method: String, params: Value) -> Result<Value, ApiError> {
    let daemon = state.daemon.clone();
    tauri::async_runtime::spawn_blocking(move || daemon.call(&method, params))
        .await
        .map_err(|e| ApiError { code: -32603, message: e.to_string() })?
}

/// Preview of a web page linked in a message (None: no preview).
#[tauri::command]
async fn link_preview(url: String) -> Option<linkpreview::Preview> {
    let cache = std::env::var_os("XDG_CACHE_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(std::env::var_os("HOME").unwrap_or_default()).join(".cache"))
        .join("braids");
    tauri::async_runtime::spawn_blocking(move || linkpreview::preview(&url, &cache)).await.ok().flatten()
}

#[derive(serde::Serialize)]
struct Update {
    version: String,
    url: String,
}

/// The latest release on GitHub when it is newer than this app (None: up to
/// date, or the check failed).
#[tauri::command]
async fn check_update() -> Option<Update> {
    tauri::async_runtime::spawn_blocking(|| {
        let body = ureq::AgentBuilder::new()
            .timeout(std::time::Duration::from_secs(10))
            .user_agent(concat!("braids/", env!("CARGO_PKG_VERSION")))
            .build()
            .get("https://api.github.com/repos/pebll/braids/releases/latest")
            .set("Accept", "application/vnd.github+json")
            .call()
            .ok()?
            .into_string()
            .ok()?;
        let release: Value = serde_json::from_str(&body).ok()?;
        let tag = release["tag_name"].as_str()?.trim_start_matches('v').to_string();
        let newer = version_parts(&tag) > version_parts(env!("CARGO_PKG_VERSION"));
        newer.then(|| Update { version: tag, url: release["html_url"].as_str().unwrap_or_default().to_string() })
    })
    .await
    .ok()
    .flatten()
}

fn version_parts(v: &str) -> Vec<u64> {
    v.split(['.', '-']).map_while(|p| p.parse().ok()).collect()
}

/// How this app was installed: "flatpak", "appimage" or "" (tarball, .deb,
/// development); start at login is not offered in a Flatpak.
#[tauri::command]
fn install_kind() -> &'static str {
    if std::env::var_os("FLATPAK_ID").is_some() {
        "flatpak"
    } else if std::env::var_os("APPIMAGE").is_some() {
        "appimage"
    } else {
        ""
    }
}

#[tauri::command]
fn daemon_connected(state: State<'_, AppState>) -> bool {
    state.daemon.is_connected()
}

#[tauri::command]
fn set_close_to_tray(state: State<'_, AppState>, enabled: bool) {
    state.close_to_tray.store(enabled, Ordering::SeqCst);
}

#[tauri::command]
fn set_badge(app: AppHandle, state: State<'_, AppState>, count: u32) {
    if let Some(window) = app.get_webview_window("main") {
        let _ = window.set_badge_count(if count > 0 { Some(count as i64) } else { None });
        let _ = window.set_title(&if count > 0 { format!("braids ({count})") } else { "braids".into() });
    }
    if let Some(tray) = app.tray_by_id(tray_id()) {
        let tip = match count { 0 => "braids".into(), 1 => "braids — 1 unread chat".into(), _ => format!("braids — {count} unread chats") };
        let _ = tray.set_tooltip(Some(&tip));
        let level = u64::from(count.min(3));
        if state.tray_level.swap(level, Ordering::SeqCst) != level {
            if let Ok(icon) = tray_icon(level) {
                let _ = tray.set_icon(Some(icon));
                let _ = tray.set_icon_as_template(level == 0);
            }
        }
    }
}

/// Saves pasted / dropped data to an owner-only temp file, returns its path.
#[tauri::command]
fn save_temp_file(state: State<'_, AppState>, name: String, data: Vec<u8>) -> Result<String, String> {
    let base = std::env::temp_dir().join(format!("braids-{}", std::process::id()));
    let n = state.temp_counter.fetch_add(1, Ordering::SeqCst);
    let dir = base.join(n.to_string());
    fs::DirBuilder::new().recursive(true).mode(0o700).create(&dir).map_err(|e| e.to_string())?;
    let file_name = PathBuf::from(&name).file_name().map(|s| s.to_owned()).unwrap_or_else(|| "file".into());
    let path = dir.join(file_name);
    let mut file = fs::OpenOptions::new().write(true).create_new(true).mode(0o600).open(&path).map_err(|e| e.to_string())?;
    file.write_all(&data).map_err(|e| e.to_string())?;
    Ok(path.to_string_lossy().into_owned())
}

/// Starts recording a voice message from the default microphone; the input
/// level arrives as `voice-level` events.
#[tauri::command]
fn voice_start(app: AppHandle, state: State<'_, AppState>) -> Result<(), String> {
    use tauri::Emitter;
    let base = std::env::temp_dir().join(format!("braids-{}", std::process::id()));
    let n = state.temp_counter.fetch_add(1, Ordering::SeqCst);
    let dir = base.join(n.to_string());
    fs::DirBuilder::new().recursive(true).mode(0o700).create(&dir).map_err(|e| e.to_string())?;
    voice::start("autoaudiosrc", dir.join("voice-message.ogg"), move |level| {
        let _ = app.emit("voice-level", level);
    })
}

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
struct VoiceClip {
    path: String,
    duration_ms: u64,
}

/// Stops the recording: the Ogg/Opus file and its length, or null when
/// cancelled (the file is deleted).
#[tauri::command]
async fn voice_stop(cancel: bool) -> Result<Option<VoiceClip>, String> {
    tauri::async_runtime::spawn_blocking(move || voice::stop(cancel))
        .await
        .map_err(|e| e.to_string())?
        .map(|clip| clip.map(|(path, duration_ms)| VoiceClip { path: path.to_string_lossy().into_owned(), duration_ms }))
}

/// Saves a copy of a received file where the user picks in the system save
/// dialog (starting in Downloads), returns the new path, "" if cancelled.
#[tauri::command]
async fn save_file_as(app: AppHandle, path: String, name: String) -> Result<String, String> {
    use tauri_plugin_dialog::DialogExt;
    // the attachment's name, else the cached file's
    let file_name = [&name, &path]
        .iter()
        .find_map(|p| PathBuf::from(p).file_name().map(|n| n.to_string_lossy().into_owned()))
        .unwrap_or_else(|| "file".into());
    let mut dialog = app.dialog().file().set_file_name(&file_name);
    if let Ok(dir) = app.path().download_dir() {
        dialog = dialog.set_directory(dir);
    }
    // blocking is fine here: async commands run off the main thread
    let Some(target) = dialog.blocking_save_file() else { return Ok(String::new()) };
    let target = target.into_path().map_err(|e| e.to_string())?;
    fs::copy(&path, &target).map_err(|e| e.to_string())?;
    Ok(target.to_string_lossy().into_owned())
}

/// The shipped third-party notices file: in the app's resources, or in the
/// release tree (bin/../share/doc/braids); "" when not installed (development).
#[tauri::command]
fn licenses_path(app: AppHandle) -> String {
    let resource = app.path().resolve("THIRD_PARTY_LICENSES", tauri::path::BaseDirectory::Resource).ok();
    let tree = std::env::current_exe()
        .ok()
        .and_then(|exe| exe.parent().map(|dir| dir.join("../share/doc/braids/THIRD_PARTY_LICENSES")));
    [resource, tree]
        .into_iter()
        .flatten()
        .find(|p| p.is_file())
        .and_then(|p| p.canonicalize().ok())
        .map(|p| p.to_string_lossy().into_owned())
        .unwrap_or_default()
}

fn show_window(app: &AppHandle) {
    if let Some(window) = app.get_webview_window("main") {
        let _ = window.unminimize();
        let _ = window.show();
        let _ = window.set_focus();
    }
}

fn build_tray(app: &AppHandle) -> tauri::Result<TrayIcon> {
    let open = MenuItem::with_id(app, "open", "Open braids", true, None::<&str>)?;
    let quit = MenuItem::with_id(app, "quit", "Quit braids", true, None::<&str>)?;
    let menu = Menu::with_items(app, &[&open, &quit])?;

    remove_old_tray_files();
    let icon = tray_icon(0)?;

    TrayIconBuilder::with_id(tray_id())
        .icon(icon)
        .icon_as_template(true)
        .tooltip("braids")
        .menu(&menu)
        .show_menu_on_left_click(false)
        .on_menu_event(|app, event| match event.id().as_ref() {
            "open" => show_window(app),
            "quit" => app.exit(0),
            _ => {}
        })
        .on_tray_icon_event(|tray, event| {
            if let TrayIconEvent::Click { button: MouseButton::Left, button_state: MouseButtonState::Up, .. } = event {
                show_window(tray.app_handle());
            }
        })
        .build(app)
}

// The app's own pages: the bundled frontend (tauri://localhost,
// http(s)://tauri.localhost), the dev server in debug builds.
fn is_app_url(url: &tauri::Url) -> bool {
    match url.scheme() {
        "tauri" | "about" | "data" | "blob" => true,
        "http" | "https" => match url.host_str() {
            Some("tauri.localhost") => true,
            Some("localhost") => cfg!(debug_assertions),
            _ => false,
        },
        _ => false,
    }
}

// The window never navigates away from braids (#20: a link could replace
// the whole app with no way back): web pages and other links go to the
// system's default app instead.
fn navigation_guard<R: tauri::Runtime>() -> tauri::plugin::TauriPlugin<R> {
    tauri::plugin::Builder::new("navigation-guard")
        .on_navigation(|_webview, url| {
            if is_app_url(url) {
                return true;
            }
            if matches!(url.scheme(), "http" | "https" | "mailto" | "tel") {
                let _ = tauri_plugin_opener::open_url(url.as_str(), None::<&str>);
            }
            false
        })
        .build()
}

pub fn run() {
    let mut builder = tauri::Builder::default();
    // one braids per profile: starting it again shows the running window
    // (registered first, so a second instance exits before doing anything)
    if !daemon::custom_profile() {
        builder = builder.plugin(tauri_plugin_single_instance::init(|app, _args, _cwd| show_window(app)));
    }
    builder
        // started at login with --minimized: only the tray icon
        .plugin(tauri_plugin_autostart::init(tauri_plugin_autostart::MacosLauncher::LaunchAgent, Some(vec!["--minimized"])))
        .plugin(tauri_plugin_clipboard_manager::init())
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_notification::init())
        .plugin(tauri_plugin_opener::init())
        .plugin(navigation_guard())
        .manage(AppState {
            daemon: Daemon::new(),
            close_to_tray: AtomicBool::new(true),
            temp_counter: AtomicU64::new(1),
            tray_level: AtomicU64::new(0),
        })
        .setup(|app| {
            let handle = app.handle().clone();
            app.state::<AppState>().daemon.start(handle.clone());
            let has_tray = match build_tray(&handle) {
                Ok(_) => true,
                Err(e) => {
                    // no tray (e.g. no status notifier host): closing the window quits
                    eprintln!("braids: no tray icon: {e}");
                    app.state::<AppState>().close_to_tray.store(false, Ordering::SeqCst);
                    false
                }
            };
            // the window starts hidden (tauri.conf.json): shown unless started
            // at login into the tray
            let minimized = std::env::args().any(|a| a == "--minimized");
            if !(minimized && has_tray) {
                show_window(&handle);
            }
            Ok(())
        })
        .on_window_event(|window, event| {
            if let WindowEvent::CloseRequested { api, .. } = event {
                let state = window.state::<AppState>();
                if state.close_to_tray.load(Ordering::SeqCst) && window.app_handle().tray_by_id(tray_id()).is_some() {
                    api.prevent_close();
                    let _ = window.hide();
                }
            }
        })
        .invoke_handler(tauri::generate_handler![api_call, daemon_connected, set_close_to_tray, set_badge, save_temp_file, save_file_as, licenses_path, voice_start, voice_stop, link_preview, check_update, install_kind])
        .run(tauri::generate_context!())
        .expect("error while running braids");
}

#[cfg(test)]
mod tests {
    #[test]
    fn app_urls() {
        let ok = |u: &str| super::is_app_url(&tauri::Url::parse(u).unwrap());
        assert!(ok("tauri://localhost/index.html"));
        assert!(ok("http://tauri.localhost/"));
        assert!(!ok("https://grigio.org/vibe-coding"));
        assert!(!ok("https://tauri.localhost.evil.example/"));
        assert!(!ok("file:///etc/passwd"));
    }
}
