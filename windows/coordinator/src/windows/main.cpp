#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

#include "q2dz/child_protocol.hpp"
#include "win32_support.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

using q2dz::win::Handle;

constexpr int kSuccess = 0;
constexpr int kInternalFailure = 2;
constexpr int kProtocolFailure = 13;
constexpr int kCleanupFailure = 14;
constexpr int kQ2Failure = 74;
constexpr std::size_t kHubMaxRooms = 5;
constexpr std::size_t kHubMaxLineBytes = 256U * 1024U;
constexpr std::size_t kHubMaxQueuedCommands = 64;
constexpr std::size_t kHubCommandsPerCycle = 1;

Handle g_stop_event;

BOOL WINAPI ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT ||
        type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        if (g_stop_event) SetEvent(g_stop_event.get());
        return TRUE;
    }
    return FALSE;
}

struct Options {
    fs::path gateway;
    std::optional<fs::path> q2;
    std::optional<fs::path> root;
    DWORD ready_ms = 60000;
    DWORD graceful_ms = 2500;
};

[[noreturn]] void usage() {
    throw std::runtime_error(
        "usage: q2dz-session --gateway <exe> [--q2 <Q2PRO-X.exe>] "
        "[--session-root <dir>] [--ready-timeout-ms N] [--graceful-timeout-ms N]");
}

DWORD parse_ms(const wchar_t* text) {
    wchar_t* end = nullptr;
    const unsigned long value = wcstoul(text, &end, 10);
    if (!text[0] || *end || value < 100 || value > 600000) usage();
    return static_cast<DWORD>(value);
}

Options parse_options(int argc, wchar_t** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        auto value = [&]() -> const wchar_t* {
            if (++index >= argc) usage();
            return argv[index];
        };
        if (argument == L"--gateway") options.gateway = value();
        else if (argument == L"--q2") options.q2 = fs::path(value());
        else if (argument == L"--session-root") options.root = fs::path(value());
        else if (argument == L"--ready-timeout-ms") options.ready_ms = parse_ms(value());
        else if (argument == L"--graceful-timeout-ms") options.graceful_ms = parse_ms(value());
        else if (argument == L"--help") usage();
        else usage();
    }
    if (options.gateway.empty()) usage();
    return options;
}

void secure_clear_string(std::string& value) noexcept {
    if (!value.empty()) SecureZeroMemory(value.data(), value.size());
    value.clear();
    value.shrink_to_fit();
}

bool valid_utf8(std::string_view value) {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        std::size_t length = 0;
        std::uint32_t codepoint = 0;
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        if (first >= 0xc2U && first <= 0xdfU) {
            length = 2;
            codepoint = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            length = 3;
            codepoint = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            length = 4;
            codepoint = first & 0x07U;
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
            codepoint > 0x10ffffU || (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return false;
        }
        index += length;
    }
    return true;
}

bool safe_room_id(std::string_view value) {
    if (value.empty() || value.size() > 255U) return false;
    for (const char raw : value) {
        const auto character = static_cast<unsigned char>(raw);
        if (!(std::isalnum(character) || character == '.' || character == '-' || character == '_')) {
            return false;
        }
    }
    return true;
}

bool json_shape(std::string_view value) {
    auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return false;
    const auto last = value.find_last_not_of(" \t\r\n");
    return value[first] == '{' && value[last] == '}';
}

struct SlotCommand {
    std::size_t slot = 0;
    std::optional<std::string> room_id;
    std::optional<std::vector<unsigned char>> config;

    SlotCommand() = default;
    SlotCommand(const SlotCommand&) = delete;
    SlotCommand& operator=(const SlotCommand&) = delete;
    SlotCommand(SlotCommand&& other) noexcept
        : slot(other.slot), room_id(std::move(other.room_id)), config(std::move(other.config)) {
        if (other.config) q2dz::win::secure_clear(*other.config);
    }
    SlotCommand& operator=(SlotCommand&& other) noexcept {
        if (this != &other) {
            if (config) q2dz::win::secure_clear(*config);
            slot = other.slot;
            room_id = std::move(other.room_id);
            config = std::move(other.config);
            if (other.config) q2dz::win::secure_clear(*other.config);
        }
        return *this;
    }
    ~SlotCommand() {
        if (config) q2dz::win::secure_clear(*config);
    }
};

q2dz::win::Handle make_event(bool manual_reset = true) {
    return Handle(CreateEventW(nullptr, manual_reset ? TRUE : FALSE, FALSE, nullptr));
}

bool parse_slot_line(std::string_view line, SlotCommand& command) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty() || line.size() + 1U > kHubMaxLineBytes) {
        return false;
    }
    if (!valid_utf8(line) || line.size() < 3U || line[0] < '0' ||
        line[0] >= static_cast<char>('0' + kHubMaxRooms) || line[1] != ' ') {
        return false;
    }
    command.slot = static_cast<std::size_t>(line[0] - '0');
    if (line.substr(2) == "-") return true;

    const auto separator = line.find(' ', 2);
    if (separator == std::string_view::npos) return false;
    const auto room = line.substr(2, separator - 2);
    if (room == "-" || !safe_room_id(room)) return false;
    if (separator + 1U >= line.size()) return false;
    const auto json = line.substr(separator + 1U);
    if (!valid_utf8(json) || !json_shape(json)) return false;
    command.room_id = std::string(room);
    command.config.emplace(json.begin(), json.end());
    return true;
}

