#include "common.hpp"
#include "content.hpp"
#include "json.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <set>
#include <string>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>

#include <cctype>
#include <climits>
#include <cstdlib>
#include <sstream>
namespace fs = std::filesystem;

namespace {

using q2dz::UniqueFd;

constexpr int kUsage = 64;
constexpr int kConfig = 65;
constexpr int kUnavailable = 69;
constexpr int kInternal = 70;
constexpr int kQ2Failure = 74;
constexpr int kCleanupFailure = 75;

#ifndef Q2DZ_BUILD_VERSION
#define Q2DZ_BUILD_VERSION "dev"
#endif

// Sole stderr sink: allow-listed SESSION/TARGET/GATEWAY events (tagged COORDINATOR by the launcher)
// and other diagnostics (landing as CHILD lines) share this one writer.
void emit_event(std::string_view line) {
    std::cerr << line << '\n';
}

// Maps a gateway-supplied stage/code/reason string onto a safe vocabulary
// before it reaches TARGET/GATEWAY logging. The gateway's own diagnostic
// vocabulary (managed_gateway_main.cpp's emit_fatal stage/code, and the
// shared gateway's TargetFailed/RoomClosed code/reason fields) is not
// controlled by this coordinator and can contain a sensitivity-filter
// substring (e.g. "ice") that would otherwise get the whole log line
// suppressed by the launcher's filter. Mirrors
// windows/coordinator/src/windows/main.cpp's safe_gateway_reason; kept as a
// separate copy per coordinator rather than a shared header (D-group
// decision: duplication accepted here).
std::string safe_gateway_reason(std::string_view reason) {
    static const std::unordered_map<std::string_view, std::string_view> kKnownCodes = {
        {"ice_configuration_failed", "transport_config_failed"},
        {"ice_configuration_too_large", "transport_config_oversized"},
        {"ice_required_before_put", "transport_config_missing"},
        {"invalid_ice_generation", "transport_config_invalid"},
        {"invalid_ice_server", "transport_config_invalid"},
        {"invalid_ice_servers", "transport_config_invalid"},
        {"transport_configure_ice_failed", "transport_startup_failed"},
        {"startup_configure_ice_failed", "transport_startup_failed"},
        {"memfd-failed", "transport_startup_failed"},
    };
    const auto found = kKnownCodes.find(reason);
    if (found != kKnownCodes.end()) return std::string(found->second);
    std::string lowered(reason);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static constexpr std::array<std::string_view, 17> kFilteredTokens = {
        "credential", "password", "secret", "token", "authorization", "ice",
        "candidate", "sdp", "turn:", "turns:", "username", "fingerprint",
        "peer", "local-description", "remote-description", "session-description",
        "aliaserror",
    };
    for (const auto token : kFilteredTokens) {
        if (lowered.find(token) != std::string::npos) return "transport_reason_redacted";
    }
    return std::string(reason);
}

// Generic allow-listed values are bounded to 64 chars (D11 grammar); room= is
// the correlation key and the validator already bounds it to 255, so a
// room-id call site passes max_len=255 explicitly instead of truncating
// further here (B2).
std::string log_value(std::string_view value, std::size_t max_len = 64) {
    std::string out;
    out.reserve(std::min<std::size_t>(value.size(), max_len));
    for (const char c : value) {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                          c == ':' || c == '-';
        out.push_back(safe ? c : '_');
        if (out.size() >= max_len) break;
    }
    return out.empty() ? std::string("none") : out;
}

std::atomic<int> signal_seen{0};
int signal_pipe_write = -1;
void signal_handler(int signal_number) {
    signal_seen.store(signal_number);
    if (signal_pipe_write >= 0) {
        const unsigned char byte = static_cast<unsigned char>(signal_number);
        (void)::write(signal_pipe_write, &byte, 1);
    }
}

struct Options {
    std::string gateway;
    std::string legacy_gateway;
    std::string adapter;
    std::string q2;
    std::string wine;
    std::string wineprefix;
    std::string runtime_base;
    std::string game;
    int ready_timeout_ms = 60000;
    int shutdown_grace_ms = 3000;
};

void usage(std::ostream& out) {
    out <<
        "Usage: q2dz-session --gateway PATH --legacy-gateway PATH --adapter FILE [options]\n"
        "\n"
        "Required room configuration is read only from stdin.\n"
        "Options:\n"
        "  --q2 PATH                 Exact Q2PRO-X.exe path; otherwise discover it\n"
        "  --wine PATH               wine/wine64 executable; otherwise discover it\n"
        "  --wineprefix DIR          Existing Wine prefix (not modified by q2dz)\n"
        "  --runtime-base DIR        Private runtime root\n"
        "  --game 3zb2               Required Q2PRO-X game profile\n"
        "  Hub commands are read as newline-delimited Q2DZ/ROOMS/2 commands\n"
        "  --ready-timeout-ms N      1000..180000 (default 60000)\n"
        "  --shutdown-grace-ms N     100..30000 (default 3000)\n";
}

q2dz::Result<int> parse_int(std::string_view text, int min, int max) {
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < min || value > max) {
        return q2dz::Result<int>::fail("integer outside accepted range");
    }
    return q2dz::Result<int>::ok(value);
}

q2dz::Result<Options> parse_options(int argc, char** argv) {
    Options out;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto take = [&](std::string& target) -> bool {
            if (i + 1 >= argc) return false;
            target = argv[++i];
            return true;
        };
        if (arg == "--gateway") { if (!take(out.gateway)) return q2dz::Result<Options>::fail("--gateway requires value"); }
        else if (arg == "--legacy-gateway") { if (!take(out.legacy_gateway)) return q2dz::Result<Options>::fail("--legacy-gateway requires value"); }
        else if (arg == "--adapter") { if (!take(out.adapter)) return q2dz::Result<Options>::fail("--adapter requires value"); }
        else if (arg == "--q2") { if (!take(out.q2)) return q2dz::Result<Options>::fail("--q2 requires value"); }
        else if (arg == "--wine") { if (!take(out.wine)) return q2dz::Result<Options>::fail("--wine requires value"); }
        else if (arg == "--wineprefix") { if (!take(out.wineprefix)) return q2dz::Result<Options>::fail("--wineprefix requires value"); }
        else if (arg == "--runtime-base") { if (!take(out.runtime_base)) return q2dz::Result<Options>::fail("--runtime-base requires value"); }
        else if (arg == "--game") { if (!take(out.game)) return q2dz::Result<Options>::fail("--game requires value"); }
        else if (arg == "--ready-timeout-ms" || arg == "--shutdown-grace-ms") {
            std::string text;
            if (!take(text)) return q2dz::Result<Options>::fail(arg + " requires value");
            auto parsed = parse_int(text, arg == "--ready-timeout-ms" ? 1000 : 100,
                                    arg == "--ready-timeout-ms" ? 180000 : 30000);
            if (!parsed) return q2dz::Result<Options>::fail(arg + ": " + parsed.error().message);
            if (arg == "--ready-timeout-ms") out.ready_timeout_ms = parsed.value();
            else out.shutdown_grace_ms = parsed.value();
        } else if (arg == "--help") { usage(std::cout); std::exit(0); }
        else return q2dz::Result<Options>::fail("unknown option: " + arg);
    }
    if (out.gateway.empty() || out.legacy_gateway.empty() || out.adapter.empty()) {
        return q2dz::Result<Options>::fail("--gateway, --legacy-gateway and --adapter are required");
    }
    if (out.game != "3zb2") return q2dz::Result<Options>::fail("--game must be 3zb2");
    return q2dz::Result<Options>::ok(std::move(out));
}

class RoomsCommandReader {
public:
    q2dz::Result<std::vector<q2dz::HubCommand>> read(int fd, bool& eof) {
        std::vector<q2dz::HubCommand> commands;
        if (discarding_) {
            if (auto discarded = discard_recovery_remainder(fd, eof); !discarded) {
                return q2dz::Result<std::vector<q2dz::HubCommand>>::fail(discarded.error().message);
            }
            if (discarding_) return q2dz::Result<std::vector<q2dz::HubCommand>>::ok(std::move(commands));
        }
        if (auto parsed = parse_queued_commands(); !parsed) {
            return q2dz::Result<std::vector<q2dz::HubCommand>>::fail(parsed.error().message);
        }
        take_commands(commands);
        if (!commands.empty()) return q2dz::Result<std::vector<q2dz::HubCommand>>::ok(std::move(commands));
        if (eof) {
            if (!framer_.finish()) {
                return q2dz::Result<std::vector<q2dz::HubCommand>>::fail("hub input ended after a partial command");
            }
            if (auto parsed = parse_queued_commands(); !parsed) {
                return q2dz::Result<std::vector<q2dz::HubCommand>>::fail(parsed.error().message);
            }
            take_commands(commands);
            return q2dz::Result<std::vector<q2dz::HubCommand>>::ok(std::move(commands));
        }

        // One bounded read and one bounded parse batch per coordinator cycle.
        // The next cycle drains any remaining framed lines before reading
        // more stdin, so a continuously fed launcher cannot starve gateway
        // events, timeouts, or the Q2 child.
        std::array<char, kMaxReadBytesPerCycle> data{};
        const ssize_t count = ::read(fd, data.data(), data.size());
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                return q2dz::Result<std::vector<q2dz::HubCommand>>::ok(std::move(commands));
            }
            return q2dz::Result<std::vector<q2dz::HubCommand>>::fail(q2dz::errno_message("read hub input"));
        }
        if (count == 0) {
            eof = true;
            if (!framer_.finish()) {
                return q2dz::Result<std::vector<q2dz::HubCommand>>::fail("hub input ended after a partial command");
            }
        } else if (!framer_.feed(std::string_view(data.data(), static_cast<std::size_t>(count)))) {
            return q2dz::Result<std::vector<q2dz::HubCommand>>::fail("hub command framing is invalid");
        }
        if (framer_.pending_byte_count() + framer_.queued_byte_count() > kMaxBufferedBytes) {
            return q2dz::Result<std::vector<q2dz::HubCommand>>::fail("hub command backlog exceeds 512 KiB");
        }
        if (auto parsed = parse_queued_commands(); !parsed) {
            return q2dz::Result<std::vector<q2dz::HubCommand>>::fail(parsed.error().message);
        }
        take_commands(commands);
        return q2dz::Result<std::vector<q2dz::HubCommand>>::ok(std::move(commands));
    }

    void begin_gateway_recovery() noexcept {
        clear_pending_commands();
        const auto pending_bytes = framer_.pending_byte_count();
        framer_.clear();
        parser_ = q2dz::HubCommandParser(q2dz::HubCommandChannel::rooms);
        if (!discarding_) discarded_bytes_ = pending_bytes;
        discarding_ = discarding_ || pending_bytes != 0;
    }

    [[nodiscard]] bool needs_service() const noexcept {
        return discarding_ || !commands_.empty() || framer_.queued_line_count() != 0;
    }

