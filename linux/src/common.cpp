#include "common.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <poll.h>
#include <pwd.h>
#include <sstream>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cctype>
#include <csignal>
#include <stdexcept>
extern char** environ;

namespace fs = std::filesystem;

namespace q2dz {

UniqueFd::~UniqueFd() { reset(); }
UniqueFd::UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
}
int UniqueFd::release() noexcept { int out = fd_; fd_ = -1; return out; }
void UniqueFd::reset(int fd) noexcept { if (fd_ >= 0) ::close(fd_); fd_ = fd; }

std::string Endpoint::to_string() const { return host + ":" + std::to_string(port); }

Result<Endpoint> parse_loopback_endpoint(std::string_view value) {
    constexpr std::string_view prefix = "127.0.0.1:";
    if (!value.starts_with(prefix)) return Result<Endpoint>::fail("endpoint must use 127.0.0.1");
    const auto port_text = value.substr(prefix.size());
    unsigned port = 0;
    const auto* begin = port_text.data();
    const auto* end = begin + port_text.size();
    const auto parsed = std::from_chars(begin, end, port);
    if (parsed.ec != std::errc{} || parsed.ptr != end || port < 1024U || port > 65535U) {
        return Result<Endpoint>::fail("endpoint port must be an integer in [1024,65535]");
    }
    return Result<Endpoint>::ok(Endpoint{"127.0.0.1", static_cast<std::uint16_t>(port)});
}

Result<UniqueFd> make_pipe_read(int pipefd[2], bool nonblocking) {
    int flags = O_CLOEXEC;
    if (nonblocking) flags |= O_NONBLOCK;
    if (::pipe2(pipefd, flags) != 0) return Result<UniqueFd>::fail(errno_message("pipe2"));
    return Result<UniqueFd>::ok(UniqueFd(pipefd[0]));
}

Result<void> set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return Result<void>::fail(errno_message("fcntl(O_NONBLOCK)"));
    }
    return Result<void>::ok();
}

Result<void> set_cloexec(int fd, bool enabled) {
    int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0) return Result<void>::fail(errno_message("fcntl(F_GETFD)"));
    if (enabled) flags |= FD_CLOEXEC; else flags &= ~FD_CLOEXEC;
    if (::fcntl(fd, F_SETFD, flags) < 0) return Result<void>::fail(errno_message("fcntl(F_SETFD)"));
    return Result<void>::ok();
}

Result<void> write_all(int fd, std::span<const std::byte> data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto* ptr = reinterpret_cast<const char*>(data.data() + offset);
        const ssize_t n = ::write(fd, ptr, data.size() - offset);
        if (n < 0) {
            if (errno == EINTR) continue;
            return Result<void>::fail(errno_message("write"));
        }
        if (n == 0) return Result<void>::fail("write returned zero");
        offset += static_cast<std::size_t>(n);
    }
    return Result<void>::ok();
}
Result<void> write_all(int fd, std::string_view data) {
    return write_all(fd, std::as_bytes(std::span(data.data(), data.size())));
}

Result<std::string> read_all_fd(int fd, std::size_t limit) {
    std::string out;
    std::array<char, 8192> buffer{};
    while (true) {
        const ssize_t n = ::read(fd, buffer.data(), buffer.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            return Result<std::string>::fail(errno_message("read"));
        }
        if (n == 0) break;
        const auto count = static_cast<std::size_t>(n);
        if (out.size() > limit - std::min(limit, count)) {
            secure_clear(out);
            return Result<std::string>::fail("input exceeds size limit");
        }
        out.append(buffer.data(), count);
        if (out.size() > limit) {
            secure_clear(out);
            return Result<std::string>::fail("input exceeds size limit");
        }
    }
    return Result<std::string>::ok(std::move(out));
}

Result<std::string> read_file(const fs::path& path, std::size_t limit) {
    UniqueFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!fd) return Result<std::string>::fail(errno_message("open " + path.string()));
    return read_all_fd(fd.get(), limit);
}

Result<void> write_private_file(const fs::path& path, std::string_view data) {
    UniqueFd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!fd) return Result<void>::fail(errno_message("create " + path.string()));
    return write_all(fd.get(), data);
}

std::string random_hex(std::size_t bytes) {
    std::vector<unsigned char> data(bytes);
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t n = ::getrandom(data.data() + offset, data.size() - offset, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(errno_message("getrandom"));
        }
        offset += static_cast<std::size_t>(n);
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto b : data) out << std::setw(2) << static_cast<unsigned>(b);
    return out.str();
}