class SlotCommandReader {
public:
    bool feed(std::string_view bytes, std::deque<SlotCommand>& output) {
        if (failed_) return false;
        buffer_.append(bytes.data(), bytes.size());
        for (;;) {
            const auto newline = buffer_.find('\n');
            if (newline == std::string::npos) {
                if (buffer_.size() > kHubMaxLineBytes) {
                    fail();
                    return false;
                }
                return true;
            }
            std::string line = buffer_.substr(0, newline);
            std::string remainder = buffer_.substr(newline + 1U);
            secure_clear_string(buffer_);
            // Copy before wiping: a moved-from small string can retain its
            // bytes while reporting size zero, so it cannot be scrubbed
            // reliably after a move.
            buffer_ = remainder;
            secure_clear_string(remainder);
            SlotCommand command;
            if (!parse_slot_line(line, command)) {
                secure_clear_string(line);
                clear(output);
                fail();
                return false;
            }
            secure_clear_string(line);
            if (output.size() >= kHubMaxQueuedCommands) {
                clear(output);
                fail();
                return false;
            }
            output.push_back(std::move(command));
        }
    }

    bool finish() {
        if (failed_) return false;
        if (!buffer_.empty()) {
            fail();
            return false;
        }
        return true;
    }

    void clear(std::deque<SlotCommand>& commands) noexcept {
        while (!commands.empty()) commands.pop_front();
    }

    void abort() noexcept {
        failed_ = true;
        secure_clear_string(buffer_);
    }

    ~SlotCommandReader() { secure_clear_string(buffer_); }

private:
    void fail() noexcept {
        failed_ = true;
        secure_clear_string(buffer_);
    }

    std::string buffer_;
    bool failed_ = false;
};

class ControlQueue {
public:
    ControlQueue() : event_(make_event()) {}

    [[nodiscard]] HANDLE event() const noexcept { return event_.get(); }

    bool feed(std::string_view bytes) {
        std::lock_guard lock(mutex_);
        if (!reader_.feed(bytes, commands_)) {
            protocol_error_ = true;
        }
        SetEvent(event_.get());
        return !protocol_error_;
    }

    void finish() {
        std::lock_guard lock(mutex_);
        if (protocol_error_) {
            SetEvent(event_.get());
        } else if (!reader_.finish()) {
            protocol_error_ = true;
        } else {
            clean_eof_ = true;
        }
        SetEvent(event_.get());
    }

    void fail() {
        std::lock_guard lock(mutex_);
        protocol_error_ = true;
        reader_.clear(commands_);
        reader_.abort();
        SetEvent(event_.get());
    }

    std::vector<SlotCommand> drain(std::size_t limit,
                                   bool& clean_eof,
                                   bool& protocol_error,
                                   bool& commands_pending) {
        std::lock_guard lock(mutex_);
        std::vector<SlotCommand> result;
        result.reserve((std::min)(commands_.size(), limit));
        while (!commands_.empty() && result.size() < limit) {
            result.push_back(std::move(commands_.front()));
            commands_.pop_front();
        }
        commands_pending = !commands_.empty();
        clean_eof = clean_eof_;
        protocol_error = protocol_error_;
        if (commands_.empty() && !clean_eof_ && !protocol_error_) ResetEvent(event_.get());
        return result;
    }

private:
    std::mutex mutex_;
    SlotCommandReader reader_;
    std::deque<SlotCommand> commands_;
    Handle event_;
    bool clean_eof_ = false;
    bool protocol_error_ = false;
};

void control_reader(HANDLE input, ControlQueue& queue) {
    std::array<char, 8192> buffer{};
    struct BufferWipe {
        std::array<char, 8192>& value;
        ~BufferWipe() noexcept { SecureZeroMemory(value.data(), value.size()); }
    } wipe{buffer};
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
            if (GetLastError() != ERROR_BROKEN_PIPE && GetLastError() != ERROR_HANDLE_EOF) {
                queue.fail();
            }
            break;
        }
        if (count == 0) break;
        if (!queue.feed(std::string_view(buffer.data(), count))) break;
    }
    queue.finish();
}

