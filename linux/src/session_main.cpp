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
        "  Hub commands are read as newline-delimited '<slot> <room-id> <JSON>' or '<slot> -' lines\n"
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

constexpr std::size_t kHubMaxRooms = 2;
constexpr std::size_t kHubMaxLine = 256U * 1024U;

struct SlotCommand {
    std::size_t slot = 0;
    std::optional<std::string> room_id;
    std::optional<std::string> config;
};

void clear_slot_command(SlotCommand& command) noexcept {
    if (command.config) q2dz::secure_clear(*command.config);
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

bool safe_room_id(std::string_view value) {
    if (value.empty() || value.size() > 255U) return false;
    for (const char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        if (!(std::isalnum(c) || c == '.' || c == '-' || c == '_')) return false;
    }
    return true;
}

q2dz::Result<SlotCommand> parse_slot_command(std::string_view line) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty() || line.size() + 1U > kHubMaxLine) {
        return q2dz::Result<SlotCommand>::fail("hub command line exceeds 256 KiB or is empty");
    }
    if (!valid_utf8(line) || line.size() < 3U || (line[0] != '0' && line[0] != '1') || line[1] != ' ') {
        return q2dz::Result<SlotCommand>::fail("hub command framing is invalid");
    }
    SlotCommand command;
    command.slot = static_cast<std::size_t>(line[0] - '0');
    if (line.substr(2) == "-") return q2dz::Result<SlotCommand>::ok(std::move(command));
    const auto first = line.find(' ', 2);
    if (first == std::string_view::npos) return q2dz::Result<SlotCommand>::fail("hub command lacks room value");
    const auto room = line.substr(2, first - 2);
    if (room == "-") return q2dz::Result<SlotCommand>::fail("empty hub slot has trailing data");
    if (!safe_room_id(room)) return q2dz::Result<SlotCommand>::fail("hub room ID is unsafe");
    if (first + 1U >= line.size()) return q2dz::Result<SlotCommand>::fail("hub command lacks room JSON");
    const auto config = line.substr(first + 1U);
    if (!valid_utf8(config)) return q2dz::Result<SlotCommand>::fail("hub room JSON is not UTF-8");
    auto parsed = q2dz::json::parse(config);
    if (!parsed || !parsed.value().is_object()) {
        return q2dz::Result<SlotCommand>::fail("hub room JSON must be an object");
    }
    command.room_id = std::string(room);
    command.config = std::string(config);
    return q2dz::Result<SlotCommand>::ok(std::move(command));
}

class SlotCommandReader {
public:
    q2dz::Result<std::vector<SlotCommand>> read(int fd, bool& eof) {
        std::vector<SlotCommand> commands;
        std::array<char, 8192> data{};
        while (true) {
            const ssize_t count = ::read(fd, data.data(), data.size());
            if (count < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                secure_clear_storage(buffer_);
                return q2dz::Result<std::vector<SlotCommand>>::fail(q2dz::errno_message("read hub input"));
            }
            if (count == 0) {
                eof = true;
                break;
            }
            buffer_.append(data.data(), static_cast<std::size_t>(count));
            if (auto parsed = parse_complete_lines(commands); !parsed) {
                return q2dz::Result<std::vector<SlotCommand>>::fail(parsed.error().message);
            }
            if (buffer_.size() >= kHubMaxLine && buffer_.find('\n') == std::string::npos) {
                clear_commands(commands);
                secure_clear_storage(buffer_);
                return q2dz::Result<std::vector<SlotCommand>>::fail("hub command line exceeds 256 KiB");
            }
        }
        if (auto parsed = parse_complete_lines(commands); !parsed) {
            return q2dz::Result<std::vector<SlotCommand>>::fail(parsed.error().message);
        }
        if (eof && !buffer_.empty()) {
            clear_commands(commands);
            secure_clear_storage(buffer_);
            return q2dz::Result<std::vector<SlotCommand>>::fail("hub input ended after a partial command");
        }
        return q2dz::Result<std::vector<SlotCommand>>::ok(std::move(commands));
    }

private:
    static void clear_commands(std::vector<SlotCommand>& commands) noexcept {
        for (auto& command : commands) clear_slot_command(command);
    }

