// SPDX-License-Identifier: GPL-3.0-or-later
// The Bloodborne Android Rebuild Tool's window (Dear ImGui on SDL3): two folders, Build, a progress bar and a log. It does not do the work
// itself: it starts this same program as `bb-recomp --pkgs <dir> --out <dir> --machine` and follows the "@@" lines it prints (player.cpp),
// so Cancel simply stops that process, and a crash in the build cannot take the window down.
#include "gui.h"

#include "bundle.h"
#include "proc.h"

#include <SDL3/SDL.h>
#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

extern const char* const kNoticesText;  // notices_data.cpp, generated from NOTICES

namespace {
namespace fs = std::filesystem;

constexpr const char* kTitle = "Bloodborne Android Rebuild Tool";
constexpr const char* kRepo = "https://github.com/ducvd89/Bloodborne-android-rebuild-tool";
constexpr const char* kDocs = "https://github.com/ducvd89/Bloodborne-android-rebuild-tool#readme";

// The stages the build reports (player.cpp) with their share of the whole bar.
struct StageInfo { const char* id; const char* label; float weight; };
const StageInfo kStages[] = {
  {"check", "Checking the PKG files", 0.01f},      {"extract", "Extracting the game", 0.50f},   {"merge", "Merging the update", 0.05f},
  {"prepare", "Preparing the game image", 0.03f},  {"analyse", "Analysing the game's code", 0.07f}, {"compile", "Recompiling", 0.25f},
  {"link", "Linking", 0.03f},                      {"verify", "Checking the result", 0.02f},    {"finish", "Copying the result", 0.04f},
};

struct State {
  std::mutex mu;
  std::deque<std::string> log;
  std::string stageId, stageText;
  uint64_t done = 0, total = 0;
  bool running = false, finished = false, failed = false, cancelled = false;
  std::string error, resultFolder;
  std::chrono::steady_clock::time_point started;
  std::atomic<bool> cancel {false};
  std::thread worker;
  bool logDirty = false;
} g;

char g_pkgs[2048] = "", g_out[2048] = "";
std::mutex g_pickMu;
std::string g_picked[2];  // a folder chosen in the system dialog, handed to the UI thread
bool g_pickedSet[2] = {false, false};
bool g_aboutOpen = false;

void addLog(const std::string& line) {
  std::lock_guard<std::mutex> l(g.mu);
  g.log.push_back(line);
  if (g.log.size() > 5000) g.log.pop_front();
  g.logDirty = true;
}

std::string selfPath() {
  const std::string d = bundleExeDir();
#ifdef _WIN32
  char b[4096]; DWORD n = GetModuleFileNameA(nullptr, b, sizeof b);
  return std::string(b, n);
#else
  char b[4096]; ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1);
  return n > 0 ? std::string(b, (size_t)n) : d + "/bb-recomp";
#endif
}

void onLine(const std::string& line) {
  if (line.rfind("@@STAGE ", 0) == 0) {
    std::string rest = line.substr(8); size_t bar = rest.find('|');
    std::lock_guard<std::mutex> l(g.mu);
    g.stageId = rest.substr(0, bar); g.stageText = bar == std::string::npos ? "" : rest.substr(bar + 1); g.done = g.total = 0;
    return;
  }
  if (line.rfind("@@PROGRESS ", 0) == 0) {
    unsigned long long d = 0, t = 0;
    if (sscanf(line.c_str() + 11, "%llu %llu", &d, &t) == 2) { std::lock_guard<std::mutex> l(g.mu); g.done = std::max<uint64_t>(g.done, d); g.total = t; }
    return;
  }
  if (line.rfind("@@ERROR ", 0) == 0) { std::lock_guard<std::mutex> l(g.mu); g.error = line.substr(8); return; }
  if (line.rfind("@@DONE ", 0) == 0) { std::lock_guard<std::mutex> l(g.mu); g.resultFolder = line.substr(7); return; }
  if (!line.empty()) addLog(line);
}

void startBuild() {
  if (g.worker.joinable()) g.worker.join();
  {
    std::lock_guard<std::mutex> l(g.mu);
    g.log.clear(); g.stageId = "check"; g.stageText = "Starting…"; g.done = g.total = 0; g.error.clear(); g.resultFolder.clear();
    g.running = true; g.finished = g.failed = g.cancelled = false; g.cancel = false; g.started = std::chrono::steady_clock::now();
  }
  const std::string pkgs = g_pkgs, out = g_out;
  g.worker = std::thread([pkgs, out] {
    std::string err;
    int code = runProcess({selfPath(), "--pkgs", pkgs, "--out", out, "--machine"}, onLine, [] { return g.cancel.load(); }, err, true);
    std::lock_guard<std::mutex> l(g.mu);
    g.running = false; g.finished = true;
    if (g.cancel) { g.cancelled = true; g.failed = true; }
    else if (code != 0) { g.failed = true; if (g.error.empty()) g.error = err.empty() ? "The build stopped with code " + std::to_string(code) + ". See the log." : err; }
  });
}