class Socket {
public:
    Socket() noexcept = default;
    explicit Socket(SOCKET value) noexcept : value_(value) {}
    ~Socket() { reset(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : value_(other.release()) {}
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    [[nodiscard]] SOCKET get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ != INVALID_SOCKET; }
    SOCKET release() noexcept {
        const SOCKET value = value_;
        value_ = INVALID_SOCKET;
        return value;
    }
    void reset(SOCKET value = INVALID_SOCKET) noexcept {
        if (value_ != INVALID_SOCKET) closesocket(value_);
        value_ = value;
    }

private:
    SOCKET value_ = INVALID_SOCKET;
};

class CustomSource {
public:
    bool start() {
        if (worker_.joinable()) return false;
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
        winsock_started_ = true;
        Socket listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (!listener) return fail_start();
        u_long nonblocking = 1;
        if (ioctlsocket(listener.get(), FIONBIO, &nonblocking) == SOCKET_ERROR) return fail_start();
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(0);
        if (bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
            listen(listener.get(), 4) == SOCKET_ERROR) {
            return fail_start();
        }
        int length = sizeof(address);
        if (getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &length) == SOCKET_ERROR ||
            ntohs(address.sin_port) < 1024) {
            return fail_start();
        }
        Handle stop_event = make_event();
        Handle failure_event = make_event();
        if (!stop_event || !failure_event) return fail_start();
        listener_ = std::move(listener);
        stop_event_ = std::move(stop_event);
        failure_event_ = std::move(failure_event);
        url_ = L"http://127.0.0.1:" + std::to_wstring(ntohs(address.sin_port)) + L"/servers";
        try {
            worker_ = std::jthread([this](std::stop_token) { serve(); });
        } catch (...) {
            listener_.reset();
            stop_event_.reset();
            failure_event_.reset();
            url_.clear();
            WSACleanup();
            winsock_started_ = false;
            return false;
        }
        return true;
    }

    [[nodiscard]] std::wstring url() const { return url_; }
    [[nodiscard]] HANDLE failure_event() const noexcept { return failure_event_.get(); }
    [[nodiscard]] bool failed() const noexcept { return failed_.load(); }

    void publish(const std::vector<q2dz::LoopbackEndpoint>& endpoints) {
        std::string body;
        for (const auto& endpoint : endpoints) {
            body += endpoint.text();
            body.push_back('\n');
        }
        std::lock_guard lock(mutex_);
        body_ = std::move(body);
    }

    void stop() noexcept {
        if (!winsock_started_) return;
        if (stop_event_) SetEvent(stop_event_.get());
        {
            std::lock_guard lock(active_mutex_);
            if (active_client_ != INVALID_SOCKET) shutdown(active_client_, SD_BOTH);
        }
        if (worker_.joinable()) worker_.join();
        listener_.reset();
        stop_event_.reset();
        failure_event_.reset();
        if (winsock_started_) WSACleanup();
        winsock_started_ = false;
    }

    ~CustomSource() { stop(); }
    CustomSource() = default;
    CustomSource(const CustomSource&) = delete;
    CustomSource& operator=(const CustomSource&) = delete;

private:
    bool fail_start() noexcept {
        listener_.reset();
        stop_event_.reset();
        failure_event_.reset();
        url_.clear();
        if (winsock_started_) WSACleanup();
        winsock_started_ = false;
        return false;
    }

    static std::size_t header_end(std::string_view request) {
        const auto crlf = request.find("\r\n\r\n");
        if (crlf != std::string_view::npos) return crlf + 4U;
        const auto lf = request.find("\n\n");
        return lf == std::string_view::npos ? std::string_view::npos : lf + 2U;
    }

    bool stopping() const noexcept {
        return stop_event_ && WaitForSingleObject(stop_event_.get(), 0) == WAIT_OBJECT_0;
    }

    bool send_all(SOCKET client, std::string_view response) {
        std::size_t offset = 0;
        const ULONGLONG deadline = GetTickCount64() + 1000U;
        while (offset < response.size()) {
            const int count = send(client, response.data() + offset,
                                   static_cast<int>(std::min<std::size_t>(response.size() - offset, INT_MAX)), 0);
            if (count > 0) {
                offset += static_cast<std::size_t>(count);
                continue;
            }
            const int error = WSAGetLastError();
            if (count == SOCKET_ERROR && (error == WSAEINTR || error == WSAEWOULDBLOCK)) {
                if (GetTickCount64() >= deadline || stopping()) return false;
                WSAPOLLFD pollfd{client, POLLWRNORM, 0};
                (void)WSAPoll(&pollfd, 1, 50);
                continue;
            }
            return false;
        }
        return true;
    }

