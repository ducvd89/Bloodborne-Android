#include "variants.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
bool hexBytes(const std::string& s, std::vector<uint8_t>& out) {
  if (s.size() % 2) return false;
  for (size_t i = 0; i < s.size(); i += 2) {
    char b[3] = {s[i], s[i + 1], 0};
    char* e; const unsigned long v = strtoul(b, &e, 16);
    if (*e) return false;
    out.push_back((uint8_t)v);
  }
  return true;
}
}

bool parseCatalog(const std::string& text, const std::string& name, Catalog& out, std::string& err) {
  std::istringstream in(text);
  std::string line;
  unsigned ln = 0;
  VarGroup* cur = nullptr;
  auto fail = [&](const char* why) { err = name + ":" + std::to_string(ln) + ": " + why + ": " + line; return false; };
  while (std::getline(in, line)) {
    ++ln;
    const size_t h = line.find('#');
    std::string l = h == std::string::npos ? line : line.substr(0, h);
    std::istringstream ls(l);
    std::string cmd;
    if (!(ls >> cmd)) continue;
    if (cmd == "group") {
      std::string n, opt;
      if (!(ls >> n)) return fail("group without a name");
      out.groups.push_back({n, "", {}});
      cur = &out.groups.back();
      while (ls >> opt) {
        if (opt.rfind("excl=", 0) == 0) cur->excl = opt.substr(5);
        else return fail("unknown group option");
      }
    } else if (cmd == "w" || cmd == "c") {
      std::string a, b;
      if (!cur) return fail("write outside a group");
      if (!(ls >> a >> b)) return fail("write needs an address and bytes");
      VarWrite w; w.code = cmd == "c";
      char* e; w.vaddr = strtoull(a.c_str(), &e, 16);
      if (*e || !hexBytes(b, w.bytes) || w.bytes.empty()) return fail("bad address or bytes");
      cur->writes.push_back(std::move(w));
    } else if (cmd == "hook") {
      std::string a, kind, opt;
      unsigned len = 0;
      if (!(ls >> a >> len >> kind)) return fail("hook needs an address, a length and a kind");
      HookDecl d;
      char* e; d.vaddr = strtoull(a.c_str(), &e, 16);
      if (*e || !len) return fail("bad hook address or length");
      d.len = len;
      if (kind == "regs") d.regs = true; else if (kind != "snap") return fail("hook kind must be snap or regs");
      while (ls >> opt) {
        if (opt == "skip") d.skip = true;
        else if (opt.rfind("resume=", 0) == 0) {
          std::istringstream rs(opt.substr(7));
          std::string t;
          while (std::getline(rs, t, ',')) { if (t.empty()) continue; d.resume.push_back(strtoull(t.c_str(), nullptr, 16)); }
        } else return fail("unknown hook option");
      }
      out.hooks.push_back(std::move(d));
    } else {
      return fail("unknown line");
    }
  }
  return true;
}

bool loadCatalogs(const std::vector<std::string>& paths, Catalog& out, std::string& err) {
  namespace fs = std::filesystem;
  std::vector<std::string> files;
  for (auto& p : paths) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
      std::vector<std::string> in;
      for (auto& e : fs::directory_iterator(p)) if (e.path().extension() == ".txt") in.push_back(e.path().string());
      std::sort(in.begin(), in.end());
      files.insert(files.end(), in.begin(), in.end());
    } else files.push_back(p);
  }
  for (auto& f : files) {
    std::ifstream in(f);
    if (!in) { err = "cannot read " + f; return false; }
    std::stringstream ss; ss << in.rdbuf();
    if (!parseCatalog(ss.str(), f, out, err)) return false;
  }
  // Merge hook declarations of one address (several files may declare the same site): the union of the options.
  std::sort(out.hooks.begin(), out.hooks.end(), [](const HookDecl& a, const HookDecl& b) { return a.vaddr < b.vaddr; });
  std::vector<HookDecl> merged;
  for (auto& h : out.hooks) {
    if (!merged.empty() && merged.back().vaddr == h.vaddr) {
      HookDecl& m = merged.back();
      if (m.regs != h.regs) { err = "hook at " + std::to_string(h.vaddr) + " declared as both snap and regs"; return false; }
      m.len = std::max(m.len, h.len); m.skip |= h.skip;
      m.resume.insert(m.resume.end(), h.resume.begin(), h.resume.end());
    } else merged.push_back(h);
  }
  for (auto& h : merged) { std::sort(h.resume.begin(), h.resume.end()); h.resume.erase(std::unique(h.resume.begin(), h.resume.end()), h.resume.end()); }
  out.hooks = std::move(merged);
  return true;
}
