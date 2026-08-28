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
#include <optional>
#include <poll.h>
#include <set>
#include <string>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
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
constexpr int kGatewayFailure = 71;
constexpr int kReadyTimeout = 72;
constexpr int kRoomClosed = 73;
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
        "  Room configurations are read as one or two newline-delimited JSON objects from stdin\n"
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

q2dz::Result<std::vector<std::string>> parse_hub_input(std::string_view input) {
    std::vector<std::string> rooms;
    if (input.empty()) return q2dz::Result<std::vector<std::string>>::fail("hub input is empty");
    std::size_t offset = 0;
    while (offset < input.size()) {
        const auto newline = input.find('\n', offset);
        auto line = input.substr(offset, newline == std::string_view::npos ? input.size() - offset : newline - offset);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) return q2dz::Result<std::vector<std::string>>::fail("hub input contains an empty line");
        if (line.size() > kHubMaxLine) return q2dz::Result<std::vector<std::string>>::fail("hub input line exceeds 256 KiB");
        auto parsed = q2dz::json::parse(line);
        if (!parsed || !parsed.value().is_object()) return q2dz::Result<std::vector<std::string>>::fail("hub input line must be a JSON object");
        if (auto room = q2dz::json::extract_room_config(parsed.value()); !room) {
            return q2dz::Result<std::vector<std::string>>::fail("hub input line: " + room.error().message);
        }
        rooms.emplace_back(line);
        if (rooms.size() > kHubMaxRooms) return q2dz::Result<std::vector<std::string>>::fail("hub input accepts at most two rooms");
        if (newline == std::string_view::npos) break;
        offset = newline + 1;
    }
    if (rooms.empty()) return q2dz::Result<std::vector<std::string>>::fail("hub input is empty");
    return q2dz::Result<std::vector<std::string>>::ok(std::move(rooms));
}

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