void folderCallback(void* user, const char* const* list, int) {
  if (!list || !list[0]) return;
  int which = (int)(intptr_t)user;
  std::lock_guard<std::mutex> l(g_pickMu);
  g_picked[which] = list[0]; g_pickedSet[which] = true;
}

float overallFraction(const std::string& stage, uint64_t done, uint64_t total) {
  float acc = 0, sum = 0;
  for (auto& s : kStages) sum += s.weight;
  for (auto& s : kStages) {
    if (stage == s.id) { acc += s.weight * (total ? (float)done / (float)total : 0.f); break; }
    acc += s.weight;
  }
  return std::min(1.f, acc / sum);
}

std::string stageLabel(const std::string& id, const std::string& text, uint64_t done, uint64_t total) {
  char b[256];
  if (id == "extract" && total) { snprintf(b, sizeof b, "Extracting: %.1f of %.1f GB", done / 1073741824.0, total / 1073741824.0); return b; }
  if (id == "compile" && total) { snprintf(b, sizeof b, "Recompiling: %" PRIu64 " / %" PRIu64 " shards", std::min(done + 1, total), total); return b; }
  if (id == "compile") return "Recompiling";
  for (auto& s : kStages) if (id == s.id) return text.empty() ? s.label : (id == "extract" || id == "merge" ? text : s.label);
  return text;
}

void theme() {
  ImGuiStyle& st = ImGui::GetStyle();
  ImGui::StyleColorsDark();
  st.WindowRounding = 0; st.FrameRounding = 4; st.GrabRounding = 4; st.ScrollbarRounding = 4; st.FramePadding = ImVec2(10, 6); st.ItemSpacing = ImVec2(10, 10);
  ImVec4* c = st.Colors;
  const ImVec4 gold(0.776f, 0.678f, 0.463f, 1.f);  // 0xC6AD76, the app's accent
  c[ImGuiCol_WindowBg] = ImVec4(0.031f, 0.035f, 0.047f, 1.f);
  c[ImGuiCol_ChildBg] = ImVec4(0.055f, 0.059f, 0.075f, 1.f);
  c[ImGuiCol_FrameBg] = ImVec4(0.09f, 0.095f, 0.115f, 1.f);
  c[ImGuiCol_Text] = ImVec4(0.91f, 0.875f, 0.79f, 1.f);
  c[ImGuiCol_Button] = ImVec4(0.17f, 0.15f, 0.11f, 1.f);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.26f, 0.17f, 1.f);
  c[ImGuiCol_ButtonActive] = ImVec4(0.40f, 0.34f, 0.21f, 1.f);
  c[ImGuiCol_PlotHistogram] = gold;
  c[ImGuiCol_CheckMark] = gold;
  c[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.1f, 0.07f, 1.f);
  c[ImGuiCol_Header] = ImVec4(0.2f, 0.17f, 0.11f, 1.f);
  c[ImGuiCol_Separator] = ImVec4(0.776f, 0.678f, 0.463f, 0.35f);
}

void aboutWindow() {
  if (g_aboutOpen) ImGui::OpenPopup("About");
  ImVec2 center = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(ImGui::GetMainViewport()->Size.x * 0.86f, ImGui::GetMainViewport()->Size.y * 0.84f), ImGuiCond_Appearing);
  if (ImGui::BeginPopupModal("About", &g_aboutOpen, ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::TextColored(ImVec4(0.776f, 0.678f, 0.463f, 1.f), "%s", kTitle);
    ImGui::TextWrapped("Version %s, (c) ducvd89, GPL-3.0-or-later.", BBR_VERSION_TEXT);
    ImGui::TextWrapped("This is free software: if you paid for this, you were scammed.");
    ImGui::TextWrapped("Source code and downloads:");
    ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.f, 1.f), "%s", kRepo);
    if (ImGui::Button("Open the page")) SDL_OpenURL(kRepo);
    ImGui::SameLine();
    if (ImGui::Button("Copy the link")) SDL_SetClipboardText(kRepo);
    ImGui::Separator();
    ImGui::TextUnformatted("Third-party software in this tool:");
    ImGui::BeginChild("notices", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.4f), ImGuiChildFlags_Borders);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(kNoticesText);
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    if (ImGui::Button("Close")) { g_aboutOpen = false; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
  }
}

