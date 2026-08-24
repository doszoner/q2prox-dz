#pragma once
#include <filesystem>
#include <string_view>
namespace q2dz {
bool safe_qpath(std::string_view path) noexcept;
bool pak_contains(const std::filesystem::path& pak, std::string_view qpath);
bool content_has_map(const std::filesystem::path& root, std::string_view game, std::string_view map);
}
