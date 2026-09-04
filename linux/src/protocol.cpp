#include "protocol.hpp"

#include "json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>

namespace q2dz {
namespace {

constexpr std::size_t kHubMaxLineBytes = 256U * 1024U;
constexpr std::string_view kRoomsPrefix = "Q2DZ/ROOMS/2 ";
constexpr std::string_view kHubPrefix = "Q2DZ/HUB/2 ";

bool has_forbidden_control(std::string_view value) {
    return std::any_of(value.begin(), value.end(), [](char raw) {
        return raw == '\0' || raw == '\r' || raw == '\n';
    });
}

bool valid_utf8(std::string_view value) {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto byte = static_cast<unsigned char>(value[index]);
        std::size_t length = 0;
        std::uint32_t codepoint = 0;
        if (byte <= 0x7fU) {
            ++index;
            continue;
        } else if (byte >= 0xc2U && byte <= 0xdfU) {
            length = 2;
            codepoint = byte & 0x1fU;
        } else if (byte >= 0xe0U && byte <= 0xefU) {
            length = 3;
            codepoint = byte & 0x0fU;
        } else if (byte >= 0xf0U && byte <= 0xf4U) {
            length = 4;
            codepoint = byte & 0x07U;
        } else {
            return false;
        }
        if (index + length > value.size()) return false;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto continuation = static_cast<unsigned char>(value[index + offset]);
            if ((continuation & 0xc0U) != 0x80U) return false;
            codepoint = (codepoint << 6U) | (continuation & 0x3fU);
        }
        if ((length == 3 && codepoint < 0x800U) ||
            (length == 4 && codepoint < 0x10000U) ||
            codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) return false;
        index += length;
    }
    return true;
}

bool safe_field_name(std::string_view name) {
    if (name.empty() || name.size() > 64U) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
}

bool is_alias_field(std::string_view name) noexcept {
    return name.size() == 5U && std::tolower(static_cast<unsigned char>(name[0])) == 'a' &&
           std::tolower(static_cast<unsigned char>(name[1])) == 'l' &&
           std::tolower(static_cast<unsigned char>(name[2])) == 'i' &&
           std::tolower(static_cast<unsigned char>(name[3])) == 'a' &&
           std::tolower(static_cast<unsigned char>(name[4])) == 's';
}

bool is_room_alias_value(std::string_view value) noexcept {
    constexpr std::array<std::string_view, 2> prefixes = {"q2.", "room.q2ded."};
    for (const auto prefix : prefixes) {
        if (value.size() < prefix.size()) continue;
        for (std::size_t start = 0; start + prefix.size() <= value.size(); ++start) {
            bool match = true;
            for (std::size_t index = 0; index < prefix.size(); ++index) {
                if (std::tolower(static_cast<unsigned char>(value[start + index])) != prefix[index]) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
    }
    return false;
}

bool allowed_hub_event_field(GatewayEventType type, std::string_view name) noexcept {
    if (type == GatewayEventType::TransportReady) return name == "id" || name == "udp" || name == "peer";
    if (type == GatewayEventType::RoomClosed) return name == "id" || name == "reason";
    if (type == GatewayEventType::Heartbeat) return name == "state" || name == "targets" || name == "ready";
    if (type == GatewayEventType::Fatal || type == GatewayEventType::TargetFailed) return name == "id" || name == "stage" || name == "code";
    return false;
}

bool safe_field_value(std::string_view value) {
    if (value.empty() || value.size() > 1024U) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return c >= 0x21U && c <= 0x7eU;
    });
}

bool safe_room_id(std::string_view value) {
    if (value.empty() || value.size() > 255U) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '-' || c == '_';
    });
}

bool parse_u64(std::string_view text, std::uint64_t& value, bool allow_zero = true) {
    if (text.empty()) return false;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() && (allow_zero || value != 0);
}

void secure_clear_bytes(std::vector<unsigned char>& value) noexcept {
    volatile unsigned char* data = value.empty() ? nullptr : value.data();
    for (std::size_t index = 0; index < value.size(); ++index) data[index] = 0;
    value.clear();
    value.shrink_to_fit();
}