    bool serve_client(SOCKET raw_client) {
        Socket client(raw_client);
        {
            std::lock_guard lock(active_mutex_);
            active_client_ = client.get();
        }
        struct ActiveClientGuard {
            CustomSource* source;
            ~ActiveClientGuard() {
                std::lock_guard lock(source->active_mutex_);
                source->active_client_ = INVALID_SOCKET;
            }
        } active{this};

        std::string request;
        const ULONGLONG deadline = GetTickCount64() + 1000U;
        while (header_end(request) == std::string_view::npos) {
            if (stopping() || GetTickCount64() >= deadline) return true;
            WSAPOLLFD pollfd{client.get(), POLLRDNORM, 0};
            const int result = WSAPoll(&pollfd, 1, 50);
            if (result == SOCKET_ERROR) {
                const int error = WSAGetLastError();
                if (error == WSAEINTR) continue;
                return true;
            }
            if (stopping()) return true;
            if (result == 0 || (pollfd.revents & (POLLRDNORM | POLLERR | POLLHUP)) == 0) continue;
            char buffer[2048];
            const int count = recv(client.get(), buffer, sizeof(buffer), 0);
            if (count == 0) return true;
            if (count == SOCKET_ERROR) {
                const int error = WSAGetLastError();
                if (error == WSAEINTR || error == WSAEWOULDBLOCK) continue;
                return true;
            }
            request.append(buffer, static_cast<std::size_t>(count));
            if (request.size() > 8U * 1024U) return true;
        }
        const auto newline = request.find('\n');
        std::string_view first_line(request.data(), newline == std::string::npos ? request.size() : newline);
        if (!first_line.empty() && first_line.back() == '\r') first_line.remove_suffix(1);
        std::string body;
        {
            std::lock_guard lock(mutex_);
            body = body_;
        }
        std::string response;
        if (first_line == "GET /servers HTTP/1.1") {
            response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
                       "Cache-Control: no-store\r\nContent-Length: " + std::to_string(body.size()) +
                       "\r\nConnection: close\r\n\r\n" + body;
        } else {
            response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        }
        (void)send_all(client.get(), response);
        return true;
    }

    void serve() noexcept {
        while (!stopping()) {
            WSAPOLLFD pollfd{listener_.get(), POLLRDNORM, 0};
            const int result = WSAPoll(&pollfd, 1, 100);
            if (result == SOCKET_ERROR) {
                const int error = WSAGetLastError();
                if (error == WSAEINTR || stopping()) continue;
                failed_.store(true);
                SetEvent(failure_event_.get());
                return;
            }
            if (stopping()) return;
            if (result == 0) continue;
            if ((pollfd.revents & (POLLERR | POLLHUP)) != 0) {
                failed_.store(true);
                SetEvent(failure_event_.get());
                return;
            }
            if ((pollfd.revents & POLLRDNORM) == 0) continue;
            const SOCKET client = accept(listener_.get(), nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                const int error = WSAGetLastError();
                if (error == WSAEINTR || error == WSAEWOULDBLOCK) continue;
                if (stopping()) return;
                failed_.store(true);
                SetEvent(failure_event_.get());
                return;
            }
            u_long nonblocking = 1;
            if (ioctlsocket(client, FIONBIO, &nonblocking) == SOCKET_ERROR) {
                closesocket(client);
                continue;
            }
            (void)serve_client(client);
        }
    }

    Socket listener_;
    Handle stop_event_;
    Handle failure_event_;
    std::jthread worker_;
    std::mutex mutex_;
    std::string body_;
    std::wstring url_;
    std::atomic<bool> failed_{false};
    std::mutex active_mutex_;
    SOCKET active_client_ = INVALID_SOCKET;
    bool winsock_started_ = false;
};

struct Queue {
    std::mutex mutex;
    q2dz::ChildEventFramer framer;
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    bool finished = false;

    bool feed(std::string_view bytes) {
        std::lock_guard lock(mutex);
        const bool keep = framer.feed(bytes);
        SetEvent(event.get());
        return keep;
    }

    void finish() {
        std::lock_guard lock(mutex);
        (void)framer.finish();
        finished = true;
        SetEvent(event.get());
    }

    std::deque<std::string> drain() {
        std::lock_guard lock(mutex);
        auto result = framer.drain();
        if (result.empty() && !finished) ResetEvent(event.get());
        return result;
    }

    bool is_finished() {
        std::lock_guard lock(mutex);
        return finished;
    }
};

void event_reader(HANDLE handle, Queue& queue) {
    char buffer[1024];
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(handle, buffer, sizeof(buffer), &count, nullptr) || count == 0) break;
        if (!queue.feed(std::string_view(buffer, count))) break;
    }
    queue.finish();
}

HANDLE native_thread_handle(std::jthread& thread) noexcept {
    return reinterpret_cast<HANDLE>(thread.native_handle());
}

void terminate_unassigned(q2dz::win::ChildProcess& child) noexcept {
    if (child.process) {
        (void)TerminateProcess(child.process.get(), 3);
        (void)WaitForSingleObject(child.process.get(), 5000);
    }
    child.thread.reset();
    child.process.reset();
}

void assign_job(HANDLE job, q2dz::win::ChildProcess& child, const wchar_t* role) {
    wchar_t value[16]{};
    const DWORD length = GetEnvironmentVariableW(L"Q2DZ_TEST_FAIL_ASSIGNMENT", value, static_cast<DWORD>(std::size(value)));
    const bool injected = length > 0 && length < std::size(value) &&
                          (std::wstring(value, length) == L"1" || std::wstring(value, length) == role);
    if (injected || !AssignProcessToJobObject(job, child.process.get())) {
        const DWORD error = injected ? ERROR_ACCESS_DENIED : GetLastError();
        terminate_unassigned(child);
        throw std::runtime_error("AssignProcessToJobObject failed: " + std::to_string(error));
    }
}

Handle make_kill_job() {
    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) throw std::runtime_error("CreateJobObject failed");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                                               JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
        throw std::runtime_error("SetInformationJobObject failed");
    }
    return job;
}