    static void secure_clear_storage(std::string& value) noexcept {
        // resize() cannot grow beyond the already allocated capacity.  This
        // makes the subsequent wipe cover stale bytes left in the backing
        // allocation, not just the current logical string.
        try {
            value.resize(value.capacity(), '\0');
        } catch (...) {
            // Best effort fallback: secure_clear() still wipes the logical
            // string if an implementation unexpectedly throws here.
        }
        q2dz::secure_clear(value);
    }

    void erase_prefix(std::size_t count) {
        if (count == 0) return;
        std::string unread;
        if (count < buffer_.size()) unread.assign(buffer_.data() + count, buffer_.size() - count);
        secure_clear_storage(buffer_);
        buffer_.swap(unread);
        secure_clear_storage(unread);
    }

    q2dz::Result<void> parse_complete_lines(std::vector<SlotCommand>& commands) {
        while (true) {
            const auto newline = buffer_.find('\n');
            if (newline == std::string::npos) break;
            if (newline + 1U > kHubMaxLine) {
                clear_commands(commands);
                secure_clear_storage(buffer_);
                return q2dz::Result<void>::fail("hub command line exceeds 256 KiB");
            }
            std::string line(buffer_.data(), newline);
            erase_prefix(newline + 1U);
            auto parsed = parse_slot_command(line);
            q2dz::secure_clear(line);
            if (!parsed) {
                clear_commands(commands);
                secure_clear_storage(buffer_);
                return q2dz::Result<void>::fail(parsed.error().message);
            }
            commands.push_back(std::move(parsed.value()));
        }
        return q2dz::Result<void>::ok();
    }

public:
    ~SlotCommandReader() { secure_clear_storage(buffer_); }

private:
    std::string buffer_;
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

class CustomSource {
public:
    q2dz::Result<void> start() {
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
        url_ = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/servers";
        try {
            worker_ = std::thread([this] { serve(); });
        } catch (const std::exception& error) {
            listener_.reset();
            stop_read_.reset();
            stop_write_.reset();
            url_.clear();
            return q2dz::Result<void>::fail(std::string("custom source thread: ") + error.what());
        }
        return q2dz::Result<void>::ok();
    }

    [[nodiscard]] std::string url() const { return url_; }

    void publish(const std::vector<q2dz::Endpoint>& endpoints) {
        std::string body;
        for (const auto& endpoint : endpoints) {
            body += "127.0.0.1:" + std::to_string(endpoint.port) + "\n";
        }
        std::lock_guard lock(mutex_);
        body_ = std::move(body);
    }

    [[nodiscard]] bool failed() const noexcept { return failed_.load(); }