class WipedText {
public:
    explicit WipedText(std::string value) : value_(value) { q2dz::secure_clear(value); }
    ~WipedText() { q2dz::secure_clear(value_); }
    [[nodiscard]] const std::string& get() const noexcept { return value_; }

private:
    std::string value_;
};

bool compact_json(std::string_view value) {
    bool quoted = false;
    bool escaped = false;
    for (const char raw : value) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (raw == '\\') escaped = true;
            else if (raw == '"') quoted = false;
        } else if (raw == '"') {
            quoted = true;
        } else if (raw == ' ' || raw == '\t') {
            return false;
        }
    }
    return !quoted && !escaped;
}

Result<void> validate_json_payload(std::string_view payload, HubCommandType type) {
    if (payload.empty() || !valid_utf8(payload) || has_forbidden_control(payload) || !compact_json(payload)) {
        return Result<void>::fail("hub JSON payload is invalid");
    }
    auto parsed = json::parse(payload);
    if (!parsed) return Result<void>::fail("hub JSON payload is invalid");
    if (type == HubCommandType::ice && !parsed.value().is_array()) {
        return Result<void>::fail("ICE payload must be an array");
    }
    if (type == HubCommandType::put && !parsed.value().is_object()) {
        return Result<void>::fail("PUT payload must be an object");
    }
    return Result<void>::ok();
}

Result<HubCommand> parse_command(std::string_view line,
                                 HubCommandChannel channel,
                                 bool* ice_seen) {
    if (line.empty() || line.size() > kHubMaxLineBytes) {
        return Result<HubCommand>::fail("hub command exceeds 256 KiB or is empty");
    }
    if (!valid_utf8(line) || has_forbidden_control(line)) {
        return Result<HubCommand>::fail("hub command contains invalid control or UTF-8");
    }
    const auto prefix = channel == HubCommandChannel::rooms ? kRoomsPrefix : kHubPrefix;
    if (!line.starts_with(prefix)) return Result<HubCommand>::fail("unsupported hub protocol");
    line.remove_prefix(prefix.size());

    const auto command_end = line.find(' ');
    const auto command_name = line.substr(0, command_end);
    HubCommandType type{};
    if (command_name == "ICE") type = HubCommandType::ice;
    else if (command_name == "PUT") type = HubCommandType::put;
    else if (command_name == "DROP") type = HubCommandType::drop;
    else if (command_name == "STOP" && channel == HubCommandChannel::hub) {
        if (command_end != std::string_view::npos) return Result<HubCommand>::fail("STOP takes no fields");
        HubCommand stop;
        stop.type = HubCommandType::stop;
        return Result<HubCommand>::ok(std::move(stop));
    } else {
        return Result<HubCommand>::fail("unknown hub command");
    }
    if (command_end == std::string_view::npos) return Result<HubCommand>::fail("hub command lacks fields");
    line.remove_prefix(command_end + 1U);

    HubCommand out;
    out.type = type;
    const bool room_channel = channel == HubCommandChannel::rooms;
    const std::string_view identity = type == HubCommandType::ice
        ? "generation="
        : (room_channel ? "room=" : "id=");
    if (type == HubCommandType::ice || type == HubCommandType::put) {
        const auto json_separator = line.find(" json=");
        if (json_separator == std::string_view::npos || json_separator < identity.size()) {
            return Result<HubCommand>::fail("hub command requires fixed json= field");
        }
        if (!line.starts_with(identity)) return Result<HubCommand>::fail("hub command identity field is invalid");
        const auto identity_value = line.substr(identity.size(), json_separator - identity.size());
        if (identity_value.empty()) return Result<HubCommand>::fail("hub command identity field is empty");
        if (type == HubCommandType::ice) {
            if (!parse_u64(identity_value, out.id_or_generation, false)) {
                return Result<HubCommand>::fail("ICE generation is outside uint64 range");
            }
        } else if (room_channel) {
            if (!safe_room_id(identity_value)) return Result<HubCommand>::fail("room ID is unsafe");
            out.room_id = std::string(identity_value);
        } else if (!parse_u64(identity_value, out.id_or_generation, false)) {
            return Result<HubCommand>::fail("target ID is outside uint64 range");
        }
        if (type == HubCommandType::put && ice_seen != nullptr && !*ice_seen) {
            return Result<HubCommand>::fail("PUT requires a preceding ICE command");
        }
        const auto payload = line.substr(json_separator + 6U);
        if (auto valid = validate_json_payload(payload, type); !valid) {
            return Result<HubCommand>::fail(valid.error().message);
        }
        out.json.assign(payload.begin(), payload.end());
    } else {
        if (!line.starts_with(identity) || line.find(' ') != std::string_view::npos) {
            return Result<HubCommand>::fail("hub DROP field is invalid");
        }
        const auto identity_value = line.substr(identity.size());
        if (room_channel) {
            if (!safe_room_id(identity_value)) return Result<HubCommand>::fail("room ID is unsafe");
            out.room_id = std::string(identity_value);
        } else if (!parse_u64(identity_value, out.id_or_generation, false)) {
            return Result<HubCommand>::fail("target ID is outside uint64 range");
        }
    }
    if (type == HubCommandType::ice && ice_seen != nullptr) *ice_seen = true;
    return Result<HubCommand>::ok(std::move(out));
}

