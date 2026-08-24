#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>

namespace q2dz {

static bool safe_field_name(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
}

static bool safe_field_value(std::string_view value) {
    if (value.empty() || value.size() > 1024) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return c >= 0x21U && c <= 0x7eU;
    });
}

Result<GatewayEvent> parse_gateway_event(std::string_view input) {
    if (input.size() > 4096) return Result<GatewayEvent>::fail("gateway event exceeds 4096 bytes");
    while (!input.empty() && (input.back() == '\r' || input.back() == '\n')) input.remove_suffix(1);
    if (!input.starts_with("Q2DZ/1 ")) return Result<GatewayEvent>::fail("unsupported gateway protocol");
    input.remove_prefix(7);
    const auto space = input.find(' ');
    const auto event_name = input.substr(0, space);
    GatewayEventType type;
    if (event_name == "TRANSPORT_READY") type = GatewayEventType::TransportReady;
    else if (event_name == "HEARTBEAT") type = GatewayEventType::Heartbeat;
    else if (event_name == "ROOM_CLOSED") type = GatewayEventType::RoomClosed;
    else if (event_name == "FATAL") type = GatewayEventType::Fatal;
    else return Result<GatewayEvent>::fail("unknown gateway event");

    GatewayEvent event{type, {}};
    if (space == std::string_view::npos) return Result<GatewayEvent>::ok(std::move(event));
    input.remove_prefix(space + 1);
    while (!input.empty()) {
        const auto next = input.find(' ');
        const auto token = input.substr(0, next);
        const auto eq = token.find('=');
        if (eq == std::string_view::npos) return Result<GatewayEvent>::fail("gateway field lacks '='");
        const auto name = token.substr(0, eq);
        const auto value = token.substr(eq + 1);
        if (!safe_field_name(name) || !safe_field_value(value)) return Result<GatewayEvent>::fail("invalid gateway field");
        if (contains_sensitive_key(name)) return Result<GatewayEvent>::fail("gateway event contains forbidden sensitive field");
        if (!event.fields.emplace(std::string(name), std::string(value)).second) {
            return Result<GatewayEvent>::fail("duplicate gateway field");
        }
        if (next == std::string_view::npos) break;
        input.remove_prefix(next + 1);
        while (!input.empty() && input.front() == ' ') input.remove_prefix(1);
    }
    return Result<GatewayEvent>::ok(std::move(event));
}

std::string format_gateway_event(GatewayEventType type,
                                 const std::map<std::string, std::string, std::less<>>& fields) {
    std::string out = "Q2DZ/1 ";
    switch (type) {
        case GatewayEventType::TransportReady: out += "TRANSPORT_READY"; break;
        case GatewayEventType::Heartbeat: out += "HEARTBEAT"; break;
        case GatewayEventType::RoomClosed: out += "ROOM_CLOSED"; break;
        case GatewayEventType::Fatal: out += "FATAL"; break;
    }
    for (const auto& [name, value] : fields) {
        out.push_back(' ');
        out += name;
        out.push_back('=');
        out += value;
    }
    out.push_back('\n');
    return out;
}

Result<Endpoint> ready_endpoint(const GatewayEvent& event) {
    if (event.type != GatewayEventType::TransportReady) return Result<Endpoint>::fail("event is not TRANSPORT_READY");
    const auto it = event.fields.find("udp");
    if (it == event.fields.end()) return Result<Endpoint>::fail("TRANSPORT_READY lacks udp field");
    return parse_loopback_endpoint(it->second);
}

std::vector<std::byte> quake_oob_ping() {
    constexpr std::array<unsigned char, 9> bytes = {0xffU,0xffU,0xffU,0xffU,'p','i','n','g','\n'};
    std::vector<std::byte> out;
    out.reserve(bytes.size());
    for (const auto value : bytes) out.push_back(static_cast<std::byte>(value));
    return out;
}

bool is_quake_oob_ack(std::span<const std::byte> datagram) {
    if (datagram.size() < 7) return false;
    for (std::size_t i = 0; i < 4; ++i) if (datagram[i] != std::byte{0xff}) return false;
    std::string text;
    text.reserve(datagram.size() - 4);
    for (std::size_t i = 4; i < datagram.size(); ++i) text.push_back(static_cast<char>(datagram[i]));
    while (!text.empty() && (text.back() == '\0' || text.back() == '\r' || text.back() == '\n')) text.pop_back();
    return text == "ack" || text.starts_with("ack ");
}

} // namespace q2dz
