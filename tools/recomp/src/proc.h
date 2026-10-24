// SPDX-License-Identifier: GPL-3.0-or-later
// Runs a child process and reads its output line by line (Linux and Windows): ps4-pkg-tool for the PKG step, and the GUI's own
// command line run (bb-recomp --pkgs ...), which it watches through the progress lines on stdout.
#pragma once
#include <functional>
#include <string>
#include <vector>

// Starts argv[0] with the arguments (no shell), stdout and stderr merged. Calls onLine for every line ('\n', '\r\n' or '\r' ends one).
// `cancel` is polled about every 100 ms: when it returns true the process is killed. Returns the exit code, or -1 when it could
// not be started or was killed (then `err` says why when it could not start).
int runProcess(const std::vector<std::string>& argv, const std::function<void(const std::string&)>& onLine,
               const std::function<bool()>& cancel, std::string& err, bool lowPriority = false);