private:
    static constexpr std::size_t kMaxReadBytesPerCycle = 64U * 1024U;
    static constexpr std::size_t kMaxCommandsPerCycle = 32U;
    static constexpr std::size_t kMaxBufferedCommands = 512U;
    static constexpr std::size_t kMaxBufferedBytes = 512U * 1024U;

    static std::size_t command_bytes(const q2dz::HubCommand& command) noexcept {
        return command.room_id.size() + command.json.size();
    }

    void clear_pending_commands() noexcept {
        for (auto& command : commands_) command.clear();
        commands_.clear();
        parsed_bytes_ = 0;
    }

    void take_commands(std::vector<q2dz::HubCommand>& commands) {
        while (!commands_.empty() && commands.size() < kMaxCommandsPerCycle) {
            const auto bytes = command_bytes(commands_.front());
            commands.push_back(std::move(commands_.front()));
            commands_.pop_front();
            parsed_bytes_ -= bytes;
        }
    }

    q2dz::Result<void> parse_queued_commands() {
        if (framer_.queued_line_count() > kMaxBufferedCommands ||
            framer_.queued_byte_count() > kMaxBufferedBytes) {
            clear_pending_commands();
            framer_.clear();
            return q2dz::Result<void>::fail("hub command backlog exceeds 512 KiB");
        }
        for (auto lines = framer_.drain(); !lines.empty(); lines.pop_front()) {
            auto parsed = parser_.parse(lines.front().view());
            if (!parsed) {
                clear_pending_commands();
                return q2dz::Result<void>::fail(parsed.error().message);
            }
            const auto bytes = command_bytes(parsed.value());
            if (commands_.size() >= kMaxBufferedCommands ||
                parsed_bytes_ > kMaxBufferedBytes ||
                bytes > kMaxBufferedBytes - parsed_bytes_) {
                clear_pending_commands();
                return q2dz::Result<void>::fail("hub command backlog exceeds 512 KiB");
            }
            parsed_bytes_ += bytes;
            commands_.push_back(std::move(parsed.value()));
        }
        return q2dz::Result<void>::ok();
    }

    q2dz::Result<void> discard_recovery_remainder(int fd, bool& eof) {
        std::array<char, kMaxReadBytesPerCycle> data{};
        const ssize_t count = ::read(fd, data.data(), data.size());
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                return q2dz::Result<void>::ok();
            }
            return q2dz::Result<void>::fail(q2dz::errno_message("read recovery remainder"));
        }
        if (count == 0) {
            eof = true;
            return q2dz::Result<void>::fail("hub input ended while discarding recovery remainder");
        }
        std::string_view bytes(data.data(), static_cast<std::size_t>(count));
        const auto newline = bytes.find('\n');
        if (newline == std::string_view::npos) {
            if (discarded_bytes_ > q2dz::HubCommandFramer::max_line_bytes ||
                bytes.size() > q2dz::HubCommandFramer::max_line_bytes - discarded_bytes_) {
                return q2dz::Result<void>::fail("hub recovery remainder exceeds 256 KiB");
            }
            discarded_bytes_ += bytes.size();
            return q2dz::Result<void>::ok();
        }
        if (discarded_bytes_ > q2dz::HubCommandFramer::max_line_bytes ||
            newline > q2dz::HubCommandFramer::max_line_bytes - discarded_bytes_) {
            return q2dz::Result<void>::fail("hub recovery remainder exceeds 256 KiB");
        }
        bytes.remove_prefix(newline + 1U);
        discarded_bytes_ = 0;
        discarding_ = false;
        if (!bytes.empty() && !framer_.feed(bytes)) {
            return q2dz::Result<void>::fail("hub command framing is invalid");
        }
        return q2dz::Result<void>::ok();
    }

public:
    ~RoomsCommandReader() = default;

private:
    q2dz::HubCommandFramer framer_;
    q2dz::HubCommandParser parser_{q2dz::HubCommandChannel::rooms};
    std::deque<q2dz::HubCommand> commands_;
    std::size_t parsed_bytes_ = 0;
    std::size_t discarded_bytes_ = 0;
    bool discarding_ = false;
};

fs::path default_runtime_base() {
    if (const char* value = std::getenv("XDG_RUNTIME_DIR"); value && *value) return fs::path(value) / "q2dz";
    return fs::path("/tmp") / ("q2dz-" + std::to_string(::getuid()));
}

q2dz::Result<fs::path> prepare_runtime_base(const std::string& configured) {
    fs::path base = configured.empty() ? default_runtime_base() : fs::path(configured);
    std::error_code ec;
    fs::create_directories(base, ec);
    if (ec) return q2dz::Result<fs::path>::fail("cannot create runtime base: " + ec.message());
    if (::chmod(base.c_str(), 0700) != 0) return q2dz::Result<fs::path>::fail(q2dz::errno_message("chmod runtime base"));
    return q2dz::canonical_owned_directory(base);
}