std::string errno_message(std::string_view operation) {
    return std::string(operation) + ": " + std::strerror(errno);
}

std::optional<fs::path> find_executable(std::string_view name) {
    if (name.find('/') != std::string_view::npos) {
        fs::path path(name);
        if (::access(path.c_str(), X_OK) == 0) return fs::absolute(path);
        return std::nullopt;
    }
    const char* path_env = std::getenv("PATH");
    if (!path_env) return std::nullopt;
    std::string paths(path_env);
    std::size_t pos = 0;
    while (pos <= paths.size()) {
        const auto next = paths.find(':', pos);
        const auto component = paths.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        fs::path candidate = (component.empty() ? fs::path(".") : fs::path(component)) / std::string(name);
        if (::access(candidate.c_str(), X_OK) == 0) return fs::absolute(candidate);
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return std::nullopt;
}

static std::string lower_copy(std::string_view input) {
    std::string out(input);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

bool contains_sensitive_key(std::string_view text) {
    const auto lower = lower_copy(text);
    constexpr std::array<std::string_view, 19> keys = {
        "credential", "password", "secret", "token", "authorization",
        "ice", "candidate", "sdp", "turn:", "turns:", "username",
        "fingerprint", "local-description", "remote-description", "session-description",
        "peer id", "peer_id", "peerid", "alias="
    };
    return std::any_of(keys.begin(), keys.end(), [&](std::string_view key) { return lower.find(key) != std::string::npos; });
}

std::string sanitize_log_line(std::string_view line) {
    if (contains_sensitive_key(line)) return "[q2dz] sensitive transport diagnostic suppressed";
    constexpr std::string_view sdp_fields = "vosiuepcbtrzkam";
    if (line.size() >= 2 && line[1] == '=' &&
        sdp_fields.find(static_cast<char>(std::tolower(static_cast<unsigned char>(line[0])))) != std::string_view::npos) {
        return "[q2dz] sensitive transport diagnostic suppressed";
    }
    constexpr std::size_t max = 2048;
    std::string out(line.substr(0, std::min(max, line.size())));
    for (char& c : out) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20U && c != '\t') c = ' ';
    }
    return out;
}

void secure_clear(std::string& value) noexcept {
    volatile char* ptr = value.empty() ? nullptr : value.data();
    for (std::size_t i = 0; i < value.size(); ++i) ptr[i] = 0;
    value.clear();
    value.shrink_to_fit();
}

Result<void> disable_core_dumps_and_ptrace() {
    struct rlimit limit{};
    limit.rlim_cur = 0;
    limit.rlim_max = 0;
    if (::setrlimit(RLIMIT_CORE, &limit) != 0) return Result<void>::fail(errno_message("setrlimit(RLIMIT_CORE)"));
    if (::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) return Result<void>::fail(errno_message("prctl(PR_SET_DUMPABLE)"));
    return Result<void>::ok();
}

std::uint64_t monotonic_millis() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::optional<std::uint64_t> proc_start_time(pid_t pid) {
    auto text = read_file("/proc/" + std::to_string(pid) + "/stat", 64U * 1024U);
    if (!text) return std::nullopt;
    const auto close = text.value().rfind(')');
    if (close == std::string::npos || close + 2 >= text.value().size()) return std::nullopt;
    std::istringstream in(text.value().substr(close + 2));
    std::string field;
    for (int index = 3; index <= 22; ++index) {
        if (!(in >> field)) return std::nullopt;
        if (index == 22) {
            std::uint64_t value = 0;
            const auto parsed = std::from_chars(field.data(), field.data() + field.size(), value);
            if (parsed.ec == std::errc{}) return value;
            return std::nullopt;
        }
    }
    return std::nullopt;
}

bool process_instance_alive(pid_t pid, std::uint64_t start_time) {
    if (::kill(pid, 0) != 0 && errno != EPERM) return false;
    const auto actual = proc_start_time(pid);
    return actual && *actual == start_time;
}

std::string process_comm(pid_t pid) {
    auto text = read_file("/proc/" + std::to_string(pid) + "/comm", 4096);
    if (!text) return {};
    while (!text.value().empty() && (text.value().back() == '\n' || text.value().back() == '\r')) text.value().pop_back();
    return text.value();
}

