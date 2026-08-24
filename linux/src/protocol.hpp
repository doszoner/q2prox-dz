#pragma once

#include "common.hpp"

#include <map>
#include <string>
#include <string_view>

namespace q2dz {

enum class GatewayEventType {
    TransportReady,
    Heartbeat,
    RoomClosed,
    Fatal
};

struct GatewayEvent {
    GatewayEventType type;
    std::map<std::string, std::string, std::less<>> fields;
};

Result<GatewayEvent> parse_gateway_event(std::string_view line);
std::string format_gateway_event(GatewayEventType type,
                                 const std::map<std::string, std::string, std::less<>>& fields);
Result<Endpoint> ready_endpoint(const GatewayEvent& event);
bool is_quake_oob_ack(std::span<const std::byte> datagram);
std::vector<std::byte> quake_oob_ping();

} // namespace q2dz