// One path row: a label, the editable path and a Browse button (the system folder dialog; typing a path works where none exists).
void pathRow(const char* label, const char* id, char* buf, size_t size, int which, SDL_Window* win, bool enabled) {
  ImGui::TextUnformatted(label);
  ImGui::BeginDisabled(!enabled);
  float button = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2 + 8;
  ImGui::SetNextItemWidth(-button - ImGui::GetStyle().ItemSpacing.x);
  ImGui::InputText(id, buf, size);
  ImGui::SameLine();
  std::string bid = std::string("Browse...##") + id;
  if (ImGui::Button(bid.c_str())) SDL_ShowOpenFolderDialog(folderCallback, (void*)(intptr_t)which, win, buf[0] ? buf : nullptr, false);
  ImGui::EndDisabled();
}

std::string trimmed(const char* s) {
  std::string t = s;
  while (!t.empty() && (t.back() == ' ' || t.back() == '\n' || t.back() == '\r' || t.back() == '"')) t.pop_back();
  size_t a = 0; while (a < t.size() && (t[a] == ' ' || t[a] == '"')) ++a;
  return t.substr(a);
}

void frame(SDL_Window* win) {
  ImGuiIO& io = ImGui::GetIO();
  {
    std::lock_guard<std::mutex> l(g_pickMu);
    if (g_pickedSet[0]) { snprintf(g_pkgs, sizeof g_pkgs, "%s", g_picked[0].c_str()); g_pickedSet[0] = false; }
    if (g_pickedSet[1]) { snprintf(g_out, sizeof g_out, "%s", g_picked[1].c_str()); g_pickedSet[1] = false; }
  }
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::Begin("main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

  ImGui::TextColored(ImVec4(0.776f, 0.678f, 0.463f, 1.f), "%s", kTitle);
  ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize("About").x - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().WindowPadding.x);
  if (ImGui::Button("About")) g_aboutOpen = true;
  ImGui::TextWrapped("Builds the folder to copy to your phone: your Bloodborne game (unpacked from your PKG files) with the game's code compiled for the native CPU.");
  ImGui::Separator();

  bool running, finished, failed, cancelled;
  std::string stageId, stageText, error, result; uint64_t done, total; std::vector<std::string> lines; bool follow;
  std::chrono::steady_clock::time_point started;
  {
    std::lock_guard<std::mutex> l(g.mu);
    running = g.running; finished = g.finished; failed = g.failed; cancelled = g.cancelled; stageId = g.stageId; stageText = g.stageText;
    done = g.done; total = g.total; error = g.error; result = g.resultFolder; started = g.started;
    lines.assign(g.log.end() - std::min<size_t>(g.log.size(), 1200), g.log.end()); follow = g.logDirty; g.logDirty = false;
  }

  pathRow("Game PKG folder  (the base game PKG and the 1.09 update PKG)", "##pkgs", g_pkgs, sizeof g_pkgs, 0, win, !running);
  pathRow("Output folder  (everything the phone needs goes into it)", "##out", g_out, sizeof g_out, 1, win, !running);

  const bool ready = !running && trimmed(g_pkgs).size() > 0 && trimmed(g_out).size() > 0;
  ImGui::BeginDisabled(!ready);
  if (ImGui::Button("  Build  ")) {
    std::string p = trimmed(g_pkgs), o = trimmed(g_out);
    snprintf(g_pkgs, sizeof g_pkgs, "%s", p.c_str()); snprintf(g_out, sizeof g_out, "%s", o.c_str());
    startBuild();
  }
  ImGui::EndDisabled();
  if (running) { ImGui::SameLine(); if (ImGui::Button("  Cancel  ")) g.cancel = true; }

  if (running || finished) {
    float frac = finished && !failed ? 1.f : overallFraction(stageId, done, total);
    std::string label = finished ? (failed ? (cancelled ? "Cancelled" : "Failed") : "Done") : stageLabel(stageId, stageText, done, total);
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
    char right[64]; snprintf(right, sizeof right, "%d:%02d", (int)(secs / 60), (int)(secs % 60));
    ImGui::TextUnformatted(label.c_str());
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(right).x - ImGui::GetStyle().WindowPadding.x);
    ImGui::TextUnformatted(right);
    char pct[16]; snprintf(pct, sizeof pct, "%d %%", (int)(frac * 100));
    ImGui::ProgressBar(frac, ImVec2(-1, ImGui::GetFrameHeight() * 1.2f), pct);
  }
  if (finished && !failed && !result.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.56f, 0.75f, 0.50f, 1.f));
    ImGui::TextWrapped("Done: copy the folder\n    %s\nto your phone and choose it as the game folder in the app (Menu > Game folder). The app imports the native CPU files in it by itself.", result.c_str());
    ImGui::PopStyleColor();
    if (ImGui::Button("Open the folder")) { std::string url = std::string("file://") + (result.size() > 1 && result[1] == (char)58 ? "/" : "") + result; SDL_OpenURL(url.c_str()); }
  } else if (finished && failed) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.46f, 0.36f, 1.f));
    ImGui::TextWrapped("%s", cancelled ? "The build was cancelled. Nothing in your PKG folder was changed." : error.c_str());
    ImGui::PopStyleColor();
  }

  ImGui::TextUnformatted("Log");
  ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
  for (auto& l : lines) ImGui::TextUnformatted(l.c_str());
  if (follow || running) ImGui::SetScrollHereY(1.f);
  ImGui::EndChild();
  ImGui::End();
  aboutWindow();
}
}  // namespace

