#include "content.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::string root;
    std::string game = "baseq2";
    std::string map;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto take = [&](std::string& target) -> bool {
            if (i + 1 >= argc) return false;
            target = argv[++i];
            return true;
        };
        if (arg == "--q2-root") { if (!take(root)) return 64; }
        else if (arg == "--game") { if (!take(game)) return 64; }
        else if (arg == "--map") { if (!take(map)) return 64; }
        else if (arg == "--help") {
            std::cout << "Usage: q2dz-content-check --q2-root DIR --game GAME --map MAP\n";
            return 0;
        } else { std::cerr << "unknown option: " << arg << "\n"; return 64; }
    }
    if (root.empty() || map.empty()) { std::cerr << "--q2-root and --map are required\n"; return 64; }
    auto result = q2dz::find_map_content(root, game, map);
    if (!result) { std::cerr << "CONTENT_ERROR reason=" << result.error().message << "\n"; return 70; }
    if (!result.value().found) {
        std::cerr << "CONTENT_MISSING game=" << game << " map=" << map << "\n";
        return 3;
    }
    std::cout << "CONTENT_READY source=" << result.value().source << "\n";
    return 0;
}