q2dz::Result<UniqueFd> acquire_global_lock(const fs::path& base) {
    UniqueFd fd(::open((base / "owner.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!fd) return q2dz::Result<UniqueFd>::fail(q2dz::errno_message("open owner lock"));
    if (::flock(fd.get(), LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK) return q2dz::Result<UniqueFd>::fail("another Dos.Zone join session is already active");
        return q2dz::Result<UniqueFd>::fail(q2dz::errno_message("flock owner lock"));
    }
    return q2dz::Result<UniqueFd>::ok(std::move(fd));
}

void cleanup_stale_sessions(const fs::path& base) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(base, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        if (!entry.is_directory(ec) || !entry.path().filename().string().starts_with("session-")) { ec.clear(); continue; }
        auto marker = q2dz::read_file(entry.path() / ".q2dz-session", 256);
        auto owner = q2dz::read_file(entry.path() / "owner", 256);
        if (!marker || !owner) continue;
        pid_t pid = -1;
        std::uint64_t start = 0;
        std::istringstream in(owner.value());
        in >> pid >> start;
        if (pid > 0 && q2dz::process_instance_alive(pid, start)) continue;
        (void)q2dz::remove_session_dir_safely(base, entry.path(), marker.value());
    }
}

q2dz::Result<fs::path> create_session_dir(const fs::path& base, const std::string& session_id) {
    const fs::path path = base / ("session-" + session_id);
    if (::mkdir(path.c_str(), 0700) != 0) return q2dz::Result<fs::path>::fail(q2dz::errno_message("mkdir session"));
    if (auto result = q2dz::write_private_file(path / ".q2dz-session", session_id); !result) return q2dz::Result<fs::path>::fail(result.error().message);
    const auto start = q2dz::proc_start_time(::getpid());
    if (!start) return q2dz::Result<fs::path>::fail("cannot read owner process start time");
    if (auto result = q2dz::write_private_file(path / "owner", std::to_string(::getpid()) + " " + std::to_string(*start)); !result) {
        return q2dz::Result<fs::path>::fail(result.error().message);
    }
    std::error_code ec;
    fs::create_directory(path / "userdata", ec);
    if (ec || ::chmod((path / "userdata").c_str(), 0700) != 0) return q2dz::Result<fs::path>::fail("cannot create session userdata");
    return q2dz::Result<fs::path>::ok(path);
}

q2dz::Result<fs::path> resolve_program(std::string_view configured, std::initializer_list<std::string_view> candidates) {
    if (!configured.empty()) {
        auto found = q2dz::find_executable(configured);
        if (!found) return q2dz::Result<fs::path>::fail("executable not found: " + std::string(configured));
        return q2dz::Result<fs::path>::ok(*found);
    }
    for (const auto candidate : candidates) if (auto found = q2dz::find_executable(candidate)) return q2dz::Result<fs::path>::ok(*found);
    return q2dz::Result<fs::path>::fail("required executable not found in PATH");
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

q2dz::Result<fs::path> discover_q2(const Options& options) {
    if (!options.q2.empty()) {
        fs::path path(options.q2);
        if (!fs::is_regular_file(path)) return q2dz::Result<fs::path>::fail("Q2 executable does not exist: " + path.string());
        return q2dz::Result<fs::path>::ok(fs::absolute(path));
    }
    if (const char* env = std::getenv("Q2PROX_EXE"); env && *env) {
        fs::path path(env);
        if (fs::is_regular_file(path)) return q2dz::Result<fs::path>::ok(fs::absolute(path));
    }
    fs::path prefix;
    if (!options.wineprefix.empty()) prefix = options.wineprefix;
    else if (const char* env = std::getenv("WINEPREFIX"); env && *env) prefix = env;
    else if (const char* home = std::getenv("HOME"); home && *home) prefix = fs::path(home) / ".wine";
    if (prefix.empty() || !fs::is_directory(prefix / "drive_c")) {
        return q2dz::Result<fs::path>::fail("Q2PRO-X.exe not specified and Wine prefix cannot be searched");
    }
    const std::set<std::string> names = {"q2pro-x.exe", "q2prox.exe", "q2pro.exe", "q2pro64.exe"};
    std::vector<fs::path> matches;
    std::error_code ec;
    std::size_t visited = 0;
    for (fs::recursive_directory_iterator it(prefix / "drive_c", fs::directory_options::skip_permission_denied, ec), end;
         it != end && visited < 200000; it.increment(ec), ++visited) {
        if (ec) { ec.clear(); continue; }
        if (it.depth() > 10) { it.disable_recursion_pending(); continue; }
        if (!it->is_regular_file(ec)) { ec.clear(); continue; }
        if (names.contains(lower(it->path().filename().string()))) matches.push_back(it->path());
    }
    if (matches.empty()) return q2dz::Result<fs::path>::fail("Q2PRO-X.exe was not found; pass --q2 explicitly");
    if (matches.size() > 1) {
        std::string message = "multiple Q2 executables found; pass --q2 explicitly:";
        for (const auto& path : matches) message += "\n  " + path.string();
        return q2dz::Result<fs::path>::fail(std::move(message));
    }
    return q2dz::Result<fs::path>::ok(fs::absolute(matches.front()));
}

struct Pipes {
    UniqueFd read;
    UniqueFd write;
};

q2dz::Result<Pipes> make_pipe(bool nonblocking_read = false) {
    int fds[2]{};
    int flags = O_CLOEXEC;
    if (nonblocking_read) flags |= O_NONBLOCK;
    if (::pipe2(fds, flags) != 0) return q2dz::Result<Pipes>::fail(q2dz::errno_message("pipe2"));
    return q2dz::Result<Pipes>::ok(Pipes{UniqueFd(fds[0]), UniqueFd(fds[1])});
}

struct SourceTarget {
    std::string room_id;
    std::uint64_t target_id = 0;
    q2dz::Endpoint endpoint;
};

struct SourceSnapshot {
    std::string servers;
    std::string rooms;
};

class CustomSource {
public:
    q2dz::Result<void> start(std::string session) {
        if (worker_.joinable()) return q2dz::Result<void>::fail("custom source already started");
        UniqueFd listener(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
        if (!listener) return q2dz::Result<void>::fail(q2dz::errno_message("custom source socket"));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(0);
        if (::bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            return q2dz::Result<void>::fail(q2dz::errno_message("custom source bind"));
        }
        if (::listen(listener.get(), 4) != 0) {
            return q2dz::Result<void>::fail(q2dz::errno_message("custom source listen"));
        }
        socklen_t address_length = sizeof(address);
        if (::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &address_length) != 0 || ntohs(address.sin_port) < 1024) {
            return q2dz::Result<void>::fail(q2dz::errno_message("custom source getsockname"));
        }
        int stop_fds[2]{};
        if (::pipe2(stop_fds, O_CLOEXEC | O_NONBLOCK) != 0) {
            return q2dz::Result<void>::fail(q2dz::errno_message("custom source stop pipe"));
        }
        listener_ = std::move(listener);
        stop_read_.reset(stop_fds[0]);
        stop_write_.reset(stop_fds[1]);
        listener_port_ = ntohs(address.sin_port);
        session_ = std::move(session);
        url_ = "http://127.0.0.1:" + std::to_string(listener_port_) + "/servers";
        try {
            worker_ = std::thread([this] { serve(); });
        } catch (const std::exception& error) {
            listener_.reset();
            stop_read_.reset();
            stop_write_.reset();
            url_.clear();
            session_.clear();
            listener_port_ = 0;
            return q2dz::Result<void>::fail(std::string("custom source thread: ") + error.what());
        }
        return q2dz::Result<void>::ok();
    }

    [[nodiscard]] std::string url() const { return url_; }

    void publish(const std::vector<SourceTarget>& targets) {
        if (targets.size() > 256U) {
            failed_.store(true);
            return;
        }
        SourceSnapshot snapshot;
        q2dz::json::Array rooms;
        rooms.reserve(targets.size());
        for (const auto& target : targets) {
            snapshot.servers += target.endpoint.to_string();
            snapshot.servers.push_back('\n');
            q2dz::json::Object room;
            room.emplace("endpoint", q2dz::json::Value{target.endpoint.to_string()});
            room.emplace("kind", q2dz::json::Value{
                target.room_id.starts_with("room.q2ded.") ? "q2ded" : "q2"});
            room.emplace("room", q2dz::json::Value{target.room_id});
            room.emplace("target", q2dz::json::Value{std::to_string(target.target_id)});
            rooms.emplace_back(q2dz::json::Value{std::move(room)});
        }
        q2dz::json::Object root;
        root.emplace("session", q2dz::json::Value{session_});
        root.emplace("targets", q2dz::json::Value{std::move(rooms)});
        root.emplace("version", q2dz::json::Value{1.0});
        snapshot.rooms = q2dz::json::stringify(q2dz::json::Value{std::move(root)});
        if (snapshot.rooms.size() > 256U * 1024U) {
            failed_.store(true);
            return;
        }
        std::lock_guard lock(mutex_);
        snapshot_ = std::move(snapshot);
    }

    [[nodiscard]] bool failed() const noexcept { return failed_.load(); }

    void stop() noexcept {
        if (!worker_.joinable()) {
            listener_.reset();
            stop_read_.reset();
            stop_write_.reset();
            session_.clear();
            listener_port_ = 0;
            return;
        }
        const unsigned char byte = 1;
        (void)::write(stop_write_.get(), &byte, sizeof(byte));
        worker_.join();
        listener_.reset();
        stop_read_.reset();
        stop_write_.reset();
        session_.clear();
        listener_port_ = 0;
    }

    ~CustomSource() { stop(); }
    CustomSource() = default;
    CustomSource(const CustomSource&) = delete;
    CustomSource& operator=(const CustomSource&) = delete;

private:
    static std::size_t header_end(std::string_view request) {
        const auto crlf = request.find("\r\n\r\n");
        if (crlf != std::string_view::npos) return crlf + 4U;
        const auto lf = request.find("\n\n");
        return lf == std::string_view::npos ? std::string_view::npos : lf + 2U;
    }

    bool send_all(int client, std::string_view response) {
        std::size_t offset = 0;
        const auto deadline = q2dz::monotonic_millis() + 1000U;
        while (offset < response.size()) {
            const ssize_t count = ::send(client, response.data() + offset, response.size() - offset, MSG_NOSIGNAL);
            if (count > 0) {
                offset += static_cast<std::size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                const auto now = q2dz::monotonic_millis();
                if (now >= deadline) return false;
                pollfd fd{client, POLLOUT, 0};
                (void)::poll(&fd, 1, static_cast<int>(deadline - now));
                continue;
            }
            return false;
        }
        return true;
    }

    bool serve_client(int client) {
        UniqueFd owned(client);
        std::string request;
        const auto deadline = q2dz::monotonic_millis() + 1000U;
        while (header_end(request) == std::string_view::npos) {
            const auto now = q2dz::monotonic_millis();
            if (now >= deadline) return true;
            pollfd fds[2] = {{stop_read_.get(), POLLIN | POLLHUP, 0}, {owned.get(), POLLIN | POLLHUP, 0}};
            const int timeout = static_cast<int>(deadline - now);
            const int result = ::poll(fds, 2, timeout);
            if (result < 0) {
                if (errno == EINTR) continue;
                failed_.store(true);
                return false;
            }
            if ((fds[0].revents & (POLLIN | POLLHUP)) != 0) return false;
            if ((fds[1].revents & (POLLIN | POLLHUP)) == 0) continue;
            std::array<char, 2048> buffer{};
            const ssize_t count = ::recv(owned.get(), buffer.data(), buffer.size(), 0);
            if (count < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                return true;
            }
            if (count == 0) return true;
            request.append(buffer.data(), static_cast<std::size_t>(count));
            if (request.size() > 8U * 1024U) return true;
        }

        const auto first_line_end = request.find('\n');
        std::string_view first_line(request.data(), first_line_end == std::string::npos ? request.size() : first_line_end);
        if (!first_line.empty() && first_line.back() == '\r') first_line.remove_suffix(1);
        bool has_host = false;
        bool exact_host = false;
        bool has_origin = false;
        bool headers_valid = true;
        const auto request_end = header_end(request);
        if (first_line_end != std::string::npos && request_end != std::string_view::npos) {
            const std::string expected_host = "127.0.0.1:" + std::to_string(listener_port_);
            const auto trim_ows = [](std::string_view value) {
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1U);
                while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1U);
                return value;
            };
            std::size_t cursor = first_line_end + 1U;
            while (cursor < request_end) {
                const auto line_end = request.find('\n', cursor);
                if (line_end == std::string::npos || line_end >= request_end) break;
                std::string_view line(request.data() + cursor, line_end - cursor);
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                cursor = line_end + 1U;
                if (line.empty()) break;
                if (line.front() == ' ' || line.front() == '\t') {
                    headers_valid = false;
                    continue;
                }
                const auto colon = line.find(':');
                if (colon == std::string_view::npos || colon == 0U) {
                    headers_valid = false;
                    continue;
                }
                std::string name(line.substr(0, colon));
                if (!std::all_of(name.begin(), name.end(), [](unsigned char c) {
                        return std::isalnum(c) || c == '-';
                    })) {
                    headers_valid = false;
                    continue;
                }
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                const auto value = line.substr(colon + 1U);
                if (!std::all_of(value.begin(), value.end(), [](unsigned char c) {
                        return c == '\t' || (c >= 0x20U && c != 0x7fU);
                    })) {
                    headers_valid = false;
                    continue;
                }
                if (name == "origin") has_origin = true;
                if (name == "host") {
                    exact_host = !has_host && trim_ows(value) == expected_host;
                    has_host = true;
                }
            }
        }
        std::string response;
        const bool rooms_route = first_line == "GET /q2dz/rooms HTTP/1.1";
        const bool servers_route = first_line == "GET /servers HTTP/1.1";
        if (servers_route || (rooms_route && headers_valid && has_host && exact_host && !has_origin)) {
            SourceSnapshot snapshot;
            {
                std::lock_guard lock(mutex_);
                snapshot = snapshot_;
            }
            const auto& body = rooms_route ? snapshot.rooms : snapshot.servers;
            const char* content_type = rooms_route ? "application/json; charset=utf-8" : "text/plain; charset=utf-8";
            response = "HTTP/1.1 200 OK\r\nContent-Type: " + std::string(content_type) + "\r\n"
                       "Cache-Control: no-store\r\nContent-Length: " + std::to_string(body.size()) +
                       "\r\nConnection: close\r\n\r\n" + body;
        } else {
            response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        }
        (void)send_all(owned.get(), response);
        return true;
    }

    void serve() noexcept {
        while (true) {
            pollfd fds[2] = {{stop_read_.get(), POLLIN | POLLHUP, 0}, {listener_.get(), POLLIN | POLLERR | POLLHUP, 0}};
            const int result = ::poll(fds, 2, -1);
            if (result < 0) {
                if (errno == EINTR) continue;
                failed_.store(true);
                break;
            }
            if ((fds[0].revents & (POLLIN | POLLHUP)) != 0) break;
            if ((fds[1].revents & (POLLERR | POLLHUP)) != 0) {
                failed_.store(true);
                break;
            }
            if ((fds[1].revents & POLLIN) == 0) continue;
            const int client = ::accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (client < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                failed_.store(true);
                break;
            }
            if (!serve_client(client)) break;
        }
    }

    UniqueFd listener_;
    UniqueFd stop_read_;
    UniqueFd stop_write_;
    std::thread worker_;
    mutable std::mutex mutex_;
    SourceSnapshot snapshot_;
    std::string url_;
    std::string session_;
    std::uint16_t listener_port_ = 0;
    std::atomic<bool> failed_{false};
};

struct Guardian {
    pid_t pid = -1;
    UniqueFd owner;
    UniqueFd control;
};

q2dz::Result<Guardian> start_guardian(const fs::path& self,
                                      const fs::path& base,
                                      const fs::path& session_dir,
                                      const std::string& session_id,
                                      int grace_ms) {
    auto owner = make_pipe(false);
    auto control = make_pipe(false);
    if (!owner) return q2dz::Result<Guardian>::fail(owner.error().message);
    if (!control) return q2dz::Result<Guardian>::fail(control.error().message);
    if (auto r = q2dz::set_cloexec(owner.value().read.get(), false); !r) return q2dz::Result<Guardian>::fail(r.error().message);
    if (auto r = q2dz::set_cloexec(control.value().read.get(), false); !r) return q2dz::Result<Guardian>::fail(r.error().message);
    const pid_t pid = ::fork();
    if (pid < 0) return q2dz::Result<Guardian>::fail(q2dz::errno_message("fork guardian"));
    if (pid == 0) {
        owner.value().write.reset();
        control.value().write.reset();
        std::vector<std::string> storage = {
            self.string(), "__guardian",
            "--owner-fd", std::to_string(owner.value().read.get()),
            "--control-fd", std::to_string(control.value().read.get()),
            "--base", base.string(),
            "--session-dir", session_dir.string(),
            "--session-id", session_id,
            "--uid", std::to_string(::getuid()),
            "--owner-pid", std::to_string(::getppid()),
            "--grace-ms", std::to_string(grace_ms)
        };
        std::vector<char*> argv;
        for (auto& item : storage) argv.push_back(item.data());
        argv.push_back(nullptr);
        ::execv(self.c_str(), argv.data());
        _exit(127);
    }
    owner.value().read.reset();
    control.value().read.reset();
    return q2dz::Result<Guardian>::ok(Guardian{pid, std::move(owner.value().write), std::move(control.value().write)});
}

struct Child {
    pid_t pid = -1;
    pid_t process_group = -1;
    UniqueFd input;
    UniqueFd control;
    UniqueFd diagnostics;
};

q2dz::Result<Child> spawn_gateway(const fs::path& gateway,
                                  const fs::path& session_dir,
                                  const std::string& session_id) {
    auto input = make_pipe(false);
    auto control = make_pipe(true);
    auto diagnostics = make_pipe(true);
    if (!input) return q2dz::Result<Child>::fail(input.error().message);
    if (!control) return q2dz::Result<Child>::fail(control.error().message);
    if (!diagnostics) return q2dz::Result<Child>::fail(diagnostics.error().message);
    const pid_t pid = ::fork();
    if (pid < 0) return q2dz::Result<Child>::fail(q2dz::errno_message("fork gateway"));
    if (pid == 0) {
        (void)::prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
        (void)::setpgid(0, 0);
        if (::dup2(input.value().read.get(), STDIN_FILENO) < 0 ||
            ::dup2(control.value().write.get(), STDOUT_FILENO) < 0 ||
            ::dup2(diagnostics.value().write.get(), STDERR_FILENO) < 0) _exit(126);
        std::vector<std::pair<std::string,std::string>> overrides = {
            {"Q2DZ_SESSION_ID", session_id},
            {"Q2DZ_RUNTIME_DIR", session_dir.string()}
        };
        auto env_storage = q2dz::current_environment_with(overrides);
        std::vector<char*> envp;
        for (auto& item : env_storage) envp.push_back(item.data());
        envp.push_back(nullptr);
        std::vector<std::string> storage = {gateway.string()};
        std::vector<char*> argv;
        for (auto& item : storage) argv.push_back(item.data());
        argv.push_back(nullptr);
        ::execve(gateway.c_str(), argv.data(), envp.data());
        _exit(127);
    }
    (void)::setpgid(pid, pid);
    input.value().read.reset();
    control.value().write.reset();
    diagnostics.value().write.reset();
    return q2dz::Result<Child>::ok(Child{pid, pid, std::move(input.value().write),
                                         std::move(control.value().read),
                                         std::move(diagnostics.value().read)});
}

q2dz::Result<pid_t> spawn_q2_live(const fs::path& wine,
                                  const fs::path& q2,
                                  std::string_view source_url,
                                  const Options& options,
                                  const std::string& session_id) {
    const pid_t pid = ::fork();
    if (pid < 0) return q2dz::Result<pid_t>::fail(q2dz::errno_message("fork Q2PRO-X"));
    if (pid == 0) {
        (void)::prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
        if (::setpgid(0, 0) != 0) _exit(126);
        if (::chdir(q2.parent_path().c_str()) != 0) _exit(126);
        UniqueFd devnull(::open("/dev/null", O_RDONLY | O_CLOEXEC));
        if (!devnull || ::dup2(devnull.get(), STDIN_FILENO) < 0) _exit(126);
        std::vector<std::pair<std::string, std::string>> overrides = { {"Q2DZ_SESSION_ID", session_id} };
        if (!options.wineprefix.empty()) overrides.emplace_back("WINEPREFIX", options.wineprefix);
        auto env_storage = q2dz::current_environment_with(overrides);
        std::vector<char*> envp;
        for (auto& item : env_storage) envp.push_back(item.data());
        envp.push_back(nullptr);
        std::vector<std::string> storage = {
            wine.string(), q2.string(),
            "+set", "game", options.game,
            "+set", "cl_timeout", "20",
            "+set", "cl_serverbrowser_custom_source", std::string(source_url)
        };
        std::vector<char*> argv;
        for (auto& item : storage) argv.push_back(item.data());
        argv.push_back(nullptr);
        ::execve(wine.c_str(), argv.data(), envp.data());
        _exit(127);
    }
    if (::setpgid(pid, pid) != 0 && errno != EACCES) {
        (void)::kill(pid, SIGKILL);
        return q2dz::Result<pid_t>::fail(q2dz::errno_message("setpgid Q2PRO-X"));
    }
    return q2dz::Result<pid_t>::ok(pid);
}

constexpr std::size_t kMaxGatewayReadBytesPerCycle = 64U * 1024U;
constexpr std::size_t kMaxGatewayEventsPerCycle = 64U;
constexpr std::size_t kMaxGatewayBufferedBytes = 512U * 1024U;

void drain_diagnostics(int fd, std::string& buffer) {
    std::array<char, kMaxGatewayReadBytesPerCycle> temp{};
    ssize_t n = 0;
    do {
        n = ::read(fd, temp.data(), temp.size());
    } while (n < 0 && errno == EINTR);
    if (n <= 0) return;
    buffer.append(temp.data(), static_cast<std::size_t>(n));
    while (true) {
        const auto newline = buffer.find('\n');
        if (newline == std::string::npos) break;
        emit_event(q2dz::sanitize_log_line(std::string_view(buffer).substr(0, newline)));
        buffer.erase(0, newline + 1);
    }
    if (buffer.size() > 64U * 1024U) buffer.erase(0, buffer.size() - 4096U);
}

std::vector<std::string> take_lines(int fd, std::string& buffer, bool& eof, bool& overflow) {
    std::vector<std::string> out;
    out.reserve(kMaxGatewayEventsPerCycle);
    overflow = false;
    const auto extract_lines = [&]() {
        while (out.size() < kMaxGatewayEventsPerCycle) {
            const auto newline = buffer.find('\n');
            if (newline == std::string::npos) break;
            out.push_back(buffer.substr(0, newline + 1U));
            buffer.erase(0, newline + 1U);
        }
    };

    extract_lines();
    if (out.size() == kMaxGatewayEventsPerCycle || eof) return out;

    std::array<char, kMaxGatewayReadBytesPerCycle> temp{};
    while (true) {
        const ssize_t n = ::read(fd, temp.data(), temp.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            eof = true;
            break;
        }
        if (n == 0) { eof = true; break; }
        buffer.append(temp.data(), static_cast<std::size_t>(n));
        if (buffer.size() > kMaxGatewayBufferedBytes) {
            q2dz::secure_clear(buffer);
            overflow = true;
            return {};
        }
        extract_lines();
        break;
    }
    return out;
}

void terminate_group(pid_t pgid, int grace_ms, pid_t gateway_pid, pid_t q2_pid) {
    if (pgid > 1) (void)::kill(-pgid, SIGTERM);
    const auto deadline = q2dz::monotonic_millis() + static_cast<std::uint64_t>(grace_ms);
    bool gateway_alive = gateway_pid > 0;
    bool q2_alive = q2_pid > 0;
    while (q2dz::monotonic_millis() < deadline && (gateway_alive || q2_alive)) {
        int status = 0;
        if (gateway_alive) {
            const pid_t result = ::waitpid(gateway_pid, &status, WNOHANG);
            if (result == gateway_pid || (result < 0 && errno == ECHILD)) gateway_alive = false;
        }
        if (q2_alive) {
            const pid_t result = ::waitpid(q2_pid, &status, WNOHANG);
            if (result == q2_pid || (result < 0 && errno == ECHILD)) q2_alive = false;
        }
        if (gateway_alive || q2_alive) ::usleep(20000);
    }
    if (pgid > 1) (void)::kill(-pgid, SIGKILL);
    int status = 0;
    if (gateway_alive) (void)::waitpid(gateway_pid, &status, 0);
    if (q2_alive) (void)::waitpid(q2_pid, &status, 0);
}

struct LiveGateway {
    Child child;
    std::string control_buffer;
    std::string diagnostic_buffer;
    bool control_eof = false;
    q2dz::GatewayEventParser event_parser;
};

struct LiveTarget {
    std::string room_id;
    std::uint64_t target_id = 0;
    std::optional<q2dz::Endpoint> endpoint;
    std::uint64_t ready_deadline = 0;
    unsigned recovery_count = 0;
    std::string terminal_reason;
    bool ready = false;
    bool tombstone = false;
    bool closed = false;
    bool retryable_failure = false;
};

using LiveTargets = std::unordered_map<std::string, LiveTarget>;

bool is_dedicated_room(std::string_view room_id) {
    return room_id.starts_with("room.q2ded.");
}

void emit_target(const LiveTarget& target, std::string_view state,
                 std::string_view reason, std::string_view retry,
                 std::size_t published_ready) {
    emit_event("TARGET room=" + log_value(target.room_id, 255) +
               " id=" + std::to_string(target.target_id) +
               " state=" + std::string(state) + " reason=" + log_value(reason) +
               " recovery=" + std::to_string(target.recovery_count) +
               " retry=" + std::string(retry) +
               " published_ready=" + std::to_string(published_ready));
}

unsigned target_recovery_limit(const LiveTarget& target) {
    // Keep the former per-slot policy: q2 may be recreated once after a
    // post-ready failure, while q2ded has two bounded recovery starts.  The
    // count is carried by room_id across target-id replacement.
    return is_dedicated_room(target.room_id) ? 2U : 1U;
}

std::string_view retry_decision(const LiveTarget& target) {
    const bool dedicated = is_dedicated_room(target.room_id);
    if ((!dedicated && (target.closed || !target.retryable_failure)) ||
        target.recovery_count >= target_recovery_limit(target)) {
        return "none";
    }
    return "immediate";
}

void emit_gateway(std::string_view event, std::string_view reason, unsigned restarts) {
    emit_event("GATEWAY event=" + std::string(event) +
               " reason=" + log_value(reason) +
               " restarts=" + std::to_string(restarts));
}

LiveTarget* find_target(LiveTargets& targets, std::uint64_t target_id) {
    for (auto& [unused, target] : targets) {
        static_cast<void>(unused);
        if (target.target_id == target_id) return &target;
    }
    return nullptr;
}

q2dz::Result<void> send_hub_command(LiveGateway& gateway, const q2dz::HubCommand& command) {
    std::string line = q2dz::format_hub_command(command);
    if (line.empty()) return q2dz::Result<void>::fail("cannot format HUB/2 command");
    line.push_back('\n');
    auto result = q2dz::write_all(gateway.child.input.get(), line);
    q2dz::secure_clear(line);
    return result;
}

q2dz::Result<void> send_hub_drop(LiveGateway& gateway, std::uint64_t target_id) {
    q2dz::HubCommand command;
    command.type = q2dz::HubCommandType::drop;
    command.id_or_generation = target_id;
    return send_hub_command(gateway, command);
}

void stop_live_gateway(LiveGateway& gateway,
                       const Options& options,
                       Guardian& guardian,
                       std::string_view reason) {
    q2dz::HubCommand stop;
    stop.type = q2dz::HubCommandType::stop;
    (void)send_hub_command(gateway, stop);
    gateway.child.input.reset();
    const pid_t process_group = gateway.child.process_group;
    if (gateway.child.pid > 0 || process_group > 1) {
        terminate_group(process_group, options.shutdown_grace_ms, gateway.child.pid, -1);
        gateway.child.pid = -1;
    }
    if (process_group > 1) {
        (void)q2dz::write_all(guardian.control.get(), "DROP " + std::to_string(process_group) + "\n");
    }
    if (gateway.child.diagnostics) drain_diagnostics(gateway.child.diagnostics.get(), gateway.diagnostic_buffer);
    gateway.child.control.reset();
    gateway.child.diagnostics.reset();
    gateway.child.input.reset();
    emit_event("HUB_GATEWAY_STOP reason=" + std::string(reason));
}

std::vector<SourceTarget> live_endpoints(const LiveTargets& targets) {
    std::vector<SourceTarget> ordered;
    ordered.reserve(targets.size());
    for (const auto& [room_id, target] : targets) {
        if (target.ready && target.endpoint) ordered.push_back(SourceTarget{room_id, target.target_id, *target.endpoint});
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        return left.room_id < right.room_id;
    });
    return ordered;
}