void resume_process(HANDLE thread) {
    if (ResumeThread(thread) == static_cast<DWORD>(-1)) throw std::runtime_error("ResumeThread failed");
}

void spawn_cleanup_guardian(const fs::path& self, const fs::path& root, const fs::path& session) {
    Handle owner(OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId()));
    if (!owner || !SetHandleInformation(owner.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
        throw std::runtime_error("cleanup guardian owner handle failed");
    }
    auto nullin = q2dz::win::open_null(false);
    auto nullout = q2dz::win::open_null(true);
    auto nullerr = q2dz::win::open_null(true);
    const std::vector<std::wstring> arguments = {
        L"--cleanup-session", root.wstring(), session.wstring(), L"--owner-handle",
        std::to_wstring(reinterpret_cast<std::uintptr_t>(owner.get()))
    };
    const std::vector<HANDLE> inherited = {owner.get(), nullin.get(), nullout.get(), nullerr.get()};
    auto guardian = q2dz::win::launch_suspended(self, arguments, self.parent_path(),
                                                nullin.get(), nullout.get(), nullerr.get(),
                                                inherited, CREATE_NO_WINDOW);
    try {
        resume_process(guardian.thread.get());
    } catch (...) {
        terminate_unassigned(guardian);
        throw;
    }
    guardian.thread.reset();
    guardian.process.reset();
}

struct HubGateway {
    fs::path directory;
    Handle job;
    q2dz::win::Pipe owner;
    Handle event_read;
    q2dz::win::ChildProcess process;
    std::optional<std::jthread> reader;
    Queue events;
    std::optional<q2dz::LoopbackEndpoint> endpoint;
    bool ready = false;
    ULONGLONG ready_deadline = 0;
};

void stop_hub_gateway(HubGateway& gateway, DWORD graceful_ms) noexcept {
    gateway.owner.write.reset();
    if (gateway.process.process) (void)WaitForSingleObject(gateway.process.process.get(), graceful_ms);
    gateway.job.reset();
    if (gateway.process.process) (void)WaitForSingleObject(gateway.process.process.get(), 5000);
    if (gateway.reader) {
        gateway.reader->request_stop();
        (void)CancelSynchronousIo(native_thread_handle(*gateway.reader));
        if (gateway.reader->joinable()) gateway.reader->join();
        gateway.reader.reset();
    }
    (void)gateway.events.drain();
    gateway.event_read.reset();
    gateway.process.thread.reset();
    gateway.process.process.reset();
    try {
        (void)q2dz::win::remove_tree_with_retry(gateway.directory, 2000);
    } catch (...) {
    }
}

struct LiveSlot {
    std::string room_id;
    unsigned post_ready_failures = 0;
    unsigned dedicated_restarts = 0;
    bool tombstone = false;
    std::uint64_t generation = 0;
    std::unique_ptr<HubGateway> gateway;
};

bool is_dedicated_room(std::string_view room_id) {
    return room_id.starts_with("room.q2ded.");
}

std::vector<q2dz::LoopbackEndpoint> ready_endpoints(const std::array<LiveSlot, kHubMaxRooms>& slots) {
    std::vector<q2dz::LoopbackEndpoint> result;
    for (const auto& slot : slots) {
        if (slot.gateway && slot.gateway->ready && slot.gateway->endpoint) result.push_back(*slot.gateway->endpoint);
    }
    return result;
}

void publish_source(CustomSource& source, const std::array<LiveSlot, kHubMaxRooms>& slots) {
    source.publish(ready_endpoints(slots));
}

void remove_slot_gateway(LiveSlot& slot, DWORD graceful_ms) noexcept {
    if (!slot.gateway) return;
    stop_hub_gateway(*slot.gateway, graceful_ms);
    slot.gateway.reset();
}

void unpublish_slot_gateway(LiveSlot& slot,
                            CustomSource& source,
                            const std::array<LiveSlot, kHubMaxRooms>& slots) {
    if (slot.gateway) {
        slot.gateway->ready = false;
        slot.gateway->endpoint.reset();
    }
    publish_source(source, slots);
}