std::string command_json(const HubCommand& command) {
    if (command.json.empty()) return {};
    return std::string(reinterpret_cast<const char*>(command.json.data()), command.json.size());
}

std::string format_command(HubCommandChannel channel, const HubCommand& command) {
    const auto prefix = channel == HubCommandChannel::rooms ? kRoomsPrefix : kHubPrefix;
    std::string out(prefix);
    WipedText payload(command_json(command));
    switch (command.type) {
        case HubCommandType::ice:
            if (command.id_or_generation == 0 || !validate_json_payload(payload.get(), command.type)) return {};
            out += "ICE generation=" + std::to_string(command.id_or_generation) + " json=";
            break;
        case HubCommandType::put:
            if (channel == HubCommandChannel::rooms) {
                if (!safe_room_id(command.room_id)) return {};
                out += "PUT room=" + command.room_id + " json=";
            } else {
                if (command.id_or_generation == 0) return {};
                out += "PUT id=" + std::to_string(command.id_or_generation) + " json=";
            }
            if (!validate_json_payload(payload.get(), command.type)) return {};
            break;
        case HubCommandType::drop:
            if (channel == HubCommandChannel::rooms) {
                if (!safe_room_id(command.room_id)) return {};
                out += "DROP room=" + command.room_id;
            } else {
                if (command.id_or_generation == 0) return {};
                out += "DROP id=" + std::to_string(command.id_or_generation);
            }
            return out;
        case HubCommandType::stop:
            if (channel != HubCommandChannel::hub || !command.room_id.empty() || !command.json.empty() || command.id_or_generation != 0) return {};
            return out + "STOP";
    }
    out += payload.get();
    return out.size() <= kHubMaxLineBytes ? out : std::string{};
}

Result<GatewayEvent> parse_v1_event(std::string_view input) {
    if (input.size() > 4096U) return Result<GatewayEvent>::fail("gateway event exceeds 4096 bytes");
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
    input.remove_prefix(space + 1U);
    while (!input.empty()) {
        const auto next = input.find(' ');
        const auto token = input.substr(0, next);
        const auto eq = token.find('=');
        if (eq == std::string_view::npos) return Result<GatewayEvent>::fail("gateway field lacks '='");
        const auto name = token.substr(0, eq);
        const auto value = token.substr(eq + 1U);
        if (!safe_field_name(name) || !safe_field_value(value)) return Result<GatewayEvent>::fail("invalid gateway field");
        if (contains_sensitive_key(name)) return Result<GatewayEvent>::fail("gateway event contains forbidden sensitive field");
        if (!event.fields.emplace(std::string(name), std::string(value)).second) {
            return Result<GatewayEvent>::fail("duplicate gateway field");
        }
        if (next == std::string_view::npos) break;
        input.remove_prefix(next + 1U);
        while (!input.empty() && input.front() == ' ') input.remove_prefix(1);
    }
    return Result<GatewayEvent>::ok(std::move(event));
}

