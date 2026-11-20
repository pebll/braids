// daemon.rs
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

//! Connection to braidsd over its Unix socket (doc/API.md). Starts braidsd
//! when it is not running, reconnects when it goes away, forwards events to
//! the web UI as `braids://event` and the connection state as
//! `braids://connection`.

use std::collections::HashMap;
use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::os::unix::process::CommandExt;
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use serde::Serialize;
use serde_json::{json, Value};
use tauri::{AppHandle, Emitter};

const CALL_TIMEOUT: Duration = Duration::from_secs(60);

#[derive(Debug, Serialize)]
pub struct ApiError {
    pub code: i64,
    pub message: String,
}

impl ApiError {
    fn new(code: i64, message: impl Into<String>) -> Self {
        ApiError { code, message: message.into() }
    }
}

#[derive(Clone)]
pub struct Daemon {
    inner: Arc<Inner>,
}

struct Inner {
    socket: PathBuf,
    confdir: Option<PathBuf>,
    writer: Mutex<Option<UnixStream>>,
    pending: Mutex<HashMap<u64, mpsc::Sender<Value>>>,
    next_id: AtomicU64,
    connected: AtomicBool,
}

/// Config dir given to braidsd, if not its default (`BRAIDS_CONFDIR`).
fn custom_confdir() -> Option<PathBuf> {
    std::env::var_os("BRAIDS_CONFDIR").filter(|v| !v.is_empty()).map(PathBuf::from)
}

/// Whether this app uses its own profile or daemon (`BRAIDS_CONFDIR` or
/// `BRAIDS_SOCKET`, e.g. tests): such instances may run next to the normal one.
pub fn custom_profile() -> bool {
    custom_confdir().is_some() || std::env::var_os("BRAIDS_SOCKET").is_some_and(|v| !v.is_empty())
}

/// Same rules as braidsd's ApiServer::GetDefaultSocketPath.
fn socket_path(confdir: &Option<PathBuf>) -> PathBuf {
    if let Some(path) = std::env::var_os("BRAIDS_SOCKET").filter(|v| !v.is_empty()) {
        return PathBuf::from(path);
    }
    if let Some(dir) = confdir {
        return dir.join("braidsd.sock");
    }
    if let Some(runtime) = std::env::var_os("XDG_RUNTIME_DIR") {
        if runtime.to_string_lossy().starts_with('/') {
            return PathBuf::from(runtime).join("braids").join("braidsd.sock");
        }
    }
    let config_home = std::env::var_os("XDG_CONFIG_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(std::env::var_os("HOME").unwrap_or_default()).join(".config"));
    config_home.join("braids").join("braidsd.sock")
}

/// braidsd next to the app (bundled), from the build tree in development,
/// or from PATH; `BRAIDSD` overrides.
fn find_braidsd() -> PathBuf {
    if let Some(path) = std::env::var_os("BRAIDSD").filter(|v| !v.is_empty()) {
        return PathBuf::from(path);
    }
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            let bundled = dir.join("braidsd");
            if bundled.is_file() {
                return bundled;
            }
        }
    }
    let dev = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../build/bin/braidsd");
    if dev.is_file() {
        return dev;
    }
    PathBuf::from("braidsd")
}

impl Daemon {
    pub fn new() -> Self {
        let confdir = custom_confdir();
        Daemon {
            inner: Arc::new(Inner {
                socket: socket_path(&confdir),
                confdir,
                writer: Mutex::new(None),
                pending: Mutex::new(HashMap::new()),
                next_id: AtomicU64::new(1),
                connected: AtomicBool::new(false),
            }),
        }
    }

    pub fn is_connected(&self) -> bool {
        self.inner.connected.load(Ordering::SeqCst)
    }

    /// Connects in the background, for the lifetime of the app.
    pub fn start(&self, app: AppHandle) {
        let inner = self.inner.clone();
        thread::spawn(move || {
            let mut last_spawn: Option<Instant> = None;
            let checked = Arc::new(AtomicBool::new(false));
            loop {
                match UnixStream::connect(&inner.socket) {
                    Ok(stream) => {
                        if !checked.swap(true, Ordering::SeqCst) {
                            let daemon = Daemon { inner: inner.clone() };
                            // tauri.conf.json's version, as braidsd's (lib/ncutil/CMakeLists.txt)
                            let version = app.package_info().version.to_string();
                            thread::spawn(move || daemon.restart_if_outdated(&version));
                        }
                        inner.run(&app, stream);
                        thread::sleep(Duration::from_millis(500));
                    }
                    Err(_) => {
                        // start braidsd at most every 10 s, it may take a moment to listen
                        if last_spawn.map_or(true, |t| t.elapsed() > Duration::from_secs(10)) {
                            last_spawn = Some(Instant::now());
                            inner.spawn_braidsd();
                        }
                        thread::sleep(Duration::from_millis(250));
                    }
                }
            }
        });
    }

    /// A braidsd that keeps running across app restarts would hide an update
    /// (or a rebuild): restart it when its version differs from the app's
    /// (an update; Nix gives every file the same 1970 time, issue #53) or its
    /// binary is newer than the process (a rebuild in development).
    fn restart_if_outdated(&self, app_version: &str) {
        for _ in 0..50 {
            if self.is_connected() {
                break;
            }
            thread::sleep(Duration::from_millis(100));
        }
        let Ok(hello) = self.call("hello", json!({})) else { return };
        let version = hello.get("version").and_then(Value::as_str).unwrap_or("");
        let start_ms = hello.get("startTime").and_then(Value::as_i64);
        let modified_ms = std::fs::metadata(find_braidsd())
            .and_then(|m| m.modified())
            .ok()
            .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
            .map(|d| d.as_millis() as i64);
        if is_outdated(version, app_version, start_ms, modified_ms) {
            eprintln!("braids: the running braidsd ({version}) is outdated (app {app_version}), restarting it");
            let _ = self.call("daemon.shutdown", json!({}));
        }
    }

