// update.rs
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

//! An update while braids keeps running in the tray: starting braids again
//! reaches the running app (single instance), which then finds out whether
//! the start came from another installation (a new Nix store path, issue #53)
//! or its own program file was replaced (.deb, install script), and offers a
//! restart into the new version.

use std::path::{Path, PathBuf};
use std::process::Command;

/// The program a second start of braids ran, when it is an update of the
/// running one. Not for AppImage and Flatpak: an AppImage's files go away with
/// its mount, a Flatpak's path is the same for every version.
pub fn updated_program(args: &[String], cwd: &str) -> Option<PathBuf> {
    if std::env::var_os("APPIMAGE").is_some() || std::env::var_os("FLATPAK_ID").is_some() {
        return None;
    }
    let current = std::env::current_exe().ok()?;
    let launched = args.first().and_then(|arg0| resolve(arg0, Path::new(cwd)));
    is_update(&current, launched.as_deref())
}

/// The update to start instead of `current` (the running program, as
/// /proc/self/exe names it), given the program a new start ran.
fn is_update(current: &Path, launched: Option<&Path>) -> Option<PathBuf> {
    let current_str = current.to_string_lossy();
    // replaced in place: Linux names the old file "<path> (deleted)"
    if let Some(path) = current_str.strip_suffix(" (deleted)") {
        return Some(launched.map(Path::to_path_buf).unwrap_or_else(|| PathBuf::from(path)));
    }
    let launched = launched?;
    // compare directories: Nix runs a wrapper (bin/braids) around the program
    // (bin/.braids-wrapped), both in the same store path
    let dir = |p: &Path| p.canonicalize().ok().and_then(|p| p.parent().map(Path::to_path_buf));
    match (dir(current), dir(launched)) {
        (Some(a), Some(b)) if a != b => Some(launched.to_path_buf()),
        _ => None,
    }
}

/// A program as a shell would find it: a path (relative to `cwd`), or a name
/// looked up in PATH.
fn resolve(arg0: &str, cwd: &Path) -> Option<PathBuf> {
    if arg0.contains('/') {
        let path = cwd.join(arg0);
        return path.is_file().then_some(path);
    }
    std::env::split_paths(&std::env::var_os("PATH")?).map(|dir| dir.join(arg0)).find(|p| p.is_file())
}

/// Starts `program` once this process has exited (it would otherwise reach
/// this instance again); the caller exits right after.
pub fn restart_into(program: &Path) -> std::io::Result<()> {
    Command::new("sh")
        .arg("-c")
        .arg(r#"while kill -0 "$1" 2>/dev/null; do sleep 0.2; done; exec "$0""#)
        .arg(program)
        .arg(std::process::id().to_string())
        .spawn()
        .map(|_| ())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    fn program(dir: &Path, name: &str) -> PathBuf {
        fs::create_dir_all(dir).unwrap();
        let path = dir.join(name);
        fs::write(&path, "").unwrap();
        path
    }

    #[test]
    fn another_installation_is_an_update() {
        let tmp = std::env::temp_dir().join(format!("braids-update-test-{}", std::process::id()));
        let old = program(&tmp.join("old/bin"), ".braids-wrapped");
        let new = program(&tmp.join("new/bin"), "braids");
        let wrapper = program(&tmp.join("old/bin"), "braids");
        assert_eq!(is_update(&old, Some(&new)), Some(new.clone()));
        // the wrapper of the running program: the same installation
        assert_eq!(is_update(&old, Some(&wrapper)), None);
        assert_eq!(is_update(&old, None), None);
        let _ = fs::remove_dir_all(&tmp);
    }

    #[test]
    fn a_replaced_program_is_an_update() {
        let current = Path::new("/usr/bin/braids (deleted)");
        assert_eq!(is_update(current, None), Some(PathBuf::from("/usr/bin/braids")));
        assert_eq!(is_update(current, Some(Path::new("/usr/bin/braids"))), Some(PathBuf::from("/usr/bin/braids")));
    }

    #[test]
    fn resolves_paths_and_names() {
        assert_eq!(resolve("sh", Path::new("/")).map(|p| p.is_file()), Some(true));
        assert_eq!(resolve("./no-such-braids", Path::new("/")), None);
    }
}
