#pragma once

#include "common.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace q2dz {

enum class HubCommandType { ice, put, drop, stop };
enum class HubCommandChannel { rooms, hub };

struct HubCommand {
    HubCommandType type{};
    std::uint64_t id_or_generation{};
    std::string room_id;
    std::vector<unsigned char> json;

    HubCommand() = default;
    HubCommand(const HubCommand&) = delete;
    HubCommand& operator=(const HubCommand&) = delete;
    HubCommand(HubCommand&& other);
    HubCommand& operator=(HubCommand&& other);
    ~HubCommand();
    void clear() noexcept;
};

class WipedLine {
public:
    explicit WipedLine(std::string&& value);
    WipedLine(const WipedLine&) = delete;
    WipedLine& operator=(const WipedLine&) = delete;
    WipedLine(WipedLine&& other);
    WipedLine& operator=(WipedLine&& other);
    ~WipedLine();

    [[nodiscard]] std::string_view view() const noexcept { return value_; }

private:
    std::string value_;
};

class HubCommandParser {
public:
    explicit HubCommandParser(HubCommandChannel channel) noexcept : channel_(channel) {}
    Result<HubCommand> parse(std::string_view line);
    [[nodiscard]] bool ice_seen() const noexcept { return ice_seen_; }

private:
    HubCommandChannel channel_;
    bool ice_seen_{false};
};

Result<HubCommand> parse_rooms_command(std::string_view line);
Result<HubCommand> parse_hub_command(std::string_view line);
std::string format_rooms_command(const HubCommand& command);
std::string format_hub_command(const HubCommand& command);

class HubCommandFramer {
public:
    enum class Error { none, line_too_long, queue_overflow };
    static constexpr std::size_t max_line_bytes = 256U * 1024U;
    static constexpr std::size_t max_queued_lines = 513U;
    static constexpr std::size_t max_queued_bytes = max_line_bytes * max_queued_lines;

    HubCommandFramer() = default;
    HubCommandFramer(const HubCommandFramer&) = delete;
    HubCommandFramer& operator=(const HubCommandFramer&) = delete;
    ~HubCommandFramer();

    [[nodiscard]] bool feed(std::string_view bytes);
    [[nodiscard]] bool finish();
    [[nodiscard]] std::deque<WipedLine> drain();
    void clear() noexcept;
    [[nodiscard]] std::size_t pending_byte_count() const noexcept { return pending_.size(); }
    [[nodiscard]] std::size_t queued_line_count() const noexcept { return lines_.size(); }
    [[nodiscard]] std::size_t queued_byte_count() const noexcept { return queued_bytes_; }
    [[nodiscard]] Error error() const noexcept { return error_; }

private:
    [[nodiscard]] bool enqueue(std::string&& line);
    [[nodiscard]] bool fail(Error error);

    std::string pending_;
    std::deque<WipedLine> lines_;
    std::size_t queued_bytes_{0};
    Error error_{Error::none};
    bool stopped_{false};
};

struct GatewayEventType {
    enum class Value { TransportReady, Heartbeat, RoomClosed, Fatal };
    constexpr GatewayEventType() noexcept : value(Value::TransportReady) {}
    constexpr GatewayEventType(Value kind) noexcept : value(kind) {}
    constexpr operator Value() const noexcept { return value; }
    static constexpr Value TransportReady = Value::TransportReady;
    static constexpr Value Heartbeat = Value::Heartbeat;
    static constexpr Value RoomClosed = Value::RoomClosed;
    static constexpr Value Fatal = Value::Fatal;
    // This out-of-range enum value is deliberately exposed as a constant,
    // keeping legacy switch statements exhaustive while distinguishing HUB/2
    // per-target FAILED from Q2DZ/1 global FATAL.
    static constexpr Value TargetFailed = static_cast<Value>(4);

    Value value;
};

struct GatewayEvent {
    GatewayEventType type;
    std::map<std::string, std::string, std::less<>> fields;
};

Result<GatewayEvent> parse_gateway_event(std::string_view line);
class GatewayEventParser {
public:
    Result<GatewayEvent> parse(std::string_view line);

private:
    int version_{0};
};
std::string format_gateway_event(GatewayEventType type,
                                 const std::map<std::string, std::string, std::less<>>& fields);
std::string format_hub_event(GatewayEventType type,
                             const std::map<std::string, std::string, std::less<>>& fields);
Result<Endpoint> ready_endpoint(const GatewayEvent& event);
bool is_quake_oob_ack(std::span<const std::byte> datagram);
std::vector<std::byte> quake_oob_ping();

} // namespace q2dz