    /// One API call; blocks until the reply (use from a blocking task).
    pub fn call(&self, method: &str, params: Value) -> Result<Value, ApiError> {
        let inner = &self.inner;
        let id = inner.next_id.fetch_add(1, Ordering::SeqCst);
        let (tx, rx) = mpsc::channel();
        inner.pending.lock().unwrap().insert(id, tx);

        let line = json!({ "jsonrpc": "2.0", "id": id, "method": method, "params": params }).to_string() + "\n";
        let written = {
            let mut writer = inner.writer.lock().unwrap();
            match writer.as_mut() {
                Some(stream) => stream.write_all(line.as_bytes()).is_ok(),
                None => false,
            }
        };
        if !written {
            inner.pending.lock().unwrap().remove(&id);
            return Err(ApiError::new(-32000, "braids core is not running"));
        }

        let reply = match rx.recv_timeout(CALL_TIMEOUT) {
            Ok(reply) => reply,
            Err(mpsc::RecvTimeoutError::Timeout) => {
                inner.pending.lock().unwrap().remove(&id);
                return Err(ApiError::new(-32000, format!("no reply to {method}")));
            }
            Err(mpsc::RecvTimeoutError::Disconnected) => {
                return Err(ApiError::new(-32000, "connection to braids core lost"));
            }
        };

        if let Some(error) = reply.get("error") {
            return Err(ApiError::new(
                error.get("code").and_then(Value::as_i64).unwrap_or(-32603),
                error.get("message").and_then(Value::as_str).unwrap_or("error").to_string(),
            ));
        }
        Ok(reply.get("result").cloned().unwrap_or(Value::Null))
    }
}

impl Inner {
    fn spawn_braidsd(&self) {
        let binary = find_braidsd();
        let mut cmd = Command::new(&binary);
        if let Some(dir) = &self.confdir {
            cmd.arg("--confdir").arg(dir);
        }
        if std::env::var_os("BRAIDS_SOCKET").is_some() {
            cmd.arg("--socket").arg(&self.socket);
        }
        // own process group: braidsd keeps running when the app quits
        cmd.stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).process_group(0);
        match cmd.spawn() {
            Ok(mut child) => {
                eprintln!("braids: started {}", binary.display());
                thread::spawn(move || {
                    let _ = child.wait();
                });
            }
            Err(e) => eprintln!("braids: cannot start {}: {e}", binary.display()),
        }
    }

    fn run(&self, app: &AppHandle, stream: UnixStream) {
        let reader = match stream.try_clone() {
            Ok(s) => BufReader::new(s),
            Err(_) => return,
        };
        *self.writer.lock().unwrap() = Some(stream);
        self.connected.store(true, Ordering::SeqCst);
        let _ = app.emit("braids://connection", true);

        for line in reader.lines() {
            let Ok(line) = line else { break };
            if line.trim().is_empty() {
                continue;
            }
            let Ok(msg) = serde_json::from_str::<Value>(&line) else { continue };
            match (msg.get("id"), msg.get("method").and_then(Value::as_str)) {
                (Some(id), None) => {
                    if let Some(tx) = id.as_u64().and_then(|id| self.pending.lock().unwrap().remove(&id)) {
                        let _ = tx.send(msg);
                    }
                }
                (_, Some("ready")) => {}
                (_, Some(_)) => {
                    let _ = app.emit("braids://event", msg);
                }
                _ => {}
            }
        }

        *self.writer.lock().unwrap() = None;
        self.connected.store(false, Ordering::SeqCst);
        // dropping the senders fails the waiting calls
        self.pending.lock().unwrap().clear();
        let _ = app.emit("braids://connection", false);
    }
}

/// Whether a running braidsd must be restarted: another version than the app
/// (empty: an old braidsd that does not say), or a binary newer than the
/// process.
fn is_outdated(daemon_version: &str, app_version: &str, start_ms: Option<i64>, modified_ms: Option<i64>) -> bool {
    if daemon_version != app_version {
        return true;
    }
    matches!((start_ms, modified_ms), (Some(start), Some(modified)) if modified > start)
}

#[cfg(test)]
mod tests {
    use super::is_outdated;

    #[test]
    fn restarts_another_version() {
        // Nix: every file is from 1970, only the version tells
        assert!(is_outdated("0.3.2", "0.3.6", Some(1_700_000_000_000), Some(1_000)));
        assert!(is_outdated("", "0.3.6", None, None));
    }

    #[test]
    fn restarts_a_rebuilt_binary() {
        assert!(is_outdated("0.3.6", "0.3.6", Some(1_000), Some(2_000)));
    }

    #[test]
    fn keeps_a_current_daemon() {
        assert!(!is_outdated("0.3.6", "0.3.6", Some(2_000), Some(1_000)));
        assert!(!is_outdated("0.3.6", "0.3.6", Some(1_700_000_000_000), Some(1_000)));
        assert!(!is_outdated("0.3.6", "0.3.6", None, None));
    }
}
