#pragma once

#include "common.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace q2dz {

struct ContentResult {
    bool found = false;
    std::string source;
    std::vector<std::string> searched;
};

Result<ContentResult> find_map_content(const std::filesystem::path& q2_root,
                                       std::string_view game,
                                       std::string_view map);
bool safe_qpath_component(std::string_view value);

} // namespace q2dz
