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
    unsigned long number = 0;
    try {
        number = std::stoul(value, &consumed);
    } catch (const std::exception &) {
        throw std::invalid_argument("invalid number");
    }
    if (consumed != value.size() || number > std::numeric_limits<int>::max()) {
        throw std::invalid_argument("invalid number");
    }
    return static_cast<int>(number);
}

}  // namespace

GatewayOptions parse_gateway_options(int argc, char **argv) {
    GatewayOptions options;
    if (argc == 1) return options;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--max-datagram") {
            options.max_datagram = parse_number(require_value(argc, argv, index));
        } else if (option == "--bind") {
            options.legacy_arguments = true;
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
            options.legacy_arguments = true;
            options.signal = require_value(argc, argv, index);
        } else if (option == "--ice-file") {
            options.legacy_arguments = true;
            options.ice_file = require_value(argc, argv, index);
        } else if (option == "--alias") {
            options.legacy_arguments = true;
            options.alias = require_value(argc, argv, index);
        } else {
            throw std::invalid_argument("unknown option: " + option);
        }
    }
    if (options.max_datagram != 1400) {
        throw std::invalid_argument("invalid or incomplete gateway options");
    }
    if (options.legacy_arguments &&
        (options.port == 0 || options.signal.rfind("wss://", 0) != 0 ||
         options.ice_file.empty() || !valid_alias(options.alias))) {
        throw std::invalid_argument("invalid or incomplete legacy gateway options");
    }
    return options;
}

int gateway_error_exit_code(std::string_view message) noexcept {
    return message == "Connection to peer was closed" ? 20 : 2;
}

std::string_view gateway_error_category(std::string_view message) noexcept {
    // Distinguishes the intended bounded init failure (the sealed-memfd
    // ICE/transport-configure path the legacy-contract verifier actually
    // exercises) from CLI argument errors, which otherwise shared the same
    // default "startup_failed" category (round-4 defect: an unknown or
    // invalid option impersonated the intended failure in the verifier).
    // Matched on our own fixed throw messages — never on raw attacker
    // text — and only the category, never the message, is logged. Any
    // other runtime failure keeps the generic "startup_failed" default.
    if (message == "Connection to peer was closed") return "peer_closed";
    if (message.rfind("unknown option: ", 0) == 0) return "unknown_option";
    if (message == "missing option value" || message == "invalid number" ||
        message == "invalid port" || message == "bind must be loopback IPv4" ||
        message == "invalid or incomplete gateway options" ||
        message == "invalid or incomplete legacy gateway options") {
        return "invalid_option";
    }
    if (message == "legacy transport initialization failed") return "transport_init_failed";
    return "startup_failed";
}

int gateway_receive_exit_code(int length, std::uint32_t from_peer) noexcept {
    return length < 0 ? (from_peer != 0 ? 20 : 2) : 0;
}