q2dz::Result<pid_t> spawn_q2_hub(const fs::path& wine,
                                 const fs::path& q2,
                                 const std::vector<q2dz::Endpoint>& endpoints,
                                 const Options& options,
                                 const std::string& session_id) {
    if (endpoints.empty() || endpoints.size() > kHubMaxRooms) {
        return q2dz::Result<pid_t>::fail("hub has no usable gateway endpoints");
    }
    std::string source;
    for (const auto& endpoint : endpoints) {
        if (!source.empty()) source.push_back(' ');
        source += "quake2://" + endpoint.to_string();
    }
    const pid_t pid = ::fork();
    if (pid < 0) return q2dz::Result<pid_t>::fail(q2dz::errno_message("fork Q2PRO-X"));
    if (pid == 0) {
        (void)::prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
        if (::setpgid(0, 0) != 0) _exit(126);
        if (::chdir(q2.parent_path().c_str()) != 0) _exit(126);
        std::vector<std::pair<std::string, std::string>> overrides = {{"Q2DZ_SESSION_ID", session_id}};
        if (!options.wineprefix.empty()) overrides.emplace_back("WINEPREFIX", options.wineprefix);
        auto env_storage = q2dz::current_environment_with(overrides);
        std::vector<char*> envp;
        for (auto& item : env_storage) envp.push_back(item.data());
        envp.push_back(nullptr);
        std::vector<std::string> storage = {
            wine.string(), q2.string(), "+set", "game", options.game,
            "+set", "cl_serverbrowser_custom_source", source,
            "+serverbrowser_open_modern"
        };
        std::vector<char*> argv;
        for (auto& item : storage) argv.push_back(item.data());
        argv.push_back(nullptr);
        ::execve(wine.c_str(), argv.data(), envp.data());
        _exit(127);
    }
    if (::setpgid(pid, pid) != 0 && errno != EACCES) {
        (void)::kill(pid, SIGKILL);
        return q2dz::Result<pid_t>::fail(q2dz::errno_message("setpgid Q2PRO-X hub"));
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

struct HubGateway {
    Child child;
    std::string control_buffer;
    std::string diagnostic_buffer;
    std::optional<q2dz::Endpoint> endpoint;
    bool control_eof = false;
    bool terminal = false;
    std::string terminal_reason;
};

int child_exit_code(int status);

void stop_hub_gateway(HubGateway& gateway, int grace_ms, UniqueFd& guardian_control, std::string_view reason) {
    if (gateway.terminal) return;
    const pid_t process_group = gateway.child.process_group;
    if (gateway.child.pid > 0 || process_group > 1) {
        terminate_group(process_group, grace_ms, gateway.child.pid, -1);
        gateway.child.pid = -1;
    }
    if (process_group > 1) (void)q2dz::write_all(guardian_control.get(), "DROP " + std::to_string(process_group) + "\n");
    drain_diagnostics(gateway.child.diagnostics.get(), gateway.diagnostic_buffer);
    gateway.child.control.reset();
    gateway.child.diagnostics.reset();
    gateway.terminal_reason = std::string(reason);
    gateway.terminal = true;
    std::cerr << "HUB_GATEWAY_STOP reason=" << reason << "\n";
}

int finish_hub(const Options& options,
               std::vector<HubGateway>& gateways,
               pid_t& q2_pid,
               Guardian& guardian,
               std::string_view reason,
               int result_code) {
    for (auto& gateway : gateways) stop_hub_gateway(gateway, options.shutdown_grace_ms, guardian.control, reason);
    if (q2_pid > 0) {
        terminate_group(q2_pid, options.shutdown_grace_ms, -1, q2_pid);
        q2_pid = -1;
    }
    std::cerr << "SESSION_CLOSING reason=" << reason << "\n";
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

int run_hub(const Options& options,
            const fs::path& gateway,
            const fs::path& legacy,
            const fs::path& adapter,
            const fs::path& wine,
            const fs::path& q2,
            const fs::path& session_dir,
            const std::string& session_id,
            std::vector<std::string>& room_json,
            Guardian& guardian,
            Pipes& signal_pipe) {
    std::vector<HubGateway> gateways;
    gateways.reserve(room_json.size());
    for (auto& input : room_json) {
        auto child = spawn_gateway(gateway, legacy, adapter, session_dir, session_id, input);
        q2dz::secure_clear(input);
        if (!child) {
            std::cerr << "HUB_GATEWAY_START_FAILED reason=" << child.error().message << "\n";
            continue;
        }
        gateways.push_back(HubGateway{std::move(child.value()), {}, {}, std::nullopt, false, false, {}});
        const pid_t process_group = gateways.back().child.process_group;
        (void)q2dz::write_all(guardian.control.get(), "PGID " + std::to_string(process_group) + "\n");
    }
    if (gateways.empty()) {
        pid_t no_q2 = -1;
        return finish_hub(options, gateways, no_q2, guardian, "no-gateway", kGatewayFailure);
    }

    const auto ready_deadline = q2dz::monotonic_millis() + static_cast<std::uint64_t>(options.ready_timeout_ms);
    while (q2dz::monotonic_millis() < ready_deadline && signal_seen.load() == 0) {
        bool pending = false;
        for (std::size_t index = 0; index < gateways.size(); ++index) {
            auto& gateway_state = gateways[index];
            if (gateway_state.terminal) continue;
            if (!gateway_state.endpoint) pending = true;
            pollfd child_fds[2] = {
                {gateway_state.child.control.get(), POLLIN | POLLHUP, 0},
                {gateway_state.child.diagnostics.get(), POLLIN | POLLHUP, 0}
            };
            if (::poll(child_fds, 2, 0) < 0 && errno != EINTR) {
                stop_hub_gateway(gateway_state, options.shutdown_grace_ms, guardian.control, "poll-failed");
                continue;
            }
            if ((child_fds[1].revents & (POLLIN | POLLHUP)) != 0) {
                drain_diagnostics(gateway_state.child.diagnostics.get(), gateway_state.diagnostic_buffer);
            }
            if ((child_fds[0].revents & (POLLIN | POLLHUP)) != 0) {
                for (const auto& line : take_lines(gateway_state.child.control.get(), gateway_state.control_buffer, gateway_state.control_eof)) {
                    auto event = q2dz::parse_gateway_event(line);
                    if (!event) {
                        stop_hub_gateway(gateway_state, options.shutdown_grace_ms, guardian.control, "invalid-gateway-event");
                        break;
                    }
                    if (event.value().type == q2dz::GatewayEventType::TransportReady) {
                        auto endpoint = q2dz::ready_endpoint(event.value());
                        if (!endpoint) {
                            stop_hub_gateway(gateway_state, options.shutdown_grace_ms, guardian.control, "invalid-ready-endpoint");
                            break;
                        }
                        gateway_state.endpoint = endpoint.value();
                    } else if (event.value().type == q2dz::GatewayEventType::RoomClosed ||
                               event.value().type == q2dz::GatewayEventType::Fatal) {
                        const auto code = event.value().fields.find("code");
                        const bool probe_timeout = event.value().type == q2dz::GatewayEventType::Fatal &&
                            code != event.value().fields.end() && code->second == "q2-oob-timeout";
                        stop_hub_gateway(gateway_state, options.shutdown_grace_ms, guardian.control,
                                         event.value().type == q2dz::GatewayEventType::RoomClosed ? "room-closed-before-q2" :
                                         (probe_timeout ? "transport-ready-timeout" : "gateway-fatal"));
                        break;
                    }
                }
            }
            if (gateway_state.terminal) continue;
            int status = 0;
            if (gateway_state.child.pid > 0 && ::waitpid(gateway_state.child.pid, &status, WNOHANG) == gateway_state.child.pid) {
                gateway_state.child.pid = -1;
                stop_hub_gateway(gateway_state, options.shutdown_grace_ms, guardian.control, "gateway-exited-before-q2");
            } else if (gateway_state.control_eof) {
                stop_hub_gateway(gateway_state, options.shutdown_grace_ms, guardian.control, "gateway-control-eof-before-q2");
            }
        }
        if (!pending) break;
        pollfd signal_fd{signal_pipe.read.get(), POLLIN, 0};
        (void)::poll(&signal_fd, 1, 20);
    }
    for (auto& gateway_state : gateways) {
        if (!gateway_state.endpoint && !gateway_state.terminal) {
            stop_hub_gateway(gateway_state, options.shutdown_grace_ms, guardian.control, "transport-ready-timeout");
        }
    }
    if (signal_seen.load() != 0) {
        pid_t no_q2 = -1;
        return finish_hub(options, gateways, no_q2, guardian, "owner-stop-before-q2", 128 + signal_seen.load());
    }

    std::vector<q2dz::Endpoint> endpoints;
    for (const auto& gateway_state : gateways) {
        if (!gateway_state.terminal && gateway_state.endpoint) endpoints.push_back(*gateway_state.endpoint);
    }
    if (endpoints.empty()) {
        pid_t no_q2 = -1;
        int result_code = kReadyTimeout;
        std::string reason = "no-ready-gateway";
        for (const auto& gateway_state : gateways) {
            if (gateway_state.terminal_reason == "room-closed-before-q2") {
                result_code = kRoomClosed;
                reason = "room-closed-before-q2";
                break;
            }
            if (!gateway_state.terminal_reason.empty() &&
                gateway_state.terminal_reason != "transport-ready-timeout") {
                result_code = kGatewayFailure;
                reason = "gateway-failure-before-q2";
            }
        }
        return finish_hub(options, gateways, no_q2, guardian, reason, result_code);
    }
    auto started = spawn_q2_hub(wine, q2, endpoints, options, session_id);
    if (!started) {
        pid_t no_q2 = -1;
        return finish_hub(options, gateways, no_q2, guardian, "q2-spawn-failed", kQ2Failure);
    }
    pid_t q2_pid = started.value();
    (void)q2dz::write_all(guardian.control.get(), "PGID " + std::to_string(q2_pid) + "\n");
    std::cerr << "HUB_READY gateways=" << endpoints.size() << "\n";

    int result_code = 0;
    std::string closing_reason;
    while (q2_pid > 0 && result_code == 0 && signal_seen.load() == 0) {
        pollfd signal_fd{signal_pipe.read.get(), POLLIN, 0};
        const int signal_poll = ::poll(&signal_fd, 1, 200);
        if (signal_poll < 0 && errno != EINTR) {
            result_code = kInternal;
            closing_reason = "poll-failed";
            break;
        }
        for (auto& gateway_state : gateways) {
            if (gateway_state.terminal) continue;
            pollfd child_fds[2] = {
                {gateway_state.child.control.get(), POLLIN | POLLHUP, 0},
                {gateway_state.child.diagnostics.get(), POLLIN | POLLHUP, 0}
            };
            if (::poll(child_fds, 2, 0) < 0 && errno != EINTR) {
                stop_hub_gateway(gateway_state, options.shutdown_grace_ms, guardian.control, "poll-failed");
                continue;
            }
            if ((child_fds[1].revents & (POLLIN | POLLHUP)) != 0) {
                drain_diagnostics(gateway_state.child.diagnostics.get(), gateway_state.diagnostic_buffer);
            }
            if ((child_fds[0].revents & (POLLIN | POLLHUP)) != 0) {
                for (const auto& line : take_lines(gateway_state.child.control.get(), gateway_state.control_buffer, gateway_state.control_eof)) {
                    auto event = q2dz::parse_gateway_event(line);
                    if (!event) {
                        stop_hub_gateway(gateway_state, options.shutdown_grace_ms, guardian.control, "invalid-gateway-event");
                        break;
                    }
                    if (event.value().type == q2dz::GatewayEventType::RoomClosed ||
                        event.value().type == q2dz::GatewayEventType::Fatal) {
                        stop_hub_gateway(gateway_state, options.shutdown_grace_ms, guardian.control,
                                         event.value().type == q2dz::GatewayEventType::RoomClosed ? "room-closed" : "gateway-fatal");
                        break;
                    }
                }
            }
            if (gateway_state.terminal) continue;
            int gateway_status = 0;
            if (gateway_state.child.pid > 0 && ::waitpid(gateway_state.child.pid, &gateway_status, WNOHANG) == gateway_state.child.pid) {
                gateway_state.child.pid = -1;
                stop_hub_gateway(gateway_state, options.shutdown_grace_ms, guardian.control, "gateway-exited");
            } else if (gateway_state.control_eof) {
                stop_hub_gateway(gateway_state, options.shutdown_grace_ms, guardian.control, "gateway-control-eof");
            }
        }
        int q2_status = 0;
        if (::waitpid(q2_pid, &q2_status, WNOHANG) == q2_pid) {
            const int code = child_exit_code(q2_status);
            q2_pid = -1;
            result_code = code == 0 ? 0 : kQ2Failure;
            closing_reason = code == 0 ? "q2-normal-exit" : "q2-failed";
        }
        if (signal_poll > 0 && (signal_fd.revents & POLLIN) != 0) {
            result_code = 128 + signal_seen.load();
            closing_reason = "owner-stop";
        }
    }
    if (signal_seen.load() != 0 && result_code == 0) {
        result_code = 128 + signal_seen.load();
        closing_reason = "owner-stop";
    }
    return finish_hub(options, gateways, q2_pid, guardian,
                      closing_reason.empty() ? "hub-stopped" : closing_reason, result_code);
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

    auto room_input = q2dz::read_all_fd(STDIN_FILENO, 1024U * 1024U);
    if (!room_input) { std::cerr << room_input.error().message << "\n"; return kConfig; }
    std::string room_json = std::move(room_input.value());
    auto parsed_hub = parse_hub_input(room_json);
    q2dz::secure_clear(room_json);
    if (!parsed_hub) { std::cerr << parsed_hub.error().message << "\n"; return kConfig; }
    std::vector<std::string> hub_room_json = std::move(parsed_hub.value());

    auto gateway = resolve_program(options.value().gateway, {});
    auto legacy = resolve_program(options.value().legacy_gateway, {});
    auto wine = resolve_program(options.value().wine, {"wine64", "wine"});
    auto q2 = discover_q2(options.value());
    if (!gateway || !legacy || !wine || !q2) {
        for (auto& input : hub_room_json) q2dz::secure_clear(input);
        if (!gateway) std::cerr << gateway.error().message << "\n";
        if (!legacy) std::cerr << legacy.error().message << "\n";
        if (!wine) std::cerr << wine.error().message << "\n";
        if (!q2) std::cerr << q2.error().message << "\n";
        return kUnavailable;
    }
    if (!fs::is_regular_file(options.value().adapter)) {
        for (auto& input : hub_room_json) q2dz::secure_clear(input);
        std::cerr << "adapter config not found: " << options.value().adapter << "\n";
        return kUnavailable;
    }

    auto runtime_base = prepare_runtime_base(options.value().runtime_base);
    if (!runtime_base) { for (auto& input : hub_room_json) q2dz::secure_clear(input); std::cerr << runtime_base.error().message << "\n"; return kInternal; }
    auto lock = acquire_global_lock(runtime_base.value());
    if (!lock) { for (auto& input : hub_room_json) q2dz::secure_clear(input); std::cerr << lock.error().message << "\n"; return kUnavailable; }
    cleanup_stale_sessions(runtime_base.value());

    const std::string session_id = q2dz::random_hex(16);
    auto session_dir = create_session_dir(runtime_base.value(), session_id);
    if (!session_dir) { for (auto& input : hub_room_json) q2dz::secure_clear(input); std::cerr << session_dir.error().message << "\n"; return kInternal; }
    fs::path self = fs::canonical("/proc/self/exe");
    auto guardian = start_guardian(self, runtime_base.value(), session_dir.value(), session_id, options.value().shutdown_grace_ms);
    if (!guardian) {
        for (auto& input : hub_room_json) q2dz::secure_clear(input);
        (void)q2dz::remove_session_dir_safely(runtime_base.value(), session_dir.value(), session_id);
        std::cerr << guardian.error().message << "\n";
        return kInternal;
    }

    auto signal_pipe = make_pipe(true);
    if (!signal_pipe) { for (auto& input : hub_room_json) q2dz::secure_clear(input); return kInternal; }
    signal_pipe_write = signal_pipe.value().write.get();
    struct sigaction action{};
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    (void)::sigaction(SIGINT, &action, nullptr);
    (void)::sigaction(SIGTERM, &action, nullptr);
    (void)::sigaction(SIGHUP, &action, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    return run_hub(options.value(), gateway.value(), legacy.value(), options.value().adapter,
                   wine.value(), q2.value(), session_dir.value(), session_id,
                   hub_room_json, guardian.value(), signal_pipe.value());
}