void publish_live_source(CustomSource& source, const LiveTargets& targets) {
    source.publish(live_endpoints(targets));
}

q2dz::Result<void> validate_live_room_config(std::string_view config) {
    auto parsed = q2dz::json::parse(config);
    if (!parsed || !parsed.value().is_object()) return q2dz::Result<void>::fail("room JSON must be an object");
    if (auto room = q2dz::json::extract_room_config(parsed.value()); !room) {
        return q2dz::Result<void>::fail(room.error().message);
    }
    return q2dz::Result<void>::ok();
}

q2dz::Result<void> start_shared_gateway(std::optional<LiveGateway>& gateway,
                                        const fs::path& gateway_path,
                                        const fs::path& session_dir,
                                        const std::string& session_id,
                                        const Options& options,
                                        Guardian& guardian) {
    auto child = spawn_gateway(gateway_path, session_dir, session_id);
    if (!child) return q2dz::Result<void>::fail(child.error().message);
    LiveGateway runtime;
    runtime.child = std::move(child.value());
    const pid_t process_group = runtime.child.process_group;
    gateway.emplace(std::move(runtime));
    if (!q2dz::write_all(guardian.control.get(), "PGID " + std::to_string(process_group) + "\n")) {
        stop_live_gateway(*gateway, options, guardian, "guardian-control-failed");
        gateway.reset();
        return q2dz::Result<void>::fail("guardian control pipe failed");
    }
    return q2dz::Result<void>::ok();
}

