#include "q2dz/session_safety.hpp"

namespace q2dz {

bool is_direct_session_child(const std::filesystem::path& canonical_root,
                             const std::filesystem::path& canonical_session) {
    return !canonical_root.empty() && !canonical_session.empty() &&
           canonical_session != canonical_root &&
           canonical_session.parent_path() == canonical_root;
}

bool is_expected_session_name(const std::filesystem::path& session) {
    const auto name = session.filename().generic_string();
    constexpr std::string_view prefix = "session-";
    return name.size() > prefix.size() && name.starts_with(prefix);
}

bool is_exact_session_marker(std::string_view marker) noexcept {
    return marker == kSessionMarker;
}

} // namespace q2dz
