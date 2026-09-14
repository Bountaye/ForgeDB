#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace forgedb {
class File {
public:
    enum class Mode { append, truncate, lock };
    File(const std::filesystem::path& path, Mode mode);
    ~File();
    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    void append(std::string_view bytes);
    std::string read(std::uint64_t offset, std::size_t length);
    std::uint64_t size() const;
    void truncate(std::uint64_t length);
    void sync();
    static void replace(const std::filesystem::path& from, const std::filesystem::path& to);
    static void sync_directory(const std::filesystem::path& path);
    void close() noexcept;
private:
    std::intptr_t handle_ = -1;
    void seek(std::uint64_t offset);
};
}