q2dz::Result<void> process_rooms_command(q2dz::HubCommand& command,
                                         LiveTargets& targets,
                                         std::optional<LiveGateway>& gateway,
                                         std::uint64_t& next_target_id,
                                         const Options& options,
                                         CustomSource& source,
                                         bool transport_disabled) {
    if (command.type == q2dz::HubCommandType::ice) {
        if (gateway && !transport_disabled) {
            q2dz::HubCommand forwarded;
            forwarded.type = q2dz::HubCommandType::ice;
            forwarded.id_or_generation = command.id_or_generation;
            forwarded.json = std::move(command.json);
            auto result = send_hub_command(*gateway, forwarded);
            forwarded.clear();
            command.clear();
            return result;
        }
        command.clear();
        return q2dz::Result<void>::ok();
    }
    if (command.type == q2dz::HubCommandType::drop) {
        const auto it = targets.find(command.room_id);
        if (it != targets.end()) {
            if (gateway && !transport_disabled) {
                auto result = send_hub_drop(*gateway, it->second.target_id);
                if (!result) {
                    command.clear();
                    return result;
                }
            }
            targets.erase(it);
            publish_live_source(source, targets);
        }
        command.clear();
        return q2dz::Result<void>::ok();
    }
    if (command.type != q2dz::HubCommandType::put) {
        command.clear();
        return q2dz::Result<void>::fail("unexpected room command");
    }

    const std::string requested_room = command.room_id;
    const std::string_view config(reinterpret_cast<const char*>(command.json.data()), command.json.size());
    if (auto valid = validate_live_room_config(config); !valid) {
        command.clear();
        return q2dz::Result<void>::ok();
    }
    if (!gateway || transport_disabled) {
        command.clear();
        return q2dz::Result<void>::ok();
    }

    // A repeated PUT is the room heartbeat. Do not replay fresh credentials
    // to an already active target; a terminal target may be replaced by the
    // next heartbeat with a new target ID.
    const auto current = targets.find(command.room_id);
    if (current != targets.end() && !current->second.tombstone) {
        command.clear();
        return q2dz::Result<void>::ok();
    }
    unsigned recovery_count = 0;
    bool retryable_failure = false;
    if (current != targets.end()) {
        const bool dedicated = is_dedicated_room(current->second.room_id);
        if ((!dedicated && (current->second.closed || !current->second.retryable_failure)) ||
            current->second.recovery_count >= target_recovery_limit(current->second)) {
            // A PUT against an exhausted tombstone stays ignored; log it so a
            // stuck room is attributable in the session log.
            emit_target(current->second, "terminal", current->second.terminal_reason,
                        "none", live_endpoints(targets).size());
            command.clear();
            return q2dz::Result<void>::ok();
        }
        recovery_count = current->second.recovery_count + 1U;
        retryable_failure = current->second.retryable_failure;
        targets.erase(current);
    }

    LiveTarget target;
    target.room_id = command.room_id;
    target.target_id = next_target_id++;
    target.ready_deadline = q2dz::monotonic_millis() + static_cast<std::uint64_t>(options.ready_timeout_ms);
    target.recovery_count = recovery_count;
    target.retryable_failure = retryable_failure;
    const auto target_id = target.target_id;
    targets.emplace(target.room_id, std::move(target));

    q2dz::HubCommand forwarded;
    forwarded.type = q2dz::HubCommandType::put;
    forwarded.id_or_generation = target_id;
    forwarded.json = std::move(command.json);
    auto result = send_hub_command(*gateway, forwarded);
    forwarded.clear();
    command.clear();
    if (!result) {
        targets.erase(requested_room);
        publish_live_source(source, targets);
        return result;
    }
    if (const auto inserted = targets.find(requested_room); inserted != targets.end()) {
        emit_target(inserted->second, "pending", "none",
                    recovery_count > 0 ? "immediate" : "none",
                    live_endpoints(targets).size());
    }
    return q2dz::Result<void>::ok();
}

