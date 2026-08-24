#include "q2dz/child_protocol.hpp"
#include <charconv>
#include <cctype>
#include <limits>

namespace q2dz {
namespace {
constexpr std::string_view prefix = "Q2DZ/1 ";
constexpr std::string_view fatal_line(ChildEventFramer::Error error) noexcept {
    return error == ChildEventFramer::Error::line_too_long
        ? "Q2DZ/1 FATAL stage=protocol code=line_too_long\n"
        : "Q2DZ/1 FATAL stage=protocol code=queue_overflow\n";
}
constexpr std::size_t max_fatal_line_bytes =
    fatal_line(ChildEventFramer::Error::line_too_long).size() > fatal_line(ChildEventFramer::Error::queue_overflow).size()
        ? fatal_line(ChildEventFramer::Error::line_too_long).size()
        : fatal_line(ChildEventFramer::Error::queue_overflow).size();
int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
} // namespace

bool ChildEventFramer::enqueue(std::string&& line) {
    if (lines_.size() + 1 >= max_queued_lines || queued_bytes_ > max_queued_bytes - max_fatal_line_bytes)
        return false;
    if (line.size() > max_queued_bytes - max_fatal_line_bytes - queued_bytes_) return false;
    queued_bytes_ += line.size();
    lines_.push_back(std::move(line));
    return true;
}

bool ChildEventFramer::fail(Error error) {
    if (stopped_) return false;
    stopped_=true;
    error_=error;
    pending_.clear();
    const auto line=fatal_line(error);
    lines_.emplace_back(line);
    queued_bytes_ += line.size();
    return false;
}

bool ChildEventFramer::feed(std::string_view bytes) {
    if (stopped_) return false;
    while (!bytes.empty()) {
        const auto newline=bytes.find('\n');
        if (newline==std::string_view::npos) {
            if (bytes.size() > max_line_bytes-pending_.size()) return fail(Error::line_too_long);
            pending_.append(bytes.data(), bytes.size());
            return true;
        }
        if (newline >= max_line_bytes-pending_.size()) return fail(Error::line_too_long);
        pending_.append(bytes.data(), newline+1);
        bytes.remove_prefix(newline+1);
        if (!enqueue(std::move(pending_))) return fail(Error::queue_overflow);
        pending_.clear();
    }
    return true;
}

bool ChildEventFramer::finish() {
    if (stopped_) return false;
    if (pending_.empty()) return true;
    if (!enqueue(std::move(pending_))) return fail(Error::queue_overflow);
    pending_.clear();
    return true;
}

std::deque<std::string> ChildEventFramer::drain() {
    auto result=std::move(lines_);
    lines_.clear();
    queued_bytes_=0;
    return result;
}

std::string LoopbackEndpoint::text() const {
    return "127.0.0.1:" + std::to_string(port);
}

bool field_name_is_forbidden(std::string_view name) noexcept {
    const auto n = lower(name);
    constexpr std::string_view forbidden[] = {
        "ice", "credential", "password", "secret", "token", "username",
        "candidate", "sdp", "authorization", "cookie"
    };
    for (const auto x : forbidden) if (n.find(x) != std::string::npos) return true;
    return false;
}

std::string percent_decode(std::string_view v, bool* ok) {
    bool valid = true;
    std::string out;
    out.reserve(v.size());
    for (std::size_t i=0; i<v.size(); ++i) {
        if (v[i] == '%') {
            if (i+2 >= v.size()) { valid=false; break; }
            const int hi=hex_value(v[i+1]), lo=hex_value(v[i+2]);
            if (hi<0 || lo<0) { valid=false; break; }
            const char c=static_cast<char>((hi<<4)|lo);
            if (c == '\0' || c == '\r' || c == '\n') { valid=false; break; }
            out.push_back(c); i+=2;
        } else {
            const unsigned char c=static_cast<unsigned char>(v[i]);
            if (c < 0x20 || c == 0x7f) { valid=false; break; }
            out.push_back(static_cast<char>(c));
        }
    }
    if (ok) *ok=valid;
    if (!valid) return {};
    return out;
}

std::optional<LoopbackEndpoint> parse_loopback_endpoint(std::string_view text) {
    constexpr std::string_view host="127.0.0.1:";
    if (!text.starts_with(host)) return std::nullopt;
    const auto p=text.substr(host.size());
    if (p.empty() || p.size()>5) return std::nullopt;
    unsigned port=0;
    const auto [end,ec]=std::from_chars(p.data(), p.data()+p.size(), port);
    if (ec != std::errc{} || end != p.data()+p.size() || port < 1024 || port > 65535)
        return std::nullopt;
    return LoopbackEndpoint{static_cast<std::uint16_t>(port)};
}

ParseResult parse_child_event(std::string_view line) {
    if (line.size() > 4096) return {{}, "event line exceeds 4096 bytes"};
    while (!line.empty() && (line.back()=='\r' || line.back()=='\n')) line.remove_suffix(1);
    if (!line.starts_with(prefix)) return {{}, "missing Q2DZ/1 prefix"};
    line.remove_prefix(prefix.size());
    const auto sp=line.find(' ');
    const auto type_text=line.substr(0,sp);
    ChildEvent e;
    if (type_text=="TRANSPORT_READY") e.type=ChildEventType::transport_ready;
    else if (type_text=="ROOM_CLOSED") e.type=ChildEventType::room_closed;
    else if (type_text=="FATAL") e.type=ChildEventType::fatal;
    else if (type_text=="HEARTBEAT") e.type=ChildEventType::heartbeat;
    else return {{}, "unknown child event"};
    if (sp != std::string_view::npos) line.remove_prefix(sp+1); else line={};
    while (!line.empty()) {
        while (!line.empty() && line.front()==' ') line.remove_prefix(1);
        if (line.empty()) break;
        const auto next=line.find(' ');
        const auto token=line.substr(0,next);
        const auto eq=token.find('=');
        if (eq==std::string_view::npos || eq==0 || eq+1>=token.size()) return {{}, "invalid key=value field"};
        const auto key=token.substr(0,eq);
        if (field_name_is_forbidden(key)) return {{}, "forbidden sensitive field in child event"};
        for (char c:key) if (!(std::isalnum(static_cast<unsigned char>(c)) || c=='_' || c=='-')) return {{}, "invalid field name"};
        bool ok=false;
        auto value=percent_decode(token.substr(eq+1), &ok);
        if (!ok) return {{}, "invalid percent-encoded value"};
        if (!e.fields.emplace(std::string(key), std::move(value)).second) return {{}, "duplicate field"};
        if (next==std::string_view::npos) break;
        line.remove_prefix(next+1);
    }
    if (e.type==ChildEventType::transport_ready) {
        const auto it=e.fields.find("udp");
        if (it==e.fields.end() || !parse_loopback_endpoint(it->second)) return {{}, "TRANSPORT_READY requires valid loopback udp field"};
    } else if (e.type==ChildEventType::fatal) {
        if (!e.fields.contains("stage") || !e.fields.contains("code"))
            return {{}, "FATAL requires stage and code fields"};
    }
    return {std::move(e), {}};
}
} // namespace q2dz