Result<GatewayEvent> parse_v2_event(std::string_view input) {
    if (input.empty() || input.size() > kHubMaxLineBytes || has_forbidden_control(input) || !valid_utf8(input)) {
        return Result<GatewayEvent>::fail("hub event framing is invalid");
    }
    if (!input.starts_with(kHubPrefix)) return Result<GatewayEvent>::fail("unsupported hub event protocol");
    input.remove_prefix(kHubPrefix.size());
    const auto space = input.find(' ');
    const auto event_name = input.substr(0, space);
    GatewayEventType type;
    if (event_name == "READY") type = GatewayEventType::TransportReady;
    else if (event_name == "CLOSED") type = GatewayEventType::RoomClosed;
    else if (event_name == "FAILED") type = GatewayEventType::TargetFailed;
    else if (event_name == "HEARTBEAT") type = GatewayEventType::Heartbeat;
    else return Result<GatewayEvent>::fail("unknown hub event");

    GatewayEvent event{type, {}};
    if (space != std::string_view::npos) input.remove_prefix(space + 1U);
    while (!input.empty()) {
        const auto next = input.find(' ');
        const auto token = input.substr(0, next);
        const auto eq = token.find('=');
        if (eq == std::string_view::npos || eq == 0 || eq + 1U >= token.size()) {
            return Result<GatewayEvent>::fail("invalid hub event field");
        }
        const auto name = token.substr(0, eq);
        const auto value = token.substr(eq + 1U);
        if (!safe_field_name(name) || is_alias_field(name) || !safe_field_value(value) || is_room_alias_value(value) ||
            contains_sensitive_key(name) || contains_sensitive_key(value)) {
            return Result<GatewayEvent>::fail("invalid or sensitive hub event field");
        }
        if (!allowed_hub_event_field(type, name)) return Result<GatewayEvent>::fail("hub event contains an unknown field");
        if (!event.fields.emplace(std::string(name), std::string(value)).second) {
            return Result<GatewayEvent>::fail("duplicate hub event field");
        }
        if (next == std::string_view::npos) break;
        input.remove_prefix(next + 1U);
    }
    const auto id = event.fields.find("id");
    if (type != GatewayEventType::Heartbeat) {
        std::uint64_t parsed_id = 0;
        if (id == event.fields.end() || !parse_u64(id->second, parsed_id, false)) {
            return Result<GatewayEvent>::fail("hub event requires a nonzero id");
        }
    }
    if (type == GatewayEventType::TransportReady) {
        const auto udp = event.fields.find("udp");
        const auto peer = event.fields.find("peer");
        std::uint64_t parsed_peer = 0;
        if (udp == event.fields.end() || !parse_loopback_endpoint(udp->second) ||
            peer == event.fields.end() || !parse_u64(peer->second, parsed_peer, false)) {
            return Result<GatewayEvent>::fail("READY requires loopback udp and numeric peer");
        }
    } else if (type == GatewayEventType::RoomClosed) {
        if (!event.fields.contains("reason")) return Result<GatewayEvent>::fail("CLOSED requires reason");
    } else if (type == GatewayEventType::Fatal || type == GatewayEventType::TargetFailed) {
        if (!event.fields.contains("stage") || !event.fields.contains("code")) {
            return Result<GatewayEvent>::fail("FAILED requires stage and code");
        }
    } else {
        const auto state = event.fields.find("state");
        const auto targets = event.fields.find("targets");
        const auto ready = event.fields.find("ready");
        std::uint64_t target_count = 0;
        std::uint64_t ready_count = 0;
        if (state == event.fields.end() || state->second != "ok" ||
            targets == event.fields.end() || ready == event.fields.end() ||
            !parse_u64(targets->second, target_count) || target_count > 256U ||
            !parse_u64(ready->second, ready_count) || ready_count > target_count) {
            return Result<GatewayEvent>::fail("HEARTBEAT counters are invalid");
        }
    }
    return Result<GatewayEvent>::ok(std::move(event));
}