bool process_has_session_marker(pid_t pid, uid_t uid, std::string_view session_id) {
    struct stat st{};
    const std::string dir = "/proc/" + std::to_string(pid);
    if (::stat(dir.c_str(), &st) != 0 || st.st_uid != uid) return false;
    auto env = read_file(dir + "/environ", 4U * 1024U * 1024U);
    if (!env) return false;
    const std::string needle = "Q2DZ_SESSION_ID=" + std::string(session_id);
    std::size_t pos = 0;
    while (pos < env.value().size()) {
        const auto end = env.value().find('\0', pos);
        const auto item = std::string_view(env.value()).substr(pos, end == std::string::npos ? env.value().size() - pos : end - pos);
        if (item == needle) return true;
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return false;
}

Result<fs::path> canonical_owned_directory(const fs::path& path) {
    std::error_code ec;
    const auto canonical = fs::weakly_canonical(path, ec);
    if (ec) return Result<fs::path>::fail("cannot canonicalize " + path.string() + ": " + ec.message());
    struct stat st{};
    if (::stat(canonical.c_str(), &st) != 0) return Result<fs::path>::fail(errno_message("stat " + canonical.string()));
    if (!S_ISDIR(st.st_mode) || st.st_uid != ::getuid()) return Result<fs::path>::fail("directory is not owned by current user: " + canonical.string());
    return Result<fs::path>::ok(canonical);
}

Result<void> remove_session_dir_safely(const fs::path& base,
                                       const fs::path& session_dir,
                                       std::string_view session_id) {
    auto base_c = canonical_owned_directory(base);
    if (!base_c) return Result<void>::fail(base_c.error().message);
    auto dir_c = canonical_owned_directory(session_dir);
    if (!dir_c) return Result<void>::fail(dir_c.error().message);
    const auto base_str = base_c.value().string() + "/";
    if (!dir_c.value().string().starts_with(base_str)) return Result<void>::fail("refusing to remove directory outside runtime base");
    auto marker = read_file(dir_c.value() / ".q2dz-session", 256);
    if (!marker || marker.value() != session_id) return Result<void>::fail("session marker mismatch; refusing cleanup");
    std::error_code ec;
    fs::remove_all(dir_c.value(), ec);
    if (ec) return Result<void>::fail("remove_all failed: " + ec.message());
    return Result<void>::ok();
}

static bool child_environment_key_allowed(std::string_view key) {
    static constexpr auto allowed = std::to_array<std::string_view>({
        "HOME", "USER", "LOGNAME", "PATH", "SHELL", "PWD",
        "TMPDIR", "TMP", "TEMP", "LANG", "LANGUAGE", "TERM", "TZ",
        "LC_ALL", "LC_CTYPE", "LC_MESSAGES", "LC_NUMERIC", "LC_TIME", "LC_COLLATE",
        "LC_MONETARY", "LC_PAPER", "LC_NAME", "LC_ADDRESS", "LC_TELEPHONE",
        "LC_MEASUREMENT", "LC_IDENTIFICATION",
        "DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY", "DBUS_SESSION_BUS_ADDRESS",
        "XDG_RUNTIME_DIR", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME",
        "XDG_DATA_DIRS", "XDG_CONFIG_DIRS", "XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP",
        "PULSE_SERVER", "PIPEWIRE_REMOTE", "SSL_CERT_FILE", "SSL_CERT_DIR",
        "WINEPREFIX", "WINEARCH", "WINEDEBUG", "WINEDLLOVERRIDES", "WINEESYNC", "WINEFSYNC",
        "DRI_PRIME", "MESA_LOADER_DRIVER_OVERRIDE", "LIBGL_DRIVERS_PATH", "__GLX_VENDOR_LIBRARY_NAME",
        "VK_ICD_FILENAMES", "VK_DRIVER_FILES", "SDL_AUDIODRIVER", "SDL_VIDEODRIVER",
        "Q2DZ_TEST_MARKER", "Q2DZ_FAKE_LEGACY_MODE", "Q2DZ_FAKE_Q2_MODE"
    });
    return std::find(allowed.begin(), allowed.end(), key) != allowed.end();
}

std::vector<std::string> current_environment_with(const std::vector<std::pair<std::string,std::string>>& overrides) {
    std::vector<std::string> out;
    for (char** item = environ; item && *item; ++item) {
        std::string current(*item);
        const auto eq = current.find('=');
        const auto key = current.substr(0, eq);
        if (!child_environment_key_allowed(key)) continue;
        bool replaced = false;
        for (const auto& [override_key, override_value] : overrides) {
            (void)override_value;
            if (key == override_key) { replaced = true; break; }
        }
        if (!replaced) out.push_back(std::move(current));
    }
    for (const auto& [key, value] : overrides) out.push_back(key + "=" + value);
    return out;
}

} // namespace q2dz
