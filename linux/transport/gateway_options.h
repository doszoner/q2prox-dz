#pragma once

#include <cstdint>
#include <string>
#include <string_view>

struct GatewayOptions {
    std::string signal;
    std::string ice_file;
    std::string alias;
    std::uint16_t port = 0;
    int max_datagram = 0;
};

GatewayOptions parse_gateway_options(int argc, char **argv);
int gateway_error_exit_code(std::string_view message) noexcept;
int gateway_receive_exit_code(int length, std::uint32_t from_peer) noexcept;
