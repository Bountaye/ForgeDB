#include "forgedb/file.hpp"
#include <algorithm>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace forgedb {
namespace {
[[noreturn]] void file_error(const char* action) {
#ifdef _WIN32
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), action);
#else
    throw std::system_error(errno, std::generic_category(), action);
#endif
}
}
File::File(const std::filesystem::path& path, Mode mode) {
#ifdef _WIN32
    const auto h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        mode == Mode::lock ? 0 : FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, mode == Mode::truncate ? CREATE_ALWAYS : OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) file_error("open persistence file");
    handle_ = reinterpret_cast<std::intptr_t>(h);
#else
    handle_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC |
        (mode == Mode::truncate ? O_TRUNC : 0), 0600);
    if (handle_ < 0) file_error("open persistence file");
    if (mode == Mode::lock && flock(static_cast<int>(handle_), LOCK_EX | LOCK_NB) != 0) {
        const int saved = errno; close(); errno = saved; file_error("data directory already locked");
    }
#endif
}
File::~File() { close(); }
File::File(File&& o) noexcept : handle_(std::exchange(o.handle_, -1)) {}
File& File::operator=(File&& o) noexcept {
    if (this != &o) { close(); handle_ = std::exchange(o.handle_, -1); } return *this;
}
void File::close() noexcept {
    if (handle_ == -1) return;
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
    ::close(static_cast<int>(handle_));
#endif
    handle_ = -1;
}
std::uint64_t File::size() const {
#ifdef _WIN32
    LARGE_INTEGER n{};
    if (!GetFileSizeEx(reinterpret_cast<HANDLE>(handle_), &n)) file_error("file size");
    return static_cast<std::uint64_t>(n.QuadPart);
#else
    struct stat s{};
    if (fstat(static_cast<int>(handle_), &s) != 0) file_error("file size");
    return static_cast<std::uint64_t>(s.st_size);
#endif
}
void File::seek(std::uint64_t offset) {
#ifdef _WIN32
    LARGE_INTEGER n{}; n.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(reinterpret_cast<HANDLE>(handle_), n, nullptr, FILE_BEGIN)) file_error("seek");
#else
    if (lseek(static_cast<int>(handle_), static_cast<off_t>(offset), SEEK_SET) < 0) file_error("seek");
#endif
}
void File::append(std::string_view bytes) {
    seek(size());
    while (!bytes.empty()) {
        const auto n = std::min<std::size_t>(bytes.size(), 1024 * 1024);
#ifdef _WIN32
        DWORD done = 0;
        if (!WriteFile(reinterpret_cast<HANDLE>(handle_), bytes.data(), static_cast<DWORD>(n), &done, nullptr)) file_error("write AOF");
#else
        const auto done = ::write(static_cast<int>(handle_), bytes.data(), n);
        if (done < 0) { if (errno == EINTR) continue; file_error("write AOF"); }
#endif
        if (done == 0) throw std::runtime_error("zero-length disk write");
        bytes.remove_prefix(static_cast<std::size_t>(done));
    }
}
std::string File::read(std::uint64_t offset, std::size_t length) {
    seek(offset);
    std::string result(length, '\0');
    std::size_t pos = 0;
    while (pos < length) {
        const auto n = std::min<std::size_t>(length - pos, 1024 * 1024);
#ifdef _WIN32
        DWORD done = 0;
        if (!ReadFile(reinterpret_cast<HANDLE>(handle_), result.data() + pos, static_cast<DWORD>(n), &done, nullptr)) file_error("read AOF");
#else
        const auto done = ::read(static_cast<int>(handle_), result.data() + pos, n);
        if (done < 0) { if (errno == EINTR) continue; file_error("read AOF"); }
#endif
        if (done == 0) break;
        pos += static_cast<std::size_t>(done);
    }
    result.resize(pos); return result;
}
void File::truncate(std::uint64_t length) {
#ifdef _WIN32
    seek(length);
    if (!SetEndOfFile(reinterpret_cast<HANDLE>(handle_))) file_error("truncate torn AOF tail");
#else
    if (ftruncate(static_cast<int>(handle_), static_cast<off_t>(length)) != 0) file_error("truncate torn AOF tail");
#endif
}
void File::sync() {
#ifdef _WIN32
    if (!FlushFileBuffers(reinterpret_cast<HANDLE>(handle_))) file_error("flush AOF");
#else
    if (fsync(static_cast<int>(handle_)) != 0) file_error("fsync AOF");
#endif
}
void File::replace(const std::filesystem::path& from, const std::filesystem::path& to) {
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) file_error("replace AOF");
#else
    if (::rename(from.c_str(), to.c_str()) != 0) file_error("replace AOF");
#endif
}
void File::sync_directory(const std::filesystem::path& path) {
#ifdef _WIN32
    (void)path; // MoveFileExW WRITE_THROUGH; Windows has no portable directory fsync.
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) file_error("open data directory");
    const int result = fsync(fd); const int saved = errno; ::close(fd);
    if (result != 0) { errno = saved; file_error("fsync data directory"); }
#endif
}
}