bool start_slot_gateway(LiveSlot& slot,
                        std::size_t slot_index,
                        HANDLE root_job,
                        DWORD ready_ms,
                        DWORD graceful_ms,
                        const fs::path& gateway_path,
                        const fs::path& session,
                        std::vector<unsigned char>& config) {
    auto room = std::make_unique<HubGateway>();
    try {
        room->job = make_kill_job();
        ++slot.generation;
        room->directory = session / (L"room-" + std::to_wstring(slot_index) + L"-" +
                                     std::to_wstring(slot.generation));
        room->ready_deadline = GetTickCount64() + ready_ms;
        fs::create_directories(room->directory / L"webview2");
        auto config_pipe = q2dz::win::make_pipe(true, false);
        auto event_pipe = q2dz::win::make_pipe(false, true);
        room->owner = q2dz::win::make_pipe(true, false);
        auto nullerr = q2dz::win::open_null(true);
        const std::vector<std::wstring> arguments = {
            L"--managed-session", L"--udp-port", L"0", L"--owner-handle",
            std::to_wstring(reinterpret_cast<std::uintptr_t>(room->owner.read.get())),
            L"--user-data-dir", (room->directory / L"webview2").wstring()
        };
        const std::vector<HANDLE> inherited = {
            config_pipe.read.get(), event_pipe.write.get(), room->owner.read.get(), nullerr.get()
        };
        room->process = q2dz::win::launch_suspended(
            gateway_path, arguments, gateway_path.parent_path(), config_pipe.read.get(),
            event_pipe.write.get(), nullerr.get(), inherited, CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP);
        assign_job(root_job, room->process, L"gateway");
        assign_job(room->job.get(), room->process, L"gateway-room");
        config_pipe.read.reset();
        event_pipe.write.reset();
        room->owner.read.reset();
        resume_process(room->process.thread.get());
        room->process.thread.reset();
        q2dz::win::write_all(config_pipe.write.get(), config.data(), config.size());
        config_pipe.write.reset();
        q2dz::win::secure_clear(config);
        room->event_read = std::move(event_pipe.read);
        if (!room->event_read || !room->events.event) throw std::runtime_error("gateway event pipe unavailable");
        room->reader.emplace([handle = room->event_read.get(), queue = &room->events](std::stop_token) {
            event_reader(handle, *queue);
        });
        slot.gateway = std::move(room);
        return true;
    } catch (...) {
        q2dz::win::secure_clear(config);
        if (room) stop_hub_gateway(*room, graceful_ms);
        throw;
    }
}

void mark_slot_failure(LiveSlot& slot,
                       DWORD graceful_ms,
                       CustomSource& source,
                       const std::array<LiveSlot, kHubMaxRooms>& slots,
                       bool room_closed) {
    const bool was_ready = slot.gateway && slot.gateway->ready;
    unpublish_slot_gateway(slot, source, slots);
    remove_slot_gateway(slot, graceful_ms);
    if (is_dedicated_room(slot.room_id)) {
        slot.tombstone = slot.dedicated_restarts >= 2;
    } else if (room_closed || !was_ready || slot.post_ready_failures != 0) {
        slot.tombstone = true;
    } else {
        slot.post_ready_failures = 1;
    }
    publish_source(source, slots);
}

bool consume_gateway_events(LiveSlot& slot,
                            DWORD graceful_ms,
                            CustomSource& source,
                            const std::array<LiveSlot, kHubMaxRooms>& slots) {
    if (!slot.gateway) return false;
    for (auto& line : slot.gateway->events.drain()) {
        const auto parsed = q2dz::parse_child_event(line);
        if (!parsed.event) {
            mark_slot_failure(slot, graceful_ms, source, slots, false);
            return true;
        }
        const auto& event = *parsed.event;
        if (event.type == q2dz::ChildEventType::transport_ready) {
            if (slot.gateway->ready) {
                mark_slot_failure(slot, graceful_ms, source, slots, false);
                return true;
            }
            const auto value = event.fields.find("udp");
            if (value == event.fields.end()) {
                mark_slot_failure(slot, graceful_ms, source, slots, false);
                return true;
            }
            slot.gateway->endpoint = q2dz::parse_loopback_endpoint(value->second);
            if (!slot.gateway->endpoint) {
                mark_slot_failure(slot, graceful_ms, source, slots, false);
                return true;
            }
            slot.gateway->ready = true;
            publish_source(source, slots);
        } else if (event.type == q2dz::ChildEventType::room_closed) {
            mark_slot_failure(slot, graceful_ms, source, slots, true);
            return true;
        } else if (event.type == q2dz::ChildEventType::fatal) {
            mark_slot_failure(slot, graceful_ms, source, slots, false);
            return true;
        }
    }
    if (slot.gateway && slot.gateway->events.is_finished() && !slot.gateway->process.process) {
        mark_slot_failure(slot, graceful_ms, source, slots, false);
        return true;
    }
    return false;
}

bool process_slot(LiveSlot& slot,
                  const Options& options,
                  CustomSource& source,
                  const std::array<LiveSlot, kHubMaxRooms>& slots) {
    if (!slot.gateway) return false;
    if (consume_gateway_events(slot, options.graceful_ms, source, slots)) return true;
    if (!slot.gateway) return true;
    if (WaitForSingleObject(slot.gateway->process.process.get(), 0) == WAIT_OBJECT_0) {
        // A gateway can close immediately after writing a terminal event.
        // Give the reader a short bounded window to drain that event before
        // classifying the process as a pre-ready failure.
        for (int attempt = 0; attempt < 20 && slot.gateway; ++attempt) {
            if (consume_gateway_events(slot, options.graceful_ms, source, slots)) return true;
            if (slot.gateway->events.is_finished()) break;
            (void)WaitForSingleObject(slot.gateway->events.event.get(), 5);
        }
        if (slot.gateway) {
            mark_slot_failure(slot, options.graceful_ms, source, slots, false);
        }
        return true;
    }
    if (slot.gateway->events.is_finished()) {
        mark_slot_failure(slot, options.graceful_ms, source, slots, false);
        return true;
    }
    if (!slot.gateway->ready && GetTickCount64() >= slot.gateway->ready_deadline) {
        mark_slot_failure(slot, options.graceful_ms, source, slots, false);
        return true;
    }
    return false;
}

