#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace q2dz {

enum class ChildEventType { transport_ready, room_closed, fatal, heartbeat };

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
    // Bound a small event burst using the existing per-line protocol ceiling.
    static constexpr std::size_t max_queued_lines = 32;
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
[[nodiscard]] std::optional<LoopbackEndpoint> parse_loopback_endpoint(std::string_view text);
[[nodiscard]] std::string percent_decode(std::string_view value, bool* ok = nullptr);
[[nodiscard]] bool field_name_is_forbidden(std::string_view name) noexcept;

} // namespace q2dz