    void stop() noexcept {
        if (!worker_.joinable()) {
            listener_.reset();
            stop_read_.reset();
            stop_write_.reset();
            return;
        }
        const unsigned char byte = 1;
        (void)::write(stop_write_.get(), &byte, sizeof(byte));
        worker_.join();
        listener_.reset();
        stop_read_.reset();
        stop_write_.reset();
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
        std::string response;
        if (first_line == "GET /servers HTTP/1.1") {
            std::string body;
            {
                std::lock_guard lock(mutex_);
                body = body_;
            }
            response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
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
    std::string body_;
    std::string url_;
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
    UniqueFd control;
    UniqueFd diagnostics;
};

q2dz::Result<Child> spawn_gateway(const fs::path& gateway,
                                  const fs::path& legacy,
                                  const fs::path& adapter,
                                  const fs::path& session_dir,
                                  const std::string& session_id,
                                  std::string_view room_json) {
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
        std::vector<std::string> storage = {
            gateway.string(), "--managed", "--legacy", legacy.string(),
            "--adapter", adapter.string(), "--bind", "127.0.0.1:0",
            "--runtime-dir", session_dir.string()
        };
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
    auto write_result = q2dz::write_all(input.value().write.get(), room_json);
    input.value().write.reset();
    if (!write_result) {
        (void)::kill(-pid, SIGKILL);
        return q2dz::Result<Child>::fail(write_result.error().message);
    }
    return q2dz::Result<Child>::ok(Child{pid, pid, std::move(control.value().read), std::move(diagnostics.value().read)});
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

void drain_diagnostics(int fd, std::string& buffer) {
    std::array<char, 4096> temp{};
    while (true) {
        const ssize_t n = ::read(fd, temp.data(), temp.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return;
        }
        if (n == 0) return;
        buffer.append(temp.data(), static_cast<std::size_t>(n));
        while (true) {
            const auto newline = buffer.find('\n');
            if (newline == std::string::npos) break;
            std::cerr << q2dz::sanitize_log_line(std::string_view(buffer).substr(0, newline)) << "\n";
            buffer.erase(0, newline + 1);
        }
        if (buffer.size() > 64U * 1024U) buffer.erase(0, buffer.size() - 4096U);
    }
}

std::vector<std::string> take_lines(int fd, std::string& buffer, bool& eof) {
    std::vector<std::string> out;
    std::array<char, 4096> temp{};
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
        if (buffer.size() > 16384U) { eof = true; break; }
        while (true) {
            const auto newline = buffer.find('\n');
            if (newline == std::string::npos) break;
            out.push_back(buffer.substr(0, newline + 1));
            buffer.erase(0, newline + 1);
        }
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
    std::optional<q2dz::Endpoint> endpoint;
    bool ready = false;
    bool control_eof = false;
    std::uint64_t ready_deadline = 0;
};

struct LiveSlot {
    std::string room_id;
    unsigned post_ready_failures = 0;
    bool tombstone = false;
    std::optional<LiveGateway> gateway;
};

void stop_live_gateway(LiveGateway& gateway,
                       const Options& options,
                       Guardian& guardian,
                       std::string_view reason) {
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
    std::cerr << "HUB_GATEWAY_STOP reason=" << reason << "\n";
}

void remove_live_gateway(LiveSlot& slot,
                         const Options& options,
                         Guardian& guardian,
                         std::string_view reason) {
    if (slot.gateway) {
        stop_live_gateway(*slot.gateway, options, guardian, reason);
        slot.gateway.reset();
    }
}

std::vector<q2dz::Endpoint> live_endpoints(const std::array<LiveSlot, kHubMaxRooms>& slots) {
    std::vector<q2dz::Endpoint> endpoints;
    for (const auto& slot : slots) {
        if (slot.gateway && slot.gateway->ready && slot.gateway->endpoint) endpoints.push_back(*slot.gateway->endpoint);
    }
    return endpoints;
}

void publish_live_source(CustomSource& source, const std::array<LiveSlot, kHubMaxRooms>& slots) {
    source.publish(live_endpoints(slots));
}

void unpublish_live_gateway(LiveSlot& slot,
                            CustomSource& source,
                            const std::array<LiveSlot, kHubMaxRooms>& slots) {
    if (slot.gateway) {
        slot.gateway->ready = false;
        slot.gateway->endpoint.reset();
    }
    publish_live_source(source, slots);
}

void mark_live_gateway_failure(LiveSlot& slot,
                               const Options& options,
                               Guardian& guardian,
                               CustomSource& source,
                               const std::array<LiveSlot, kHubMaxRooms>& slots,
                               std::string_view reason,
                               bool room_closed) {
    const bool was_ready = slot.gateway && slot.gateway->ready;
    unpublish_live_gateway(slot, source, slots);
    remove_live_gateway(slot, options, guardian, reason);
    if (room_closed || !was_ready || slot.post_ready_failures != 0) {
        slot.tombstone = true;
    } else {
        slot.post_ready_failures = 1;
    }
    publish_live_source(source, slots);
}

q2dz::Result<void> validate_live_room_config(std::string_view config) {
    auto parsed = q2dz::json::parse(config);
    if (!parsed || !parsed.value().is_object()) return q2dz::Result<void>::fail("room JSON must be an object");
    if (auto room = q2dz::json::extract_room_config(parsed.value()); !room) {
        return q2dz::Result<void>::fail(room.error().message);
    }
    return q2dz::Result<void>::ok();
}

q2dz::Result<void> start_live_gateway(LiveSlot& slot,
                                      const fs::path& gateway,
                                      const fs::path& legacy,
                                      const fs::path& adapter,
                                      const fs::path& session_dir,
                                      const std::string& session_id,
                                      const Options& options,
                                      Guardian& guardian,
                                      std::string_view room_json) {
    auto child = spawn_gateway(gateway, legacy, adapter, session_dir, session_id, room_json);
    if (!child) return q2dz::Result<void>::fail(child.error().message);
    LiveGateway runtime{std::move(child.value()), {}, {}, std::nullopt, false, false,
                        q2dz::monotonic_millis() + static_cast<std::uint64_t>(options.ready_timeout_ms)};
    const pid_t process_group = runtime.child.process_group;
    slot.gateway.emplace(std::move(runtime));
    if (!q2dz::write_all(guardian.control.get(), "PGID " + std::to_string(process_group) + "\n")) {
        remove_live_gateway(slot, options, guardian, "guardian-control-failed");
        return q2dz::Result<void>::fail("guardian control pipe failed");
    }
    return q2dz::Result<void>::ok();
}

q2dz::Result<void> process_live_command(SlotCommand& command,
                                        std::array<LiveSlot, kHubMaxRooms>& slots,
                                        const fs::path& gateway,
                                        const fs::path& legacy,
                                        const fs::path& adapter,
                                        const fs::path& session_dir,
                                        const std::string& session_id,
                                        const Options& options,
                                        Guardian& guardian,
                                        CustomSource& source) {
    auto& slot = slots[command.slot];
    if (!command.room_id) {
        unpublish_live_gateway(slot, source, slots);
        remove_live_gateway(slot, options, guardian, "slot-cleared");
        slot.room_id.clear();
        slot.post_ready_failures = 0;
        slot.tombstone = false;
        publish_live_source(source, slots);
        clear_slot_command(command);
        return q2dz::Result<void>::ok();
    }

    const std::string requested_id = *command.room_id;
    const bool same_room = slot.room_id == requested_id;
    if (same_room && (slot.gateway || slot.tombstone)) {
        // Heartbeats for a live or tombstoned room carry fresh credentials but
        // cannot change its lifecycle state.
        clear_slot_command(command);
        return q2dz::Result<void>::ok();
    }
    if (!command.config) {
        slot.tombstone = true;
        clear_slot_command(command);
        return q2dz::Result<void>::ok();
    }
    const auto semantic = validate_live_room_config(*command.config);
    if (!same_room) {
        unpublish_live_gateway(slot, source, slots);
        remove_live_gateway(slot, options, guardian, "slot-replaced");
        slot.room_id = requested_id;
        slot.post_ready_failures = 0;
        slot.tombstone = false;
    }
    if (!semantic) {
        slot.tombstone = true;
        q2dz::secure_clear(*command.config);
        publish_live_source(source, slots);
        return q2dz::Result<void>::ok();
    }
    auto started = start_live_gateway(slot, gateway, legacy, adapter, session_dir, session_id,
                                      options, guardian, *command.config);
    q2dz::secure_clear(*command.config);
    if (!started) {
        if (started.error().message == "guardian control pipe failed") return started;
        slot.tombstone = true;
    }
    publish_live_source(source, slots);
    return q2dz::Result<void>::ok();
}

bool process_live_gateway(LiveSlot& slot,
                          const Options& options,
                          Guardian& guardian,
                          CustomSource& source,
                          const std::array<LiveSlot, kHubMaxRooms>& slots) {
    if (!slot.gateway) return false;
    auto& gateway = *slot.gateway;
    pollfd child_fds[2] = {
        {gateway.child.control.get(), POLLIN | POLLHUP, 0},
        {gateway.child.diagnostics.get(), POLLIN | POLLHUP, 0}
    };
    if (::poll(child_fds, 2, 0) < 0 && errno != EINTR) {
        mark_live_gateway_failure(slot, options, guardian, source, slots, "gateway-poll-failed", false);
        return true;
    }
    if ((child_fds[1].revents & (POLLIN | POLLHUP)) != 0) {
        drain_diagnostics(gateway.child.diagnostics.get(), gateway.diagnostic_buffer);
    }
    if ((child_fds[0].revents & (POLLIN | POLLHUP)) != 0) {
        for (const auto& line : take_lines(gateway.child.control.get(), gateway.control_buffer, gateway.control_eof)) {
            auto event = q2dz::parse_gateway_event(line);
            if (!event) {
                mark_live_gateway_failure(slot, options, guardian, source, slots, "invalid-gateway-event", false);
                return true;
            }
            switch (event.value().type) {
                case q2dz::GatewayEventType::TransportReady: {
                    if (gateway.ready) {
                        mark_live_gateway_failure(slot, options, guardian, source, slots, "duplicate-transport-ready", false);
                        return true;
                    }
                    auto endpoint = q2dz::ready_endpoint(event.value());
                    if (!endpoint) {
                        mark_live_gateway_failure(slot, options, guardian, source, slots, "invalid-ready-endpoint", false);
                        return true;
                    }
                    gateway.endpoint = endpoint.value();
                    gateway.ready = true;
                    publish_live_source(source, slots);
                    break;
                }
                case q2dz::GatewayEventType::RoomClosed:
                    mark_live_gateway_failure(slot, options, guardian, source, slots, "room-closed", true);
                    return true;
                case q2dz::GatewayEventType::Fatal:
                    mark_live_gateway_failure(slot, options, guardian, source, slots, "gateway-fatal", false);
                    return true;
                case q2dz::GatewayEventType::Heartbeat:
                    break;
            }
            if (!slot.gateway) return true;
        }
    }
    if (!slot.gateway) return true;
    auto& current = *slot.gateway;
    int status = 0;
    if (current.child.pid > 0 && ::waitpid(current.child.pid, &status, WNOHANG) == current.child.pid) {
        current.child.pid = -1;
        mark_live_gateway_failure(slot, options, guardian, source, slots,
                                  current.ready ? "gateway-exited" : "gateway-exited-before-ready", false);
        return true;
    }
    if (current.control_eof) {
        mark_live_gateway_failure(slot, options, guardian, source, slots,
                                  current.ready ? "gateway-control-eof" : "gateway-control-eof-before-ready", false);
        return true;
    }
    if (!current.ready && q2dz::monotonic_millis() >= current.ready_deadline) {
        mark_live_gateway_failure(slot, options, guardian, source, slots, "transport-ready-timeout", false);
        return true;
    }
    return false;
}

int child_exit_code(int status);

int finish_live_hub(const Options& options,
                    std::array<LiveSlot, kHubMaxRooms>& slots,
                    pid_t& q2_pid,
                    Guardian& guardian,
                    CustomSource& source,
                    std::string_view reason,
                    int result_code) {
    source.publish({});
    source.stop();
    for (auto& slot : slots) remove_live_gateway(slot, options, guardian, reason);
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
        std::cerr << "guardian cleanup failed\n";
        return kCleanupFailure;
    }
    std::cerr << "SESSION_CLOSED reason=" << reason << "\n";
    return result_code;
}

int run_live_hub(const Options& options,
                 const fs::path& gateway,
                 const fs::path& legacy,
                 const fs::path& adapter,
                 const fs::path& wine,
                 const fs::path& q2,
                 const fs::path& session_dir,
                 const std::string& session_id,
                 Guardian& guardian,
                 Pipes& signal_pipe) {
    CustomSource source;
    if (auto started = source.start(); !started) {
        std::array<LiveSlot, kHubMaxRooms> slots{};
        pid_t no_q2 = -1;
        return finish_live_hub(options, slots, no_q2, guardian, source,
                               "source-start-failed", kInternal);
    }
    source.publish({});
    auto q2_result = spawn_q2_live(wine, q2, source.url(), options, session_id);
    if (!q2_result) {
        std::array<LiveSlot, kHubMaxRooms> slots{};
        pid_t no_q2 = -1;
        return finish_live_hub(options, slots, no_q2, guardian, source, "q2-spawn-failed", kQ2Failure);
    }
    pid_t q2_pid = q2_result.value();
    if (auto registered = q2dz::write_all(guardian.control.get(), "PGID " + std::to_string(q2_pid) + "\n"); !registered) {
        std::array<LiveSlot, kHubMaxRooms> slots{};
        return finish_live_hub(options, slots, q2_pid, guardian, source,
                               "guardian-control-failed", kInternal);
    }
    if (auto nonblocking = q2dz::set_nonblocking(STDIN_FILENO); !nonblocking) {
        std::array<LiveSlot, kHubMaxRooms> slots{};
        return finish_live_hub(options, slots, q2_pid, guardian, source, "hub-input-failed", kInternal);
    }

    std::array<LiveSlot, kHubMaxRooms> slots{};
    SlotCommandReader reader;
    bool input_eof = false;
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
        if ((fds[1].revents & (POLLIN | POLLHUP)) != 0) {
            auto commands = reader.read(STDIN_FILENO, input_eof);
            if (!commands) {
                std::cerr << "HUB_PROTOCOL_ERROR " << commands.error().message << "\n";
                result_code = kConfig;
                closing_reason = "hub-protocol-failed";
                break;
            }
            for (auto& command : commands.value()) {
                auto processed = process_live_command(command, slots, gateway, legacy, adapter, session_dir, session_id,
                                                      options, guardian, source);
                clear_slot_command(command);
                if (!processed) {
                    std::cerr << "HUB_FATAL guardian-control-failed\n";
                    result_code = kInternal;
                    closing_reason = "guardian-control-failed";
                    break;
                }
            }
            if (input_eof) {
                closing_reason = "control-eof";
                break;
            }
        }
        for (auto& slot : slots) (void)process_live_gateway(slot, options, guardian, source, slots);
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
    return finish_live_hub(options, slots, q2_pid, guardian, source, closing_reason, result_code);
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
        std::cerr << "hardening failed: " << hardened.error().message << "\n";
        return kInternal;
    }
    auto options = parse_options(argc, argv);
    if (!options) { std::cerr << options.error().message << "\n"; usage(std::cerr); return kUsage; }

    auto gateway = resolve_program(options.value().gateway, {});
    auto legacy = resolve_program(options.value().legacy_gateway, {});
    auto wine = resolve_program(options.value().wine, {"wine64", "wine"});
    auto q2 = discover_q2(options.value());
    if (!gateway || !legacy || !wine || !q2) {
        if (!gateway) std::cerr << gateway.error().message << "\n";
        if (!legacy) std::cerr << legacy.error().message << "\n";
        if (!wine) std::cerr << wine.error().message << "\n";
        if (!q2) std::cerr << q2.error().message << "\n";
        return kUnavailable;
    }
    if (!fs::is_regular_file(options.value().adapter)) {
        std::cerr << "adapter config not found: " << options.value().adapter << "\n";
        return kUnavailable;
    }

    auto runtime_base = prepare_runtime_base(options.value().runtime_base);
    if (!runtime_base) { std::cerr << runtime_base.error().message << "\n"; return kInternal; }
    auto lock = acquire_global_lock(runtime_base.value());
    if (!lock) { std::cerr << lock.error().message << "\n"; return kUnavailable; }
    cleanup_stale_sessions(runtime_base.value());

    const std::string session_id = q2dz::random_hex(16);
    auto session_dir = create_session_dir(runtime_base.value(), session_id);
    if (!session_dir) { std::cerr << session_dir.error().message << "\n"; return kInternal; }
    fs::path self = fs::canonical("/proc/self/exe");
    auto guardian = start_guardian(self, runtime_base.value(), session_dir.value(), session_id, options.value().shutdown_grace_ms);
    if (!guardian) {
        (void)q2dz::remove_session_dir_safely(runtime_base.value(), session_dir.value(), session_id);
        std::cerr << guardian.error().message << "\n";
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
