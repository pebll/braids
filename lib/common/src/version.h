// version.h
//
// Copyright (c) 2022-2026 Kristofer Berggren
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#pragma once

// braids' version (macro name kept from nchat). Bumped in one place, the
// app's app/src-tauri/tauri.conf.json: lib/ncutil/CMakeLists.txt reads it and
// passes it as BRAIDS_APP_VERSION (it was a number here, and fell behind).
#ifdef BRAIDS_APP_VERSION
#define NCHAT_VERSION BRAIDS_APP_VERSION
#else
#define NCHAT_VERSION "unknown"
#endif
