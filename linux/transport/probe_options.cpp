#include "probe_options.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>

namespace {

std::string require_value(int argc, char **argv, int &index) {
    if (++index >= argc) {
        throw std::invalid_argument("missing value for option");
    }
    return argv[index];
}

std::vector<int> parse_echo_sizes(const std::string &value) {
    std::vector<int> sizes;
    std::istringstream input(value);
    std::string item;
    while (std::getline(input, item, ',')) {
        std::size_t consumed = 0;
        const int size = std::stoi(item, &consumed);
        if (consumed != item.size() || size < 1 || size > 1400) {
            throw std::invalid_argument("invalid echo size");
        }
        sizes.push_back(size);
    }
    if (sizes.empty()) {
        throw std::invalid_argument("empty echo sizes");
    }
    return sizes;
}

}  // namespace

ProbeOptions parse_probe_options(int argc, char **argv) {
    ProbeOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--signal") {
            options.signal = require_value(argc, argv, index);
        } else if (option == "--ice-file") {
            options.ice_file = require_value(argc, argv, index);
        } else if (option == "--alias") {
            options.alias = require_value(argc, argv, index);
        } else if (option == "--timeout-seconds") {
            options.timeout_seconds = std::stoi(require_value(argc, argv, index));
        } else if (option == "--stop-after") {
            options.stop_after_alias_channel_ready =
                require_value(argc, argv, index) == "alias-channel-ready";
        } else if (option == "--echo-sizes") {
            options.echo_sizes =
                parse_echo_sizes(require_value(argc, argv, index));
        } else if (option == "--count") {
            options.echo_count = std::stoi(require_value(argc, argv, index));
        } else {
            throw std::invalid_argument("unknown option: " + option);
        }
    }

    const bool echo_mode = !options.echo_sizes.empty() && options.echo_count > 0;
    if (options.signal.rfind("wss://", 0) != 0 || options.ice_file.empty() ||
        !valid_alias(options.alias) || options.timeout_seconds < 1 ||
        options.echo_count < 0 ||
        options.stop_after_alias_channel_ready == echo_mode) {
        throw std::invalid_argument("invalid or incomplete probe options");
    }
    return options;
}

bool valid_alias(const std::string &alias) {
    return !alias.empty() && alias.size() <= 255 &&
           std::all_of(alias.begin(), alias.end(), [](unsigned char character) {
               return std::isalnum(character) || character == '.' ||
                      character == '-' || character == '_';
           });
}

bool valid_ice_json(const std::string &json) {
    const auto first = std::find_if_not(json.begin(), json.end(), [](unsigned char c) {
        return std::isspace(c);
    });
    const auto last = std::find_if_not(json.rbegin(), json.rend(), [](unsigned char c) {
        return std::isspace(c);
    });
    return first != json.end() && last != json.rend() && first < last.base() &&
           *first == '[' && *last == ']';
}