bool valid_event_fields(const std::map<std::string, std::string, std::less<>>& fields) {
    return std::all_of(fields.begin(), fields.end(), [](const auto& field) {
        return safe_field_name(field.first) && safe_field_value(field.second) &&
               !contains_sensitive_key(field.first) && !contains_sensitive_key(field.second);
    });
}

bool valid_hub_event_fields(GatewayEventType type,
                            const std::map<std::string, std::string, std::less<>>& fields) {
    return std::all_of(fields.begin(), fields.end(), [type](const auto& field) {
        return allowed_hub_event_field(type, field.first) && safe_field_name(field.first) &&
               !is_alias_field(field.first) && safe_field_value(field.second) &&
               !is_room_alias_value(field.second) && !contains_sensitive_key(field.first) &&
               !contains_sensitive_key(field.second);
    });
}

void append_hub_event_fields(std::string& out,
                             GatewayEventType type,
                             const std::map<std::string, std::string, std::less<>>& fields) {
    std::array<std::string_view, 4> order{};
    std::size_t count = 0;
    if (type == GatewayEventType::TargetFailed) {
        order = {"id", "stage", "code", {}};
        count = 3;
    } else {
        switch (type) {
            case GatewayEventType::TransportReady: order = {"id", "udp", "peer", {}}; count = 3; break;
            case GatewayEventType::RoomClosed: order = {"id", "reason", {}, {}}; count = 2; break;
            case GatewayEventType::Fatal: order = {"id", "stage", "code", {}}; count = 3; break;
            case GatewayEventType::Heartbeat: order = {"state", "targets", "ready", {}}; count = 3; break;
        }
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (const auto it = fields.find(order[index]); it != fields.end()) {
            out.push_back(' ');
            out += order[index];
            out.push_back('=');
            out += it->second;
        }
    }
    for (const auto& [name, value] : fields) {
        bool ordered = false;
        for (std::size_t index = 0; index < count; ++index) ordered = ordered || name == order[index];
        if (!ordered) {
            out.push_back(' ');
            out += name;
            out.push_back('=');
            out += value;
        }
    }
}

} // namespace

WipedLine::WipedLine(std::string&& value) : value_(value) {
    q2dz::secure_clear(value);
}

WipedLine::WipedLine(WipedLine&& other) : value_(other.value_) {
    q2dz::secure_clear(other.value_);
}

WipedLine& WipedLine::operator=(WipedLine&& other) {
    if (this != &other) {
        q2dz::secure_clear(value_);
        value_ = other.value_;
        q2dz::secure_clear(other.value_);
    }
    return *this;
}

WipedLine::~WipedLine() { q2dz::secure_clear(value_); }

HubCommand::HubCommand(HubCommand&& other)
    : type(other.type), id_or_generation(other.id_or_generation), room_id(other.room_id), json(std::move(other.json)) {
    q2dz::secure_clear(other.room_id);
    secure_clear_bytes(other.json);
}

HubCommand& HubCommand::operator=(HubCommand&& other) {
    if (this != &other) {
        clear();
        type = other.type;
        id_or_generation = other.id_or_generation;
        room_id = other.room_id;
        json = std::move(other.json);
        q2dz::secure_clear(other.room_id);
        secure_clear_bytes(other.json);
    }
    return *this;
}

HubCommand::~HubCommand() { clear(); }

void HubCommand::clear() noexcept {
    secure_clear_bytes(json);
    q2dz::secure_clear(room_id);
    type = HubCommandType{};
    id_or_generation = 0;
}

Result<HubCommand> HubCommandParser::parse(std::string_view line) {
    return parse_command(line, channel_, &ice_seen_);
}

Result<HubCommand> parse_rooms_command(std::string_view line) {
    bool ice_seen = false;
    return parse_command(line, HubCommandChannel::rooms, &ice_seen);
}

Result<HubCommand> parse_hub_command(std::string_view line) {
    bool ice_seen = false;
    return parse_command(line, HubCommandChannel::hub, &ice_seen);
}

std::string format_rooms_command(const HubCommand& command) {
    return format_command(HubCommandChannel::rooms, command);
}

std::string format_hub_command(const HubCommand& command) {
    return format_command(HubCommandChannel::hub, command);
}