int guiMain(int argc, char** argv) {
#ifdef _WIN32
  // Started by a double click the program has a console of its own: hide it. From a terminal it stays.
  { DWORD pid; if (GetConsoleProcessList(&pid, 1) == 1) FreeConsole(); }
#endif
  std::string shot;
  bool autobuild = false, midShot = false;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--gui-selftest") && i + 1 < argc) shot = argv[++i];
    else if (!strcmp(argv[i], "--pkgs-default") && i + 1 < argc) snprintf(g_pkgs, sizeof g_pkgs, "%s", argv[++i]);
    else if (!strcmp(argv[i], "--out-default") && i + 1 < argc) snprintf(g_out, sizeof g_out, "%s", argv[++i]);
    else if (!strcmp(argv[i], "--about")) g_aboutOpen = true;
    else if (!strcmp(argv[i], "--gui-autobuild")) autobuild = true;  // self-test: press Build, screenshot mid-build (<shot>.mid.bmp) and at the end
  }
  if (g_out[0] == 0) {
    const char* home = getenv("HOME"); if (!home) home = getenv("USERPROFILE");
#ifdef _WIN32
    if (home) snprintf(g_out, sizeof g_out, "%s\\Bloodborne-for-phone", home);
#else
    if (home) snprintf(g_out, sizeof g_out, "%s/Bloodborne-for-phone", home);
#endif
  }
  SDL_SetHint(SDL_HINT_APP_NAME, kTitle);
  if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
  float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
  if (!(scale > 0.5f)) scale = 1.f;
  SDL_Window* win = SDL_CreateWindow(kTitle, (int)(980 * scale), (int)(720 * scale), SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!win) { fprintf(stderr, "SDL window: %s\n", SDL_GetError()); return 1; }
  SDL_Renderer* ren = SDL_CreateRenderer(win, nullptr);
  if (!ren) { fprintf(stderr, "SDL renderer: %s\n", SDL_GetError()); return 1; }
  SDL_SetRenderVSync(ren, 1);
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  theme();
  ImGuiStyle& st = ImGui::GetStyle();
  st.FontSizeBase = 17.f; st.FontScaleDpi = scale;
  st.ScaleAllSizes(scale);
  ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
  ImGui_ImplSDLRenderer3_Init(ren);

  bool quit = false; int frames = 0;
  while (!quit) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      ImGui_ImplSDL3_ProcessEvent(&e);
      if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(win))) quit = true;
    }
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    frame(win);
    ImGui::Render();
    SDL_SetRenderScale(ren, ImGui::GetIO().DisplayFramebufferScale.x, ImGui::GetIO().DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(ren, 8, 9, 12, 255);
    SDL_RenderClear(ren);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
    ++frames;
    if (autobuild && frames == 3) startBuild();
    bool fin, mid;
    { std::lock_guard<std::mutex> l(g.mu); fin = g.finished; mid = g.running && g.stageId == "compile" && g.done >= 12; }
    if (!shot.empty() && autobuild && mid && !midShot) {
      midShot = true;
      SDL_Surface* s = SDL_RenderReadPixels(ren, nullptr);
      if (s) { SDL_SaveBMP(s, (shot + ".mid.bmp").c_str()); SDL_DestroySurface(s); }
    }
    static int finFrames = 0;
    if (!shot.empty() && ((!autobuild && frames == 8) || (autobuild && fin && ++finFrames == 5))) {  // --gui-selftest: a screenshot, then leave
      SDL_Surface* s = SDL_RenderReadPixels(ren, nullptr);
      if (s) { SDL_SaveBMP(s, shot.c_str()); SDL_DestroySurface(s); }
      quit = true;
    }
    SDL_RenderPresent(ren);
    if (g.running) SDL_Delay(1); else SDL_Delay(8);
  }
  g.cancel = true;
  if (g.worker.joinable()) g.worker.join();
  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
  return 0;
}