void mark_target_terminal(LiveTarget& target, bool closed, std::string_view reason) {
    const bool was_ready = target.ready;
    target.endpoint.reset();
    target.ready = false;
    target.terminal_reason = log_value(reason);
    target.tombstone = true;
    target.closed = closed;
    target.retryable_failure = !closed && (was_ready || is_dedicated_room(target.room_id));
    if (!is_dedicated_room(target.room_id) && !target.retryable_failure) {
        // q2 never retries a target that did not reach READY, or a CLOSED
        // target. Mark its one recovery slot consumed.
        target.recovery_count = target_recovery_limit(target);
    }
}

q2dz::Result<void> process_shared_gateway(std::optional<LiveGateway>& gateway,
                                          LiveTargets& targets,
                                          CustomSource& source) {
    if (!gateway) return q2dz::Result<void>::ok();
    auto& runtime = *gateway;
    drain_diagnostics(runtime.child.diagnostics.get(), runtime.diagnostic_buffer);
    bool event_overflow = false;
    auto lines = take_lines(runtime.child.control.get(), runtime.control_buffer, runtime.control_eof, event_overflow);
    if (event_overflow) return q2dz::Result<void>::fail("gateway event backlog exceeds 512 KiB");
    for (auto& line : lines) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        auto event = runtime.event_parser.parse(line);
        q2dz::secure_clear(line);
        if (!event) {
            emit_gateway("fatal", "event_protocol", 0);
            return q2dz::Result<void>::fail("invalid-gateway-event: " + event.error().message);
        }
        const auto id_it = event.value().fields.find("id");
        std::uint64_t target_id = 0;
        if (id_it != event.value().fields.end()) {
            const auto parsed = std::from_chars(id_it->second.data(), id_it->second.data() + id_it->second.size(), target_id);
            if (parsed.ec != std::errc{} || parsed.ptr != id_it->second.data() + id_it->second.size()) {
                return q2dz::Result<void>::fail("invalid-gateway-event-id");
            }
        }
        if (event.value().type == q2dz::GatewayEventType::Heartbeat) continue;
        auto* target = find_target(targets, target_id);
        if (event.value().type == q2dz::GatewayEventType::TransportReady) {
            if (target == nullptr || target->tombstone) continue; // stale event after DROP or timeout
            if (target->ready) return q2dz::Result<void>::fail("duplicate-transport-ready");
            auto endpoint = q2dz::ready_endpoint(event.value());
            if (!endpoint) return q2dz::Result<void>::fail("invalid-ready-endpoint");
            target->endpoint = endpoint.value();
            target->ready = true;
            publish_live_source(source, targets);
            emit_target(*target, "ready", "none", "none", live_endpoints(targets).size());
        } else if (event.value().type == q2dz::GatewayEventType::RoomClosed) {
            if (target == nullptr) continue;
            const auto reason_field = event.value().fields.find("reason");
            mark_target_terminal(*target, true,
                                 safe_gateway_reason(reason_field != event.value().fields.end()
                                                         ? reason_field->second : "room_closed"));
            publish_live_source(source, targets);
            emit_target(*target, "terminal", target->terminal_reason,
                        retry_decision(*target), live_endpoints(targets).size());
        } else if (event.value().type == q2dz::GatewayEventType::TargetFailed) {
            if (target == nullptr) continue;
            const auto code_field = event.value().fields.find("code");
            mark_target_terminal(*target, false,
                                 safe_gateway_reason(code_field != event.value().fields.end()
                                                         ? code_field->second : "target_failed"));
            publish_live_source(source, targets);
            emit_target(*target, "terminal", target->terminal_reason,
                        retry_decision(*target), live_endpoints(targets).size());
        } else if (event.value().type == q2dz::GatewayEventType::Fatal) {
            const auto code_field = event.value().fields.find("code");
            const std::string code = code_field != event.value().fields.end() ? code_field->second : "fatal";
            emit_gateway("fatal", safe_gateway_reason(code), 0);
            return q2dz::Result<void>::fail("gateway-fatal: " + safe_gateway_reason(code));
        } else {
            emit_gateway("fatal", "event_unsupported", 0);
            return q2dz::Result<void>::fail("unsupported-gateway-event");
        }
    }
    if (runtime.control_eof) return q2dz::Result<void>::fail("gateway-control-eof");
    int status = 0;
    if (runtime.child.pid > 0) {
        const pid_t result = ::waitpid(runtime.child.pid, &status, WNOHANG);
        if (result == runtime.child.pid) {
            runtime.child.pid = -1;
            return q2dz::Result<void>::fail("gateway-exited");
        }
        if (result < 0 && errno != EINTR && errno != ECHILD) return q2dz::Result<void>::fail("gateway-wait-failed");
    }
    return q2dz::Result<void>::ok();
}

