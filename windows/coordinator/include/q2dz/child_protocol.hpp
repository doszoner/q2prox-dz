#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
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

struct HubParseResult {
    std::optional<HubCommand> command;
    std::string error;
};

class HubCommandParser {
public:
    explicit HubCommandParser(HubCommandChannel channel) noexcept : channel_(channel) {}
    [[nodiscard]] HubParseResult parse(std::string_view line);
    [[nodiscard]] bool ice_seen() const noexcept { return ice_seen_; }

private:
    HubCommandChannel channel_;
    bool ice_seen_{false};
};

[[nodiscard]] HubParseResult parse_rooms_command(std::string_view line);
[[nodiscard]] HubParseResult parse_hub_command(std::string_view line);
[[nodiscard]] std::string format_rooms_command(const HubCommand& command);
[[nodiscard]] std::string format_hub_command(const HubCommand& command);

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

enum class ChildEventType { transport_ready, room_closed, fatal, heartbeat, target_failed };

struct LoopbackEndpoint {
    std::uint16_t port{};
    [[nodiscard]] std::string text() const;
};

struct ChildEvent {
    ChildEventType type{};
    std::map<std::string, std::string, std::less<>> fields;
};

struct ParseResult {
    std::optional<ChildEvent> event;
    std::string error;
};

class ChildEventFramer {
public:
    enum class Error { none, line_too_long, queue_overflow };

    static constexpr std::size_t max_line_bytes = 4096;
    // Keep one slot for a fatal event while covering 256 READY events and a heartbeat.
    static constexpr std::size_t max_queued_lines = 258;
    static constexpr std::size_t max_queued_bytes = 64 * 1024;

    [[nodiscard]] bool feed(std::string_view bytes);
    [[nodiscard]] bool finish();
    [[nodiscard]] std::deque<std::string> drain();
    [[nodiscard]] std::size_t pending_byte_count() const noexcept { return pending_.size(); }
    [[nodiscard]] std::size_t queued_line_count() const noexcept { return lines_.size(); }
    [[nodiscard]] std::size_t queued_byte_count() const noexcept { return queued_bytes_; }
    [[nodiscard]] Error error() const noexcept { return error_; }

private:
    [[nodiscard]] bool enqueue(std::string&& line);
    [[nodiscard]] bool fail(Error error);

    std::string pending_;
    std::deque<std::string> lines_;
    std::size_t queued_bytes_{0};
    Error error_{Error::none};
    bool stopped_{false};
};

[[nodiscard]] ParseResult parse_child_event(std::string_view line);
class ChildEventParser {
public:
    [[nodiscard]] ParseResult parse(std::string_view line);

private:
    int version_{0};
};
[[nodiscard]] std::string format_child_event(ChildEventType type,
                                             const std::map<std::string, std::string, std::less<>>& fields);
[[nodiscard]] std::optional<LoopbackEndpoint> parse_loopback_endpoint(std::string_view text);
[[nodiscard]] std::string percent_decode(std::string_view value, bool* ok = nullptr);
[[nodiscard]] bool field_name_is_forbidden(std::string_view name) noexcept;

} // namespace q2dz
