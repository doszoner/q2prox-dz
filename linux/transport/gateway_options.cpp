#include "gateway_options.h"

#include "probe_options.h"

#include <limits>
#include <stdexcept>

namespace {

std::string require_value(int argc, char **argv, int &index) {
    if (++index >= argc) {
        throw std::invalid_argument("missing option value");
    }
    return argv[index];
}

int parse_number(const std::string &value) {
    std::size_t consumed = 0;
    const unsigned long number = std::stoul(value, &consumed);
    if (consumed != value.size() || number > std::numeric_limits<int>::max()) {
        throw std::invalid_argument("invalid number");
    }
    return static_cast<int>(number);
}

}  // namespace

GatewayOptions parse_gateway_options(int argc, char **argv) {
    GatewayOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--bind") {
            const std::string bind = require_value(argc, argv, index);
            constexpr const char prefix[] = "127.0.0.1:";
            if (bind.rfind(prefix, 0) != 0) {
                throw std::invalid_argument("bind must be loopback IPv4");
            }
            const int port = parse_number(bind.substr(sizeof(prefix) - 1));
            if (port < 1 || port > 65535) {
                throw std::invalid_argument("invalid port");
            }
            options.port = static_cast<std::uint16_t>(port);
        } else if (option == "--signal") {
            options.signal = require_value(argc, argv, index);
        } else if (option == "--ice-file") {
            options.ice_file = require_value(argc, argv, index);
        } else if (option == "--alias") {
            options.alias = require_value(argc, argv, index);
        } else if (option == "--max-datagram") {
            options.max_datagram = parse_number(require_value(argc, argv, index));
        } else {
            throw std::invalid_argument("unknown option: " + option);
        }
    }
    if (options.port == 0 || options.signal.rfind("wss://", 0) != 0 ||
        options.ice_file.empty() || !valid_alias(options.alias) ||
        options.max_datagram != 1400) {
        throw std::invalid_argument("invalid or incomplete gateway options");
    }
    return options;
}

int gateway_error_exit_code(std::string_view message) noexcept {
    return message == "Connection to peer was closed" ? 20 : 2;
}

int gateway_receive_exit_code(int length, std::uint32_t from_peer) noexcept {
    return length < 0 ? (from_peer != 0 ? 20 : 2) : 0;
}