q2dz::Result<void> process_target_timeouts(LiveTargets& targets,
                                           std::optional<LiveGateway>& gateway,
                                           CustomSource& source) {
    if (!gateway) return q2dz::Result<void>::ok();
    const auto now = q2dz::monotonic_millis();
    std::vector<std::uint64_t> expired;
    for (const auto& [unused, target] : targets) {
        static_cast<void>(unused);
        if (!target.ready && !target.tombstone && target.ready_deadline <= now) expired.push_back(target.target_id);
    }
    for (const auto target_id : expired) {
        auto* target = find_target(targets, target_id);
        if (!target) continue;
        auto dropped = send_hub_drop(*gateway, target_id);
        if (!dropped) return dropped;
        mark_target_terminal(*target, false, "ready_timeout");
        emit_target(*target, "terminal", target->terminal_reason,
                    retry_decision(*target), live_endpoints(targets).size());
    }
    if (!expired.empty()) publish_live_source(source, targets);
    return q2dz::Result<void>::ok();
}

q2dz::Result<void> recover_shared_gateway(std::optional<LiveGateway>& gateway,
                                          LiveTargets& targets,
                                          CustomSource& source,
                                          const Options& options,
                                          const fs::path& gateway_path,
                                          const fs::path& session_dir,
                                          const std::string& session_id,
                                          Guardian& guardian,
                                          RoomsCommandReader& reader,
                                          unsigned& restart_count,
                                          bool& transport_disabled,
                                          std::string_view reason) {
    reader.begin_gateway_recovery();
    source.publish({});
    targets.clear();
    if (gateway) {
        stop_live_gateway(*gateway, options, guardian, reason);
        emit_gateway("stop", reason, restart_count);
        gateway.reset();
    }
    if (restart_count != 0) {
        transport_disabled = true;
        emit_gateway("fatal", "transport_disabled", restart_count);
        emit_event("HUB_TRANSPORT_DISABLED reason=" + std::string(reason));
        return q2dz::Result<void>::ok();
    }
    ++restart_count;
    auto restarted = start_shared_gateway(gateway, gateway_path, session_dir, session_id, options, guardian);
    if (!restarted) {
        transport_disabled = true;
        emit_gateway("fatal", "restart_failed", restart_count);
        emit_event("HUB_TRANSPORT_DISABLED reason=restart-failed");
    } else {
        emit_gateway("restart", "none", restart_count);
    }
    return q2dz::Result<void>::ok();
}

int child_exit_code(int status);

int finish_live_hub(const Options& options,
                    LiveTargets& targets,
                    std::optional<LiveGateway>& gateway,
                    pid_t& q2_pid,
                    Guardian& guardian,
                    CustomSource& source,
                    unsigned restart_count,
                    std::string_view reason,
                    int result_code) {
    targets.clear();
    source.publish({});
    if (gateway) {
        stop_live_gateway(*gateway, options, guardian, reason);
        emit_gateway("stop", reason, restart_count);
        gateway.reset();
    }
    source.stop();
    if (q2_pid > 0) {
        terminate_group(q2_pid, options.shutdown_grace_ms, -1, q2_pid);
        q2_pid = -1;
    }
    const auto disarm = q2dz::write_all(guardian.control.get(), "DISARM\n");
    guardian.control.reset();
    guardian.owner.reset();
    int guardian_status = 0;
    if (::waitpid(guardian.pid, &guardian_status, 0) != guardian.pid ||
        !WIFEXITED(guardian_status) || WEXITSTATUS(guardian_status) != 0 || !disarm) {
        emit_event("guardian cleanup failed");
        emit_event("SESSION stop reason=" + log_value(reason) +
                   " rc=" + std::to_string(kCleanupFailure));
        return kCleanupFailure;
    }
    emit_event("SESSION stop reason=" + log_value(reason) +
               " rc=" + std::to_string(result_code));
    emit_event("SESSION_CLOSED reason=" + std::string(reason));
    return result_code;
}

int run_live_hub(const Options& options,
                 const fs::path& gateway_path,
                 const fs::path& legacy,
                 const fs::path& adapter,
                 const fs::path& wine,
                 const fs::path& q2,
                 const fs::path& session_dir,
                 const std::string& session_id,
                 Guardian& guardian,
                 Pipes& signal_pipe) {
    static_cast<void>(legacy);
    static_cast<void>(adapter);
    CustomSource source;
    LiveTargets targets;
    std::optional<LiveGateway> gateway;
    unsigned restart_count = 0;
    bool transport_disabled = false;
    emit_event("SESSION start build=" + std::string(Q2DZ_BUILD_VERSION) +
               " session=" + log_value(session_id) +
               " instance=" + std::to_string(static_cast<long long>(::getpid())) +
               " gateway_restarts=0");
    if (auto started = source.start(session_id); !started) {
        pid_t no_q2 = -1;
        return finish_live_hub(options, targets, gateway, no_q2, guardian, source,
                               restart_count, "source-start-failed", kInternal);
    }
    source.publish({});
    // A transport child is shared by all targets. Failure to start transport
    // is isolated: Q2 still starts with an empty custom source.
    if (auto started = start_shared_gateway(gateway, gateway_path, session_dir, session_id, options, guardian); !started) {
        transport_disabled = true;
        emit_gateway("fatal", "start_failed", restart_count);
        emit_event("HUB_TRANSPORT_DISABLED reason=start-failed");
    } else {
        emit_gateway("start", "none", restart_count);
    }
    auto q2_result = spawn_q2_live(wine, q2, source.url(), options, session_id);
    if (!q2_result) {
        pid_t no_q2 = -1;
        return finish_live_hub(options, targets, gateway, no_q2, guardian, source,
                               restart_count, "q2-spawn-failed", kQ2Failure);
    }
    pid_t q2_pid = q2_result.value();
    if (auto registered = q2dz::write_all(guardian.control.get(), "PGID " + std::to_string(q2_pid) + "\n"); !registered) {
        return finish_live_hub(options, targets, gateway, q2_pid, guardian, source,
                               restart_count, "guardian-control-failed", kInternal);
    }
    if (auto nonblocking = q2dz::set_nonblocking(STDIN_FILENO); !nonblocking) {
        return finish_live_hub(options, targets, gateway, q2_pid, guardian, source,
                               restart_count, "hub-input-failed", kInternal);
    }

    RoomsCommandReader reader;
    bool input_eof = false;
    std::uint64_t next_target_id = 1;
    int result_code = 0;
    std::string closing_reason;
    while (result_code == 0 && signal_seen.load() == 0) {
        if (source.failed()) {
            result_code = kInternal;
            closing_reason = "source-failed";
            break;
        }
        pollfd fds[2] = {{signal_pipe.read.get(), POLLIN, 0}, {STDIN_FILENO, POLLIN | POLLHUP, 0}};
        const int poll_result = ::poll(fds, 2, 100);
        if (poll_result < 0 && errno != EINTR) {
            result_code = kInternal;
            closing_reason = "poll-failed";
            break;
        }
        if ((fds[1].revents & (POLLIN | POLLHUP)) != 0 || reader.needs_service()) {
            auto commands = reader.read(STDIN_FILENO, input_eof);
            if (!commands) {
                emit_event("HUB_PROTOCOL_ERROR " + commands.error().message);
                result_code = kConfig;
                closing_reason = "hub-protocol-failed";
                break;
            }
            bool command_failed = false;
            for (auto& command : commands.value()) {
                auto processed = process_rooms_command(command, targets, gateway, next_target_id,
                                                        options, source, transport_disabled);
                if (!processed) {
                    command_failed = true;
                    break;
                }
                command.clear();
            }
            if (command_failed) {
                for (auto& command : commands.value()) command.clear();
                (void)recover_shared_gateway(gateway, targets, source, options, gateway_path, session_dir,
                                             session_id, guardian, reader, restart_count, transport_disabled,
                                             "gateway-command-failed");
            }
            if (input_eof) {
                closing_reason = "control-eof";
                break;
            }
        }
        if (auto gateway_status = process_shared_gateway(gateway, targets, source); !gateway_status) {
            (void)recover_shared_gateway(gateway, targets, source, options, gateway_path, session_dir,
                                         session_id, guardian, reader, restart_count, transport_disabled,
                                         gateway_status.error().message);
        }
        if (auto timeout_status = process_target_timeouts(targets, gateway, source); !timeout_status) {
            (void)recover_shared_gateway(gateway, targets, source, options, gateway_path, session_dir,
                                         session_id, guardian, reader, restart_count, transport_disabled,
                                         "target-timeout-control-failed");
        }
        if (source.failed()) {
            result_code = kInternal;
            closing_reason = "source-failed";
            break;
        }
        int q2_status = 0;
        if (q2_pid > 0 && ::waitpid(q2_pid, &q2_status, WNOHANG) == q2_pid) {
            const int code = child_exit_code(q2_status);
            q2_pid = -1;
            result_code = code == 0 ? 0 : kQ2Failure;
            closing_reason = code == 0 ? "q2-normal-exit" : "q2-failed";
            break;
        }
    }
    if (signal_seen.load() != 0 && closing_reason.empty()) {
        result_code = 128 + signal_seen.load();
        closing_reason = "owner-stop";
    }
    if (closing_reason.empty()) closing_reason = "hub-stopped";
    return finish_live_hub(options, targets, gateway, q2_pid, guardian, source,
                           restart_count, closing_reason, result_code);
}

int child_exit_code(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 255;
}

