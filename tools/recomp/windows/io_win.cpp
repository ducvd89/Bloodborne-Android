// tools/android/prepare/io.h on Windows (instead of io.cpp: that one uses mmap, open/O_CLOEXEC, dirent, realpath). The same interface
// and the same errors (as Python's OSError family), with Win32 file mapping and std::filesystem. Only the file access is here: the
// image-building logic (ps4.cpp, link.cpp, game_check.cpp, bb_prepare.cpp ...) is the phone's own source, compiled unchanged.
#include "io.h"

#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <cerrno>
#include <filesystem>

namespace fs = std::filesystem;

namespace {
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
int errnoOf(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_NAME: return ENOENT;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: return EACCES;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: return EEXIST;
    case ERROR_DIRECTORY: return ENOTDIR;
    default: return EIO;
    }
}
}  // namespace

MappedFile& MappedFile::operator=(MappedFile&& o) noexcept {
    close();
    data_ = o.data_; size_ = o.size_; map_ = o.map_; map_len_ = o.map_len_;
    fallback_ = std::move(o.fallback_);
    if (!fallback_.empty()) data_ = fallback_.data();
    o.data_ = nullptr; o.size_ = 0; o.map_ = nullptr; o.map_len_ = 0;
    return *this;
}

void MappedFile::close() {
    if (map_) UnmapViewOfFile(map_);
    map_ = nullptr;
    map_len_ = 0;
    data_ = nullptr;
    size_ = 0;
    Bytes().swap(fallback_);
}

MappedFile read_file(const std::string& path) {
    std::wstring w = wide(path);
    DWORD attrs = GetFileAttributesW(w.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) raise_errno(errnoOf(GetLastError()), path);
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) raise_errno(EISDIR, path);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) raise_errno(errnoOf(GetLastError()), path);
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size)) { int e = errnoOf(GetLastError()); CloseHandle(h); raise_errno(e, path); }
    MappedFile f;
    if (size.QuadPart > 0) {
        HANDLE m = CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
        void* p = m ? MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0) : nullptr;
        if (m) CloseHandle(m);  // the view keeps the mapping alive
        if (p) {
            CloseHandle(h);
            f.map_ = p;
            f.map_len_ = f.size_ = (size_t)size.QuadPart;
            f.data_ = (const u8*)p;
            return f;
        }
    }
    Bytes buf;
    u8 chunk[1 << 16];
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(h, chunk, sizeof chunk, &n, nullptr)) { int e = errnoOf(GetLastError()); CloseHandle(h); raise_errno(e, path); }
        if (!n) break;
        buf.insert(buf.end(), chunk, chunk + n);
    }
    CloseHandle(h);
    f.fallback_ = std::move(buf);
    f.data_ = f.fallback_.data();
    f.size_ = f.fallback_.size();
    return f;
}

OutFile::OutFile(const std::string& path) : path_(path) {
    fd_ = _wopen(wide(path).c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (fd_ < 0) raise_errno(errno, path);
}
void OutFile::write(const void* p, size_t n) {
    const char* c = (const char*)p;
    while (n) {
        int w = _write(fd_, c, n > (1u << 30) ? (1u << 30) : (unsigned)n);
        if (w < 0) raise_errno(errno, path_);
        c += w;
        n -= (size_t)w;
    }
}
void OutFile::close_fd() {
    if (fd_ >= 0) _close(fd_);
    fd_ = -1;
}
void OutFile::close() {
    int fd = fd_;
    fd_ = -1;
    if (fd >= 0 && _close(fd) < 0) raise_errno(errno, path_);
}

bool is_file(const std::string& path) {
    std::error_code ec;
    return fs::is_regular_file(fs::path(wide(path)), ec);
}

void make_dir(const std::string& path) {
    if (!CreateDirectoryW(wide(path).c_str(), nullptr)) raise_errno(errnoOf(GetLastError()), path);
}

void make_dirs(const std::string& path) {
    std::error_code ec;
    fs::path p(wide(path));
    if (fs::is_directory(p, ec)) return;
    if (fs::exists(p, ec)) raise_errno(EEXIST, path);
    fs::create_directories(p, ec);
    if (ec && !fs::is_directory(p)) raise_errno(EACCES, path);
}

void rmtree_ignore_errors(const std::string& path) {
    std::error_code ec;
    fs::remove_all(fs::path(wide(path)), ec);
}

void write_text_file(const std::string& path, const std::string& text) {
    OutFile f(path);
    f.write(text);
    f.close();
}

std::string resolve_path(const std::string& path) {
    std::error_code ec;
    fs::path p = fs::absolute(fs::path(wide(path)), ec).lexically_normal();
    std::wstring w = p.generic_wstring();
    if (w.empty()) return path_norm(path);
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    while (s.size() > 3 && s.back() == '/') s.pop_back();
    return s;
}
