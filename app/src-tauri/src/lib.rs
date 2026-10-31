// lib.rs
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

//! The braids app shell: connects the web UI to braidsd (daemon.rs), plus
//! the platform bits the UI cannot do itself (tray, badge, temp files).

mod daemon;

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
    if let Some(tray) = app.tray_by_id("main") {
        let tip = if count > 0 { format!("braids — {count} unread") } else { "braids".into() };
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

/// Copies a received file into the user's Downloads folder under its name
/// (never overwriting: "name (1).ext" etc.), returns the new path.
#[tauri::command]
fn save_to_downloads(app: AppHandle, path: String, name: String) -> Result<String, String> {
    let dir = app.path().download_dir().map_err(|e| e.to_string())?;
    fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    // the attachment's name, else the cached file's
    let file_name = [&name, &path]
        .iter()
        .find_map(|p| PathBuf::from(p).file_name().map(|n| n.to_owned()))
        .unwrap_or_else(|| "file".into());
    let stem = PathBuf::from(&file_name).file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
    let ext = PathBuf::from(&file_name).extension().map(|s| format!(".{}", s.to_string_lossy())).unwrap_or_default();
    for n in 0..1000 {
        let target = if n == 0 { dir.join(&file_name) } else { dir.join(format!("{stem} ({n}){ext}")) };
        match fs::OpenOptions::new().write(true).create_new(true).open(&target) {
            Ok(mut out) => {
                let mut input = fs::File::open(&path).map_err(|e| e.to_string())?;
                std::io::copy(&mut input, &mut out).map_err(|e| e.to_string())?;
                return Ok(target.to_string_lossy().into_owned());
            }
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(e) => return Err(e.to_string()),
        }
    }
    Err("too many files with this name".into())
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

    let icon = tray_icon(0)?;

    TrayIconBuilder::with_id("main")
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

pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_notification::init())
        .plugin(tauri_plugin_opener::init())
        .manage(AppState {
            daemon: Daemon::new(),
            close_to_tray: AtomicBool::new(true),
            temp_counter: AtomicU64::new(1),
            tray_level: AtomicU64::new(0),
        })
        .setup(|app| {
            let handle = app.handle().clone();
            app.state::<AppState>().daemon.start(handle.clone());
            if let Err(e) = build_tray(&handle) {
                // no tray (e.g. no status notifier host): closing the window quits
                eprintln!("braids: no tray icon: {e}");
                app.state::<AppState>().close_to_tray.store(false, Ordering::SeqCst);
            }
            Ok(())
        })
        .on_window_event(|window, event| {
            if let WindowEvent::CloseRequested { api, .. } = event {
                let state = window.state::<AppState>();
                if state.close_to_tray.load(Ordering::SeqCst) && window.app_handle().tray_by_id("main").is_some() {
                    api.prevent_close();
                    let _ = window.hide();
                }
            }
        })
        .invoke_handler(tauri::generate_handler![api_call, daemon_connected, set_close_to_tray, set_badge, save_temp_file, save_to_downloads])
        .run(tauri::generate_context!())
        .expect("error while running braids");
}
