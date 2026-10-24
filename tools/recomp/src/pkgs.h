// SPDX-License-Identifier: GPL-3.0-or-later
// The game as PKG files: find and identify the base game and its update(s) in a folder, unpack them with ps4-pkg-tool (the tool the app's
// PkgInstaller runs, built by tools/android/build_pkg_tool.sh from its patched shadPS4 source) and merge the updates over the base into
// one game folder, as PkgInstaller.installGame does (the same checks and messages, the same merge: rename over, case-insensitive names,
// eboot.bin and sce_sys last). Only fake (unencrypted) PKGs can be unpacked, like on the phone.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct PkgInfo {
  std::string path, file, contentId, titleId, category, appVer;
  uint64_t size = 0;
};
struct PkgSet {
  std::string titleId;
  PkgInfo base;                  // category gd
  std::vector<PkgInfo> updates;  // category gp, oldest first
};

// Reads the header and param.sfo of a PKG (PkgInstaller.readPkg).
bool readPkgHeader(const std::string& path, PkgInfo& info, std::string& err);
// Finds the *.pkg files of a folder and sorts them out. false: `err` says what is wrong, for the player.
bool findPkgs(const std::string& dir, PkgSet& set, std::string& err);
// Checks every PKG of the set for damage (pkgverify.h) before anything is unpacked: false with `err` naming the file, what is wrong and what to do.
// Reports bytes and speed through progress.h (stage "pkgcheck").
bool checkPkgs(const PkgSet& set, std::string& err, const std::function<void(const std::string&)>& log, const std::function<bool()>& cancel);
// Unpacks the base into <gameParent>/<TITLE_ID> and merges the updates into it (staging in `work`). gameDir: that folder.
bool installPkgs(const PkgSet& set, const std::string& tool, const std::string& gameParent, const std::string& work, std::string& gameDir,
                 std::string& err, const std::function<void(const std::string&)>& log, const std::function<bool()>& cancel);