// ---------------- guardian ----------------

struct GuardianOptions {
    int owner_fd = -1;
    int control_fd = -1;
    fs::path base;
    fs::path session_dir;
    std::string session_id;
    uid_t uid = 0;
    pid_t owner_pid = -1;
    int grace_ms = 3000;
};

q2dz::Result<GuardianOptions> parse_guardian_options(int argc, char** argv) {
    GuardianOptions out;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) return q2dz::Result<GuardianOptions>::fail("guardian option lacks value");
        const std::string value = argv[++i];
        auto number = [&](long long min, long long max) -> q2dz::Result<long long> {
            long long parsed = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed < min || parsed > max) {
                return q2dz::Result<long long>::fail("invalid guardian number");
            }
            return q2dz::Result<long long>::ok(parsed);
        };
        if (arg == "--owner-fd") { auto n = number(0, INT_MAX); if (!n) return q2dz::Result<GuardianOptions>::fail(n.error().message); out.owner_fd = static_cast<int>(n.value()); }
        else if (arg == "--control-fd") { auto n = number(0, INT_MAX); if (!n) return q2dz::Result<GuardianOptions>::fail(n.error().message); out.control_fd = static_cast<int>(n.value()); }
        else if (arg == "--base") out.base = value;
        else if (arg == "--session-dir") out.session_dir = value;
        else if (arg == "--session-id") out.session_id = value;
        else if (arg == "--uid") { auto n = number(0, UINT_MAX); if (!n) return q2dz::Result<GuardianOptions>::fail(n.error().message); out.uid = static_cast<uid_t>(n.value()); }
        else if (arg == "--owner-pid") { auto n = number(1, INT_MAX); if (!n) return q2dz::Result<GuardianOptions>::fail(n.error().message); out.owner_pid = static_cast<pid_t>(n.value()); }
        else if (arg == "--grace-ms") { auto n = number(100, 30000); if (!n) return q2dz::Result<GuardianOptions>::fail(n.error().message); out.grace_ms = static_cast<int>(n.value()); }
        else return q2dz::Result<GuardianOptions>::fail("unknown guardian option");
    }
    if (out.owner_fd < 0 || out.control_fd < 0 || out.base.empty() || out.session_dir.empty() || out.session_id.empty()) {
        return q2dz::Result<GuardianOptions>::fail("guardian configuration incomplete");
    }
    return q2dz::Result<GuardianOptions>::ok(std::move(out));
}

std::vector<pid_t> marked_processes(const GuardianOptions& options) {
    std::vector<pid_t> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/proc", fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        const auto name = entry.path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isdigit(c); })) continue;
        int value = 0;
        const auto parsed = std::from_chars(name.data(), name.data() + name.size(), value);
        if (parsed.ec != std::errc{} || value <= 1) continue;
        const pid_t pid = static_cast<pid_t>(value);
        if (pid == ::getpid() || pid == options.owner_pid) continue;
        const auto comm = q2dz::process_comm(pid);
        if (comm == "wineserver" || comm == "wineserver64") continue;
        if (q2dz::process_has_session_marker(pid, options.uid, options.session_id)) out.push_back(pid);
    }
    return out;
}

void kill_owned(const GuardianOptions& options, const std::set<pid_t>& pgids) {
    for (const auto pgid : pgids) {
        if (pgid > 1 && pgid != ::getpgrp()) (void)::kill(-pgid, SIGTERM);
    }
    for (const auto pid : marked_processes(options)) (void)::kill(pid, SIGTERM);
    const auto deadline = q2dz::monotonic_millis() + static_cast<std::uint64_t>(options.grace_ms);
    while (q2dz::monotonic_millis() < deadline) {
        if (marked_processes(options).empty()) break;
        ::usleep(50000);
    }
    for (const auto pgid : pgids) {
        if (pgid > 1 && pgid != ::getpgrp()) (void)::kill(-pgid, SIGKILL);
    }
    for (const auto pid : marked_processes(options)) (void)::kill(pid, SIGKILL);
}

int guardian_main(int argc, char** argv) {
    auto options = parse_guardian_options(argc, argv);
    if (!options) return kUsage;
    (void)::setsid();
    ::umask(0077);
    (void)q2dz::disable_core_dumps_and_ptrace();
    std::set<pid_t> pgids;
    std::string control_buffer;
    bool disarmed = false;
    while (true) {
        pollfd fds[2] = {{options.value().owner_fd, POLLIN | POLLHUP, 0}, {options.value().control_fd, POLLIN | POLLHUP, 0}};
        const int rc = ::poll(fds, 2, -1);
        if (rc < 0) { if (errno == EINTR) continue; break; }
        if ((fds[1].revents & POLLIN) != 0) {
            std::array<char, 256> temp{};
            const ssize_t n = ::read(options.value().control_fd, temp.data(), temp.size());
            if (n > 0) {
                control_buffer.append(temp.data(), static_cast<std::size_t>(n));
                while (true) {
                    const auto newline = control_buffer.find('\n');
                    if (newline == std::string::npos) break;
                    const auto line = control_buffer.substr(0, newline);
                    control_buffer.erase(0, newline + 1);
                    if (line.starts_with("PGID ")) {
                        int parsed = -1;
                        const auto text = std::string_view(line).substr(5);
                        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
                        if (result.ec == std::errc{} && result.ptr == text.data() + text.size() && parsed > 1) pgids.insert(static_cast<pid_t>(parsed));
                    } else if (line.starts_with("DROP ")) {
                        int parsed = -1;
                        const auto text = std::string_view(line).substr(5);
                        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
                        if (result.ec == std::errc{} && result.ptr == text.data() + text.size()) pgids.erase(static_cast<pid_t>(parsed));
                    } else if (line == "DISARM") {
                        disarmed = true;
                        break;
                    }
                }
            }
        }
        if (disarmed) break;
        if ((fds[0].revents & (POLLHUP | POLLERR)) != 0) { kill_owned(options.value(), pgids); break; }
        if ((fds[1].revents & (POLLHUP | POLLERR)) != 0) {
            // Losing coordinator control without explicit DISARM is a crash.
            kill_owned(options.value(), pgids);
            break;
        }
    }
    auto cleanup = q2dz::remove_session_dir_safely(options.value().base, options.value().session_dir, options.value().session_id);
    return cleanup ? 0 : kCleanupFailure;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string_view(argv[1]) == "__guardian") return guardian_main(argc, argv);
    ::umask(0077);
    if (auto hardened = q2dz::disable_core_dumps_and_ptrace(); !hardened) {
        emit_event("hardening failed: " + hardened.error().message);
        return kInternal;
    }
    auto options = parse_options(argc, argv);
    if (!options) { emit_event(options.error().message); usage(std::cerr); return kUsage; }

    auto gateway = resolve_program(options.value().gateway, {});
    auto legacy = resolve_program(options.value().legacy_gateway, {});
    auto wine = resolve_program(options.value().wine, {"wine64", "wine"});
    auto q2 = discover_q2(options.value());
    if (!gateway || !legacy || !wine || !q2) {
        if (!gateway) emit_event(gateway.error().message);
        if (!legacy) emit_event(legacy.error().message);
        if (!wine) emit_event(wine.error().message);
        if (!q2) emit_event(q2.error().message);
        return kUnavailable;
    }
    if (!fs::is_regular_file(options.value().adapter)) {
        emit_event("adapter config not found: " + options.value().adapter);
        return kUnavailable;
    }

    auto runtime_base = prepare_runtime_base(options.value().runtime_base);
    if (!runtime_base) { emit_event(runtime_base.error().message); return kInternal; }
    auto lock = acquire_global_lock(runtime_base.value());
    if (!lock) { emit_event(lock.error().message); return kUnavailable; }
    cleanup_stale_sessions(runtime_base.value());

    const std::string session_id = q2dz::random_hex(16);
    auto session_dir = create_session_dir(runtime_base.value(), session_id);
    if (!session_dir) { emit_event(session_dir.error().message); return kInternal; }
    fs::path self = fs::canonical("/proc/self/exe");
    auto guardian = start_guardian(self, runtime_base.value(), session_dir.value(), session_id, options.value().shutdown_grace_ms);
    if (!guardian) {
        (void)q2dz::remove_session_dir_safely(runtime_base.value(), session_dir.value(), session_id);
        emit_event(guardian.error().message);
        return kInternal;
    }

    auto signal_pipe = make_pipe(true);
    if (!signal_pipe) {
        (void)q2dz::write_all(guardian.value().control.get(), "DISARM\n");
        guardian.value().control.reset();
        guardian.value().owner.reset();
        int guardian_status = 0;
        (void)::waitpid(guardian.value().pid, &guardian_status, 0);
        (void)q2dz::remove_session_dir_safely(runtime_base.value(), session_dir.value(), session_id);
        return kInternal;
    }
    signal_pipe_write = signal_pipe.value().write.get();
    struct sigaction action{};
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    (void)::sigaction(SIGINT, &action, nullptr);
    (void)::sigaction(SIGTERM, &action, nullptr);
    (void)::sigaction(SIGHUP, &action, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    return run_live_hub(options.value(), gateway.value(), legacy.value(), options.value().adapter,
                        wine.value(), q2.value(), session_dir.value(), session_id,
                        guardian.value(), signal_pipe.value());
}
