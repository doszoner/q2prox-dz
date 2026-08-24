#include "content.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace q2dz {

bool safe_qpath_component(std::string_view value) {
    if (value.empty() || value.size() > 64 || value == "." || value == "..") return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
}

static std::uint32_t le32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8U) |
           (static_cast<std::uint32_t>(p[2]) << 16U) |
           (static_cast<std::uint32_t>(p[3]) << 24U);
}

static Result<bool> pak_contains(const fs::path& pak, std::string_view target) {
    UniqueFd fd(::open(pak.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    if (!fd) return Result<bool>::fail(errno_message("open " + pak.string()));
    struct stat st{};
    if (::fstat(fd.get(), &st) != 0) return Result<bool>::fail(errno_message("fstat " + pak.string()));
    if (!S_ISREG(st.st_mode)) return Result<bool>::fail("PAK is not a regular file: " + pak.string());
    if (st.st_size < 12) return Result<bool>::fail("PAK is shorter than header: " + pak.string());
    std::array<unsigned char, 12> header{};
    if (::pread(fd.get(), header.data(), header.size(), 0) != static_cast<ssize_t>(header.size())) {
        return Result<bool>::fail("cannot read PAK header: " + pak.string());
    }
    if (std::memcmp(header.data(), "PACK", 4) != 0) return Result<bool>::fail("invalid PAK magic: " + pak.string());
    const std::uint32_t dir_offset = le32(header.data() + 4);
    const std::uint32_t dir_length = le32(header.data() + 8);
    constexpr std::uint32_t entry_size = 64;
    if (dir_length % entry_size != 0) return Result<bool>::fail("invalid PAK directory length: " + pak.string());
    const std::uint64_t end = static_cast<std::uint64_t>(dir_offset) + static_cast<std::uint64_t>(dir_length);
    if (end > static_cast<std::uint64_t>(st.st_size)) return Result<bool>::fail("PAK directory outside file: " + pak.string());
    if (dir_length > 64U * 1024U * 1024U) return Result<bool>::fail("PAK directory exceeds safety limit");
    std::vector<unsigned char> directory(dir_length);
    if (::pread(fd.get(), directory.data(), directory.size(), static_cast<off_t>(dir_offset)) != static_cast<ssize_t>(directory.size())) {
        return Result<bool>::fail("cannot read PAK directory: " + pak.string());
    }
    for (std::size_t offset = 0; offset < directory.size(); offset += entry_size) {
        const auto* entry = directory.data() + offset;
        std::size_t name_length = 0;
        while (name_length < 56 && entry[name_length] != 0) ++name_length;
        std::string name(reinterpret_cast<const char*>(entry), name_length);
        std::replace(name.begin(), name.end(), '\\', '/');
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool unsafe_qpath = name.find("../") != std::string::npos ||
                                  name.starts_with('/') || name.find(':') != std::string::npos;
        const std::uint32_t file_offset = le32(entry + 56);
        const std::uint32_t file_length = le32(entry + 60);
        const std::uint64_t file_end = static_cast<std::uint64_t>(file_offset) + static_cast<std::uint64_t>(file_length);
        if (file_end > static_cast<std::uint64_t>(st.st_size)) return Result<bool>::fail("PAK entry outside file: " + pak.string());
        if (unsafe_qpath) continue;
        if (name == target) return Result<bool>::ok(true);
    }
    return Result<bool>::ok(false);
}

Result<ContentResult> find_map_content(const fs::path& q2_root,
                                       std::string_view game,
                                       std::string_view map) {
    if (!safe_qpath_component(map)) return Result<ContentResult>::fail("unsafe map name");
    if (!game.empty() && !safe_qpath_component(game)) return Result<ContentResult>::fail("unsafe game directory");
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(q2_root, ec);
    if (ec || !fs::is_directory(root)) return Result<ContentResult>::fail("invalid Q2 root: " + q2_root.string());

    std::vector<std::string> games;
    if (!game.empty()) games.emplace_back(game);
    if (game != "baseq2") games.emplace_back("baseq2");
    ContentResult result;
    std::string target = "maps/" + std::string(map) + ".bsp";
    std::transform(target.begin(), target.end(), target.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    for (const auto& game_name : games) {
        const fs::path game_dir = root / game_name;
        const fs::path loose = game_dir / "maps" / (std::string(map) + ".bsp");
        result.searched.push_back(loose.string());
        if (fs::is_regular_file(loose, ec) && !ec) {
            result.found = true;
            result.source = loose.string();
            return Result<ContentResult>::ok(std::move(result));
        }
        ec.clear();
        if (!fs::is_directory(game_dir, ec) || ec) { ec.clear(); continue; }
        std::vector<fs::path> paks;
        for (const auto& entry : fs::directory_iterator(game_dir, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) { ec.clear(); continue; }
            auto ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".pak") paks.push_back(entry.path());
        }
        ec.clear();
        std::sort(paks.begin(), paks.end());
        for (const auto& pak : paks) {
            result.searched.push_back(pak.string() + ":" + target);
            auto contains = pak_contains(pak, target);
            if (!contains) return Result<ContentResult>::fail(contains.error().message);
            if (contains.value()) {
                result.found = true;
                result.source = pak.string() + ":" + target;
                return Result<ContentResult>::ok(std::move(result));
            }
        }
    }
    return Result<ContentResult>::ok(std::move(result));
}

} // namespace q2dz
