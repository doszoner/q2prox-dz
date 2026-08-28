#pragma once

#include <filesystem>
#include <string_view>

namespace q2dz {

inline constexpr std::string_view kSessionMarker = "q2dz-session-v1\n";

// These checks are deliberately platform-neutral. The Windows adapter adds
// canonicalization, ownership and reparse-point checks before deleting files.
[[nodiscard]] bool is_direct_session_child(const std::filesystem::path& canonical_root,
                                           const std::filesystem::path& canonical_session);
[[nodiscard]] bool is_expected_session_name(const std::filesystem::path& session);
[[nodiscard]] bool is_exact_session_marker(std::string_view marker) noexcept;

} // namespace q2dz