void process_slot_command(SlotCommand& command,
                          std::array<LiveSlot, kHubMaxRooms>& slots,
                          HANDLE root_job,
                          const Options& options,
                          const fs::path& gateway,
                          const fs::path& session,
                          CustomSource& source) {
    LiveSlot& slot = slots[command.slot];
    if (!command.room_id) {
        unpublish_slot_gateway(slot, source, slots);
        remove_slot_gateway(slot, options.graceful_ms);
        slot.room_id.clear();
        slot.post_ready_failures = 0;
        slot.dedicated_restarts = 0;
        slot.tombstone = false;
        publish_source(source, slots);
        return;
    }

    const std::string requested = *command.room_id;
    const bool same_room = slot.room_id == requested;
    if (same_room && (slot.gateway || slot.tombstone)) {
        if (command.config) q2dz::win::secure_clear(*command.config);
        return;
    }
    if (!same_room) {
        unpublish_slot_gateway(slot, source, slots);
        remove_slot_gateway(slot, options.graceful_ms);
        slot.room_id = requested;
        slot.post_ready_failures = 0;
        slot.dedicated_restarts = 0;
        slot.tombstone = false;
    }
    if (!command.config) {
        slot.tombstone = true;
        publish_source(source, slots);
        return;
    }
    if (same_room && is_dedicated_room(slot.room_id)) ++slot.dedicated_restarts;
    try {
        if (!start_slot_gateway(slot, command.slot, root_job, options.ready_ms, options.graceful_ms,
                                gateway, session, *command.config)) {
            slot.tombstone = !is_dedicated_room(slot.room_id) || slot.dedicated_restarts >= 2;
        }
    } catch (...) {
        slot.tombstone = !is_dedicated_room(slot.room_id) || slot.dedicated_restarts >= 2;
    }
    q2dz::win::secure_clear(*command.config);
    publish_source(source, slots);
}

std::vector<HANDLE> wait_handles(const std::array<LiveSlot, kHubMaxRooms>& slots,
                                 CustomSource& source,
                                 HANDLE control,
                                 HANDLE q2) {
    std::vector<HANDLE> handles = {g_stop_event.get(), source.failure_event()};
    if (q2) handles.push_back(q2);
    handles.push_back(control);
    for (const auto& slot : slots) {
        if (!slot.gateway) continue;
        if (slot.gateway->events.event) handles.push_back(slot.gateway->events.event.get());
        if (slot.gateway->process.process) handles.push_back(slot.gateway->process.process.get());
    }
    if (handles.size() >= MAXIMUM_WAIT_OBJECTS) throw std::runtime_error("hub wait handle limit exceeded");
    return handles;
}

