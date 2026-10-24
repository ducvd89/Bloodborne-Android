#include "proc.h"
#include <cerrno>
#include <cstring>

#include <atomic>
#include <thread>
#include <chrono>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {
struct LineSplitter {
  std::string cur; bool lastCr = false;
  const std::function<void(const std::string&)>& out;
  explicit LineSplitter(const std::function<void(const std::string&)>& o) : out(o) {}
  void feed(const char* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      char c = p[i];
      if (c == '\n') { if (!lastCr) { out(cur); } cur.clear(); lastCr = false; }
      else if (c == '\r') { out(cur); cur.clear(); lastCr = true; }
      else { cur.push_back(c); lastCr = false; }
    }
  }
  void flush() { if (!cur.empty()) out(cur); cur.clear(); }
};
}  // namespace

#ifdef _WIN32
namespace {
std::wstring wide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}
// CommandLineToArgvW's rules in reverse.
std::wstring quote(const std::string& a) {
  std::wstring w = wide(a), r = L"\"";
  size_t slashes = 0;
  for (wchar_t c : w) {
    if (c == L'\\') { ++slashes; continue; }
    if (c == L'"') { r.append(slashes * 2 + 1, L'\\'); r += L'"'; }
    else { r.append(slashes, L'\\'); r += c; }
    slashes = 0;
  }
  r.append(slashes * 2, L'\\');
  return r + L"\"";
}
}  // namespace

int runProcess(const std::vector<std::string>& argv, const std::function<void(const std::string&)>& onLine,
               const std::function<bool()>& cancel, std::string& err, bool lowPriority) {
  SECURITY_ATTRIBUTES sa { sizeof sa, nullptr, TRUE };
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) { err = "cannot create a pipe"; return -1; }
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  std::wstring cmd;
  for (auto& a : argv) { if (!cmd.empty()) cmd += L' '; cmd += quote(a); }
  STARTUPINFOW si {}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi {};
  DWORD flags = CREATE_NO_WINDOW | (lowPriority ? BELOW_NORMAL_PRIORITY_CLASS : 0);
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr, &si, &pi)) {
    err = "cannot start " + argv[0] + " (error " + std::to_string(GetLastError()) + ")";
    CloseHandle(rd); CloseHandle(wr); return -1;
  }
  CloseHandle(wr);
  std::atomic<bool> done {false}, killed {false};
  std::thread watch([&] {
    while (!done) {
      if (cancel && cancel()) { killed = true; TerminateProcess(pi.hProcess, 1); return; }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });
  LineSplitter split(onLine);
  char buf[4096]; DWORD n;
  while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) split.feed(buf, n);
  split.flush();
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
  done = true; watch.join();
  CloseHandle(rd); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
  return killed ? -1 : (int)code;
}
#else
int runProcess(const std::vector<std::string>& argv, const std::function<void(const std::string&)>& onLine,
               const std::function<bool()>& cancel, std::string& err, bool lowPriority) {
  int fds[2];
  if (pipe(fds) != 0) { err = "cannot create a pipe"; return -1; }
  posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
  posix_spawn_file_actions_addclose(&fa, fds[0]);
  posix_spawn_file_actions_addclose(&fa, fds[1]);
  std::vector<char*> av;
  for (auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
  av.push_back(nullptr);
  pid_t pid;
  int rc = posix_spawn(&pid, argv[0].c_str(), &fa, nullptr, av.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  close(fds[1]);
  if (rc != 0) { err = "cannot start " + argv[0] + ": " + strerror(rc); close(fds[0]); return -1; }
  if (lowPriority) setpriority(PRIO_PROCESS, pid, 19);
  std::atomic<bool> done {false}, killed {false};
  std::thread watch([&] {
    while (!done) {
      if (cancel && cancel()) { killed = true; kill(pid, SIGTERM); std::this_thread::sleep_for(std::chrono::seconds(3)); if (!done) kill(pid, SIGKILL); return; }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });
  LineSplitter split(onLine);
  char buf[4096]; ssize_t n;
  while ((n = read(fds[0], buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR)) if (n > 0) split.feed(buf, (size_t)n);
  split.flush();
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  done = true; watch.join();
  if (killed) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
#endif
