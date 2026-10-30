// main.rs
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

// no console window on Windows (not a target, kept for completeness)
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    braids_lib::run()
}