int run_hub(const Options& options, const fs::path& q2, const fs::path& self) {
    Handle job;
    Handle session_lock;
    Handle stop_event;
    fs::path session;
    bool session_created = false;
    bool ctrl_handler_set = false;
    std::optional<q2dz::win::ChildProcess> q2_process;
    std::array<LiveSlot, kHubMaxRooms> slots{};
    CustomSource source;
    int result = kInternalFailure;

    auto cleanup = [&]() noexcept {
        source.publish({});
        source.stop();
        for (auto& slot : slots) remove_slot_gateway(slot, options.graceful_ms);
        if (q2_process && q2_process->process) {
            if (WaitForSingleObject(q2_process->process.get(), options.graceful_ms) == WAIT_TIMEOUT && job) {
                TerminateJobObject(job.get(), 3);
                (void)WaitForSingleObject(q2_process->process.get(), 5000);
            }
        }
        q2_process.reset();
        job.reset();
        if (ctrl_handler_set) {
            SetConsoleCtrlHandler(ctrl_handler, FALSE);
            ctrl_handler_set = false;
        }
        g_stop_event.reset();
        session_lock.reset();
        if (session_created) {
            try {
                if (!q2dz::win::remove_tree_with_retry(session, 10000)) result = kCleanupFailure;
            } catch (...) {
                result = kCleanupFailure;
            }
            session_created = false;
        }
    };

    try {
        stop_event = make_event();
        if (!stop_event) throw std::runtime_error("CreateEvent failed");
        g_stop_event = std::move(stop_event);
        if (!SetConsoleCtrlHandler(ctrl_handler, TRUE)) throw std::runtime_error("SetConsoleCtrlHandler failed");
        ctrl_handler_set = true;
        job = make_kill_job();
        const fs::path root = options.root.value_or(q2dz::win::default_session_root());
        q2dz::win::cleanup_stale_sessions(root);
        session = q2dz::win::create_session_directory(root, session_lock);
        session_created = true;
        spawn_cleanup_guardian(self, root, session);

        if (!source.start()) throw std::runtime_error("custom source failed to start");
        source.publish({});
        auto q2_stdin = q2dz::win::open_null(false);
        const std::vector<std::wstring> q2_args = {
            L"+set", L"game", L"3zb2", L"+set", L"cl_timeout", L"20",
            L"+set", L"cl_serverbrowser_custom_source",
            source.url()
        };
        auto pending = q2dz::win::launch_suspended(
            q2, q2_args, q2.parent_path(), q2_stdin.get(), GetStdHandle(STD_OUTPUT_HANDLE),
            GetStdHandle(STD_ERROR_HANDLE), {}, 0);
        q2_stdin.reset();
        assign_job(job.get(), pending, L"q2");
        resume_process(pending.thread.get());
        pending.thread.reset();
        q2_process.emplace(std::move(pending));

        ControlQueue control;
        if (!control.event()) throw std::runtime_error("hub control event unavailable");
        std::jthread input_reader([&control](std::stop_token) {
            control_reader(GetStdHandle(STD_INPUT_HANDLE), control);
        });

        bool done = false;
        while (!done) {
            if (source.failed()) {
                result = kInternalFailure;
                break;
            }
            auto handles = wait_handles(slots, source, control.event(), q2_process->process.get());
            const DWORD wait = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, 100);
            if (wait == WAIT_FAILED) {
                result = kInternalFailure;
                break;
            }
            if (wait == WAIT_OBJECT_0) {
                result = kSuccess;
                break;
            }
            if (wait == WAIT_OBJECT_0 + 1) {
                result = kInternalFailure;
                break;
            }
            if (q2_process && wait == WAIT_OBJECT_0 + 2) {
                DWORD code = 1;
                result = GetExitCodeProcess(q2_process->process.get(), &code) && code == 0
                    ? kSuccess : kQ2Failure;
                break;
            }
            bool clean_eof = false;
            bool protocol_error = false;
            bool commands_pending = false;
            if (control.event() && WaitForSingleObject(control.event(), 0) == WAIT_OBJECT_0) {
                auto commands = control.drain(kHubCommandsPerCycle, clean_eof, protocol_error,
                                              commands_pending);
                for (auto& command : commands) {
                    process_slot_command(command, slots, job.get(), options, options.gateway, session, source);
                }
                if (protocol_error) {
                    result = kProtocolFailure;
                    break;
                }
                if (clean_eof && !commands_pending) {
                    result = kSuccess;
                    break;
                }
            }
            for (auto& slot : slots) (void)process_slot(slot, options, source, slots);
        }
        input_reader.request_stop();
        (void)CancelSynchronousIo(native_thread_handle(input_reader));
        if (input_reader.joinable()) input_reader.join();
        cleanup();
        return result;
    } catch (const std::exception& error) {
        std::cerr << "q2dz-session: " << error.what() << "\n";
        cleanup();
        return result;
    } catch (...) {
        std::cerr << "q2dz-session: unknown failure\n";
        cleanup();
        return result;
    }
}

int run_cleanup_guardian(int argc, wchar_t** argv) {
    if (argc != 6 || std::wstring(argv[1]) != L"--cleanup-session" ||
        std::wstring(argv[4]) != L"--owner-handle") return kProtocolFailure;
    wchar_t* end = nullptr;
    const auto raw = wcstoull(argv[5], &end, 10);
    if (!argv[5][0] || !end || *end || raw == 0) return kProtocolFailure;
    const fs::path root(argv[2]);
    const fs::path session(argv[3]);
    std::error_code error;
    if (!fs::exists(session, error)) return error ? kProtocolFailure : kSuccess;
    if (!q2dz::win::validate_session_for_cleanup(root, session)) return kProtocolFailure;
    Handle owner(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(raw)));
    if (WaitForSingleObject(owner.get(), INFINITE) != WAIT_OBJECT_0) return kProtocolFailure;
    if (!fs::exists(session, error)) return error ? kProtocolFailure : kSuccess;
    if (!q2dz::win::validate_session_for_cleanup(root, session)) return kProtocolFailure;
    return q2dz::win::remove_tree_with_retry(session, 10000) ? kSuccess : kCleanupFailure;
}

fs::path self_path() {
    std::wstring buffer(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (count == 0 || count == buffer.size()) throw std::runtime_error("GetModuleFileNameW failed");
    buffer.resize(count);
    return fs::path(buffer);
}

}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc >= 2 && std::wstring(argv[1]) == L"--cleanup-session") return run_cleanup_guardian(argc, argv);
        const auto options = parse_options(argc, argv);
        const auto self = self_path();
        const auto q2 = q2dz::win::locate_q2(options.q2, self);
        if (!q2) throw std::runtime_error("Q2PRO-X.exe not found; pass --q2 or set Q2PROX_EXE");
        if (!fs::is_regular_file(options.gateway)) throw std::runtime_error("gateway executable not found");
        return run_hub(options, *q2, self);
    } catch (const std::exception& error) {
        std::cerr << "q2dz-session: " << error.what() << "\n";
        return kInternalFailure;
    } catch (...) {
        std::cerr << "q2dz-session: unknown failure\n";
        return kInternalFailure;
    }
}

#endif