bool HubCommandFramer::enqueue(std::string&& line) {
    if (lines_.size() >= max_queued_lines || queued_bytes_ > max_queued_bytes ||
        line.size() > max_queued_bytes - queued_bytes_) {
        q2dz::secure_clear(line);
        return false;
    }
    queued_bytes_ += line.size();
    lines_.emplace_back(std::move(line));
    return true;
}

bool HubCommandFramer::fail(Error error) {
    stopped_ = true;
    error_ = error;
    clear();
    error_ = error;
    return false;
}

HubCommandFramer::~HubCommandFramer() { clear(); }

void HubCommandFramer::clear() noexcept {
    q2dz::secure_clear(pending_);
    lines_.clear();
    queued_bytes_ = 0;
}

bool HubCommandFramer::feed(std::string_view bytes) {
    if (stopped_) return false;
    while (!bytes.empty()) {
        const auto newline = bytes.find('\n');
        if (newline == std::string_view::npos) {
            if (pending_.size() > max_line_bytes || bytes.size() > max_line_bytes - pending_.size()) return fail(Error::line_too_long);
            pending_.append(bytes.data(), bytes.size());
            return true;
        }
        if (pending_.size() > max_line_bytes || newline > max_line_bytes - pending_.size()) return fail(Error::line_too_long);
        pending_.append(bytes.data(), newline);
        bytes.remove_prefix(newline + 1U);
        if (!enqueue(std::move(pending_))) return fail(Error::queue_overflow);
        pending_.clear();
    }
    return true;
}

bool HubCommandFramer::finish() {
    if (stopped_) return false;
    if (!pending_.empty() && !enqueue(std::move(pending_))) return fail(Error::queue_overflow);
    pending_.clear();
    return true;
}

std::deque<WipedLine> HubCommandFramer::drain() {
    auto result = std::move(lines_);
    lines_.clear();
    queued_bytes_ = 0;
    return result;
}

std::deque<WipedLine> HubCommandFramer::drain(std::size_t maximum) {
    if (maximum >= lines_.size()) return drain();
    std::deque<WipedLine> result;
    while (maximum != 0 && !lines_.empty()) {
        queued_bytes_ -= lines_.front().view().size();
        result.emplace_back(std::move(lines_.front()));
        lines_.pop_front();
        --maximum;
    }
    return result;
}

Result<GatewayEvent> parse_gateway_event(std::string_view input) {
    if (input.starts_with(kHubPrefix)) return parse_v2_event(input);
    return parse_v1_event(input);
}

Result<GatewayEvent> GatewayEventParser::parse(std::string_view line) {
    const int candidate = line.starts_with("Q2DZ/1 ") ? 1 : line.starts_with(kHubPrefix) ? 2 : 0;
    if (candidate == 0 || (version_ != 0 && version_ != candidate)) {
        return Result<GatewayEvent>::fail("gateway protocol version changed in stream");
    }
    auto parsed = parse_gateway_event(line);
    if (parsed && version_ == 0) version_ = candidate;
    return parsed;
}

std::string format_gateway_event(GatewayEventType type,
                                 const std::map<std::string, std::string, std::less<>>& fields) {
    if (!valid_event_fields(fields)) return {};
    std::string out = "Q2DZ/1 ";
    if (type == GatewayEventType::TargetFailed) return {};
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

std::string format_hub_event(GatewayEventType type,
                             const std::map<std::string, std::string, std::less<>>& fields) {
    if (!valid_hub_event_fields(type, fields)) return {};
    std::string out = "Q2DZ/HUB/2 ";
    if (type == GatewayEventType::TargetFailed) {
        out += "FAILED";
    } else switch (type) {
        case GatewayEventType::TransportReady: out += "READY"; break;
        case GatewayEventType::Heartbeat: out += "HEARTBEAT"; break;
        case GatewayEventType::RoomClosed: out += "CLOSED"; break;
        case GatewayEventType::Fatal: return {};
    }
    append_hub_event_fields(out, type, fields);
    if (!parse_v2_event(out)) return {};
    return out.size() <= kHubMaxLineBytes ? out : std::string{};
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
