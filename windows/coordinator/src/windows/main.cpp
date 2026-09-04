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
#include <charconv>
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
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

using q2dz::win::Handle;

constexpr int kSuccess = 0;
constexpr int kInternalFailure = 2;
constexpr int kProtocolFailure = 13;
constexpr int kCleanupFailure = 14;
constexpr int kQ2Failure = 74;
constexpr std::size_t kHubCommandsPerCycle = 32;

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

q2dz::win::Handle make_event(bool manual_reset = true) {
    return Handle(CreateEventW(nullptr, manual_reset ? TRUE : FALSE, FALSE, nullptr));
}

class ControlQueue {
public:
    ControlQueue() : event_(make_event()) {}

    [[nodiscard]] HANDLE event() const noexcept { return event_.get(); }

    bool feed(std::string_view bytes) {
        std::lock_guard lock(mutex_);
        if (protocol_error_) {
            clear_commands();
            SetEvent(event_.get());
            return false;
        }
        if (discard_until_newline_) {
            const auto newline = bytes.find('\n');
            const auto discarded = newline == std::string_view::npos ? bytes.size() : newline;
            if (discarded_fragment_bytes_ > q2dz::HubCommandFramer::max_line_bytes ||
                discarded > q2dz::HubCommandFramer::max_line_bytes - discarded_fragment_bytes_) {
                protocol_error_ = true;
                clear_commands();
                discard_until_newline_ = false;
                discarded_fragment_bytes_ = 0;
                framer_.clear();
                SetEvent(event_.get());
                return false;
            }
            discarded_fragment_bytes_ += discarded;
            if (newline == std::string_view::npos) {
                SetEvent(event_.get());
                return true;
            }
            bytes.remove_prefix(newline + 1U);
            discard_until_newline_ = false;
            discarded_fragment_bytes_ = 0;
        }
        if (!bytes.empty()) {
            // Framed lines and parsed commands share one count/byte budget.
            const auto pending_bytes = framer_.pending_byte_count();
            const auto queued_count = commands_.size() + framer_.queued_line_count() +
                                       (pending_bytes != 0 ? 1U : 0U);
            std::size_t incoming_count = static_cast<std::size_t>(std::count(bytes.begin(), bytes.end(), '\n'));
            if (bytes.back() != '\n') ++incoming_count;
            if (pending_bytes != 0 && incoming_count != 0) --incoming_count;
            const auto queued_bytes = command_bytes_ + framer_.queued_byte_count() + pending_bytes;
            if (queued_count > q2dz::HubCommandFramer::max_queued_lines ||
                incoming_count > q2dz::HubCommandFramer::max_queued_lines - queued_count ||
                queued_bytes > q2dz::HubCommandFramer::max_queued_bytes ||
                bytes.size() > q2dz::HubCommandFramer::max_queued_bytes - queued_bytes ||
                !framer_.feed(bytes)) {
                protocol_error_ = true;
                clear_commands();
                framer_.clear();
            }
        }
        SetEvent(event_.get());
        return !protocol_error_;
    }

    void finish() {
        std::lock_guard lock(mutex_);
        if (protocol_error_) {
            clear_commands();
            SetEvent(event_.get());
            return;
        }
        if (discard_until_newline_) {
            protocol_error_ = true;
            clear_commands();
            discard_until_newline_ = false;
            discarded_fragment_bytes_ = 0;
            framer_.clear();
            SetEvent(event_.get());
            return;
        }
        discard_until_newline_ = false;
        if (!framer_.finish()) {
            protocol_error_ = true;
            clear_commands();
        } else {
            clean_eof_ = true;
        }
        SetEvent(event_.get());
    }

    void fail() {
        std::lock_guard lock(mutex_);
        protocol_error_ = true;
        clear_commands();
        discard_until_newline_ = false;
        discarded_fragment_bytes_ = 0;
        framer_.clear();
        SetEvent(event_.get());
    }

    void discard_pending() {
        std::lock_guard lock(mutex_);
        clear_commands();
        const auto pending_bytes = framer_.pending_byte_count();
        if (!discard_until_newline_) discarded_fragment_bytes_ = pending_bytes;
        discard_until_newline_ = discard_until_newline_ || pending_bytes != 0;
        framer_.clear();
        parser_ = q2dz::HubCommandParser(q2dz::HubCommandChannel::rooms);
        if (clean_eof_ || protocol_error_) SetEvent(event_.get());
        else ResetEvent(event_.get());
    }

    std::vector<q2dz::HubCommand> drain(std::size_t limit,
                                        bool& clean_eof,
                                        bool& protocol_error,
                                        bool& commands_pending) {
        std::lock_guard lock(mutex_);
        if (protocol_error_) {
            clear_commands();
            clean_eof = clean_eof_;
            protocol_error = true;
            commands_pending = false;
            return {};
        }
        parse_queued_lines();
        if (protocol_error_) {
            clear_commands();
            clean_eof = clean_eof_;
            protocol_error = true;
            commands_pending = false;
            return {};
        }
        std::vector<q2dz::HubCommand> result;
        result.reserve((std::min)(commands_.size(), limit));
        while (!commands_.empty() && result.size() < limit) {
            command_bytes_ -= command_bytes(commands_.front());
            result.push_back(std::move(commands_.front()));
            commands_.pop_front();
        }
        commands_pending = !commands_.empty() || framer_.queued_line_count() != 0;
        clean_eof = clean_eof_;
        protocol_error = protocol_error_;
        if (commands_.empty() && !clean_eof_ && !protocol_error_) ResetEvent(event_.get());
        return result;
    }

private:
    [[nodiscard]] static std::size_t command_bytes(const q2dz::HubCommand& command) noexcept {
        return command.room_id.size() + command.json.size();
    }

    void clear_commands() noexcept {
        while (!commands_.empty()) commands_.pop_front();
        command_bytes_ = 0;
    }

    void parse_queued_lines() {
        if (protocol_error_) return;
        for (auto lines = framer_.drain(); !lines.empty(); lines.pop_front()) {
            auto parsed = parser_.parse(lines.front().view());
            if (!parsed.command) {
                protocol_error_ = true;
                clear_commands();
                return;
            }
            command_bytes_ += command_bytes(*parsed.command);
            commands_.push_back(std::move(*parsed.command));
        }
    }

    std::mutex mutex_;
    q2dz::HubCommandFramer framer_;
    q2dz::HubCommandParser parser_{q2dz::HubCommandChannel::rooms};
    std::deque<q2dz::HubCommand> commands_;
    std::size_t command_bytes_ = 0;
    Handle event_;
    bool clean_eof_ = false;
    bool protocol_error_ = false;
    bool discard_until_newline_ = false;
    std::size_t discarded_fragment_bytes_ = 0;
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
    q2dz::win::Pipe input;
    Handle event_read;
    q2dz::win::ChildProcess process;
    std::optional<std::jthread> reader;
    Queue events;
    q2dz::ChildEventParser event_parser;
};

void stop_hub_gateway(HubGateway& gateway, DWORD graceful_ms) noexcept {
    q2dz::HubCommand stop;
    stop.type = q2dz::HubCommandType::stop;
    std::string line = q2dz::format_hub_command(stop);
    if (!line.empty()) {
        line.push_back('\n');
        try {
            if (gateway.input.write) {
                q2dz::win::write_all(gateway.input.write.get(),
                                     reinterpret_cast<const unsigned char*>(line.data()), line.size());
            }
        } catch (...) {
        }
    }
    secure_clear_string(line);
    gateway.input.write.reset();
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

std::optional<std::uint64_t> parse_target_id(std::string_view text) {
    if (text.empty()) return std::nullopt;
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0) return std::nullopt;
    return value;
}

bool is_dedicated_room(std::string_view room_id) {
    return room_id.starts_with("room.q2ded.");
}

unsigned target_recovery_limit(std::string_view room_id) {
    // The counter includes the current failed attempt: q2 has one retry
    // after a post-ready failure, while q2ded has two bounded retries.
    return is_dedicated_room(room_id) ? 3U : 2U;
}

enum class TargetState { pending, ready, terminal };

struct LiveTarget {
    std::string room_id;
    std::uint64_t target_id = 0;
    TargetState state = TargetState::pending;
    std::optional<q2dz::LoopbackEndpoint> endpoint;
    unsigned recovery_count = 0;
    bool closed = false;
    bool retryable_failure = false;
    ULONGLONG ready_deadline = 0;
};

using LiveTargets = std::unordered_map<std::string, LiveTarget>;
using TargetRoutes = std::unordered_map<std::uint64_t, std::string>;

bool send_gateway_command(HubGateway& gateway, const q2dz::HubCommand& command) {
    std::string line = q2dz::format_hub_command(command);
    if (line.empty()) return false;
    line.push_back('\n');
    try {
        q2dz::win::write_all(gateway.input.write.get(),
                             reinterpret_cast<const unsigned char*>(line.data()), line.size());
        secure_clear_string(line);
        return true;
    } catch (...) {
        secure_clear_string(line);
        return false;
    }
}

std::uint64_t allocate_target_id(TargetRoutes& routes, std::uint64_t& next_id) {
    for (;;) {
        const std::uint64_t candidate = next_id == 0 ? 1 : next_id++;
        if (next_id == 0) next_id = 1;
        if (candidate != 0 && !routes.contains(candidate)) return candidate;
    }
}

void publish_source(CustomSource& source, const LiveTargets& targets) {
    std::vector<std::pair<std::string, q2dz::LoopbackEndpoint>> ordered;
    ordered.reserve(targets.size());
    for (const auto& [room_id, target] : targets) {
        if (target.state == TargetState::ready && target.endpoint) {
            ordered.emplace_back(room_id, *target.endpoint);
        }
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    std::vector<q2dz::LoopbackEndpoint> endpoints;
    endpoints.reserve(ordered.size());
    for (const auto& [room_id, endpoint] : ordered) {
        (void)room_id;
        endpoints.push_back(endpoint);
    }
    source.publish(endpoints);
}

std::unique_ptr<HubGateway> start_shared_gateway(HANDLE root_job,
                                                 const fs::path& gateway_path,
                                                 const fs::path& session,
                                                 DWORD graceful_ms) {
    auto gateway = std::make_unique<HubGateway>();
    try {
        gateway->job = make_kill_job();
        gateway->directory = session / L"gateway";
        fs::create_directories(gateway->directory / L"webview2");
        auto input_pipe = q2dz::win::make_pipe(true, false);
        auto event_pipe = q2dz::win::make_pipe(false, true);
        gateway->owner = q2dz::win::make_pipe(true, false);
        auto nullerr = q2dz::win::open_null(true);
        const std::vector<std::wstring> arguments = {
            L"--managed-session", L"--udp-port", L"0", L"--owner-handle",
            std::to_wstring(reinterpret_cast<std::uintptr_t>(gateway->owner.read.get())),
            L"--user-data-dir", (gateway->directory / L"webview2").wstring()
        };
        const std::vector<HANDLE> inherited = {
            input_pipe.read.get(), event_pipe.write.get(), gateway->owner.read.get(), nullerr.get()
        };
        gateway->process = q2dz::win::launch_suspended(
            gateway_path, arguments, gateway_path.parent_path(), input_pipe.read.get(),
            event_pipe.write.get(), nullerr.get(), inherited, CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP);
        assign_job(root_job, gateway->process, L"gateway");
        assign_job(gateway->job.get(), gateway->process, L"gateway-shared");
        input_pipe.read.reset();
        event_pipe.write.reset();
        gateway->owner.read.reset();
        resume_process(gateway->process.thread.get());
        gateway->process.thread.reset();
        gateway->input.write = std::move(input_pipe.write);
        gateway->event_read = std::move(event_pipe.read);
        if (!gateway->input.write || !gateway->event_read || !gateway->events.event) {
            throw std::runtime_error("shared gateway pipe unavailable");
        }
        gateway->reader.emplace([handle = gateway->event_read.get(), queue = &gateway->events](std::stop_token) {
            event_reader(handle, *queue);
        });
        return gateway;
    } catch (...) {
        if (gateway) stop_hub_gateway(*gateway, graceful_ms);
        throw;
    }
}

void erase_target(LiveTargets& targets, TargetRoutes& routes, LiveTargets::iterator target) {
    routes.erase(target->second.target_id);
    targets.erase(target);
}

void terminal_target(LiveTarget& target, bool closed, CustomSource& source,
                     const LiveTargets& targets) {
    const bool was_ready = target.state == TargetState::ready;
    target.state = TargetState::terminal;
    target.endpoint.reset();
    target.closed = closed;
    target.retryable_failure = is_dedicated_room(target.room_id) || (!closed && was_ready);
    if (target.retryable_failure && target.recovery_count < target_recovery_limit(target.room_id)) {
        ++target.recovery_count;
    }
    publish_source(source, targets);
}

bool consume_gateway_events(HubGateway& gateway, CustomSource& source, LiveTargets& targets,
                            TargetRoutes& routes) {
    for (auto& line : gateway.events.drain()) {
        const auto parsed = gateway.event_parser.parse(line);
        secure_clear_string(line);
        if (!parsed.event) return false;
        const auto& event = *parsed.event;
        if (event.type == q2dz::ChildEventType::heartbeat) continue;
        const auto id_field = event.fields.find("id");
        if (id_field == event.fields.end()) return false;
        const auto id = parse_target_id(id_field->second);
        if (!id) return false;
        const auto route = routes.find(*id);
        if (route == routes.end()) continue;
        const auto target = targets.find(route->second);
        if (target == targets.end() || target->second.target_id != *id) {
            routes.erase(route);
            continue;
        }
        if (event.type == q2dz::ChildEventType::transport_ready) {
            if (target->second.state != TargetState::pending) continue;
            const auto endpoint = event.fields.find("udp");
            if (endpoint == event.fields.end()) return false;
            const auto parsed_endpoint = q2dz::parse_loopback_endpoint(endpoint->second);
            if (!parsed_endpoint) return false;
            target->second.endpoint = parsed_endpoint;
            target->second.state = TargetState::ready;
            publish_source(source, targets);
        } else if (event.type == q2dz::ChildEventType::room_closed ||
                   event.type == q2dz::ChildEventType::target_failed) {
            if (target->second.state != TargetState::terminal) {
                routes.erase(route);
                terminal_target(target->second,
                                event.type == q2dz::ChildEventType::room_closed, source, targets);
            }
        } else {
            return false;
        }
    }
    return true;
}

bool process_gateway(HubGateway& gateway, const Options& options, CustomSource& source,
                     LiveTargets& targets, TargetRoutes& routes) {
    if (!consume_gateway_events(gateway, source, targets, routes)) return false;
    if (gateway.events.is_finished() ||
        (gateway.process.process && WaitForSingleObject(gateway.process.process.get(), 0) == WAIT_OBJECT_0)) {
        for (int attempt = 0; attempt < 20 && !gateway.events.is_finished(); ++attempt) {
            (void)WaitForSingleObject(gateway.events.event.get(), 5);
            if (!consume_gateway_events(gateway, source, targets, routes)) return false;
        }
        return false;
    }
    (void)options;
    return true;
}

bool process_room_command(q2dz::HubCommand& command, HubGateway* gateway, bool disabled,
                          DWORD ready_ms, CustomSource& source, LiveTargets& targets,
                          TargetRoutes& routes, std::uint64_t& next_id) {
    if (disabled || gateway == nullptr) {
        command.clear();
        return true;
    }
    if (command.type == q2dz::HubCommandType::ice) {
        q2dz::HubCommand forwarded;
        forwarded.type = q2dz::HubCommandType::ice;
        forwarded.id_or_generation = command.id_or_generation;
        forwarded.json = std::move(command.json);
        const bool sent = send_gateway_command(*gateway, forwarded);
        forwarded.clear();
        command.clear();
        return sent;
    }
    if (command.type == q2dz::HubCommandType::drop) {
        const auto target = targets.find(command.room_id);
        if (target != targets.end()) {
            q2dz::HubCommand forwarded;
            forwarded.type = q2dz::HubCommandType::drop;
            forwarded.id_or_generation = target->second.target_id;
            const bool sent = send_gateway_command(*gateway, forwarded);
            forwarded.clear();
            erase_target(targets, routes, target);
            publish_source(source, targets);
            command.clear();
            return sent;
        }
        command.clear();
        return true;
    }
    if (command.type != q2dz::HubCommandType::put) {
        command.clear();
        return false;
    }
    const auto existing = targets.find(command.room_id);
    if (existing != targets.end() && existing->second.state != TargetState::terminal) {
        command.clear();
        return true;
    }
    unsigned recovery_count = 0;
    if (existing != targets.end()) {
        if ((existing->second.closed && !is_dedicated_room(command.room_id)) ||
            !existing->second.retryable_failure ||
            existing->second.recovery_count >= target_recovery_limit(command.room_id)) {
            command.clear();
            return true;
        }
        recovery_count = existing->second.recovery_count;
        erase_target(targets, routes, existing);
    }
    LiveTarget target;
    target.room_id = command.room_id;
    target.target_id = allocate_target_id(routes, next_id);
    target.recovery_count = recovery_count;
    target.ready_deadline = GetTickCount64() + ready_ms;
    const auto room_id = target.room_id;
    const auto target_id = target.target_id;
    targets.emplace(room_id, std::move(target));
    routes.emplace(target_id, room_id);
    q2dz::HubCommand forwarded;
    forwarded.type = q2dz::HubCommandType::put;
    forwarded.id_or_generation = target_id;
    forwarded.json = std::move(command.json);
    const bool sent = send_gateway_command(*gateway, forwarded);
    forwarded.clear();
    command.clear();
    if (!sent) {
        const auto inserted = targets.find(room_id);
        if (inserted != targets.end()) erase_target(targets, routes, inserted);
        publish_source(source, targets);
    }
    return sent;
}

bool process_target_timeouts(HubGateway* gateway, CustomSource& source, LiveTargets& targets,
                             TargetRoutes& routes) {
    if (gateway == nullptr) return true;
    std::vector<std::uint64_t> expired;
    const auto now = GetTickCount64();
    for (const auto& [room_id, target] : targets) {
        (void)room_id;
        if (target.state == TargetState::pending && now >= target.ready_deadline) expired.push_back(target.target_id);
    }
    for (const auto id : expired) {
        const auto route = routes.find(id);
        if (route == routes.end()) continue;
        auto target = targets.find(route->second);
        if (target == targets.end()) continue;
        q2dz::HubCommand drop;
        drop.type = q2dz::HubCommandType::drop;
        drop.id_or_generation = id;
        if (!send_gateway_command(*gateway, drop)) return false;
        routes.erase(route);
        terminal_target(target->second, false, source, targets);
    }
    return true;
}

void recover_gateway(std::unique_ptr<HubGateway>& gateway, unsigned& restart_count,
                     bool& transport_disabled, HANDLE root_job, const Options& options,
                     const fs::path& session, ControlQueue& control, CustomSource& source,
                     LiveTargets& targets, TargetRoutes& routes) {
    source.publish({});
    targets.clear();
    routes.clear();
    control.discard_pending();
    if (gateway) {
        stop_hub_gateway(*gateway, options.graceful_ms);
        gateway.reset();
    }
    if (restart_count != 0) {
        transport_disabled = true;
        return;
    }
    ++restart_count;
    try {
        gateway = start_shared_gateway(root_job, options.gateway, session, options.graceful_ms);
    } catch (...) {
        transport_disabled = true;
    }
}

int run_hub(const Options& options, const fs::path& q2, const fs::path& self) {
    Handle job;
    Handle session_lock;
    Handle stop_event;
    fs::path session;
    bool session_created = false;
    bool ctrl_handler_set = false;
    std::optional<q2dz::win::ChildProcess> q2_process;
    std::unique_ptr<HubGateway> gateway;
    LiveTargets targets;
    TargetRoutes routes;
    std::uint64_t next_target_id = 1;
    unsigned restart_count = 0;
    bool transport_disabled = false;
    CustomSource source;
    int result = kInternalFailure;

    auto cleanup = [&]() noexcept {
        source.publish({});
        source.stop();
        targets.clear();
        routes.clear();
        if (gateway) {
            stop_hub_gateway(*gateway, options.graceful_ms);
            gateway.reset();
        }
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
        try {
            gateway = start_shared_gateway(job.get(), options.gateway, session, options.graceful_ms);
        } catch (...) {
            transport_disabled = true;
            std::cerr << "q2dz-session: shared transport unavailable\n";
        }
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
            if (WaitForSingleObject(g_stop_event.get(), 0) == WAIT_OBJECT_0) {
                result = kSuccess;
                break;
            }
            if (WaitForSingleObject(q2_process->process.get(), 0) == WAIT_OBJECT_0) {
                DWORD code = 1;
                result = GetExitCodeProcess(q2_process->process.get(), &code) && code == 0
                    ? kSuccess : kQ2Failure;
                break;
            }
            if (source.failed()) {
                result = kInternalFailure;
                break;
            }
            bool clean_eof = false;
            bool protocol_error = false;
            bool commands_pending = false;
            if (control.event() && WaitForSingleObject(control.event(), 0) == WAIT_OBJECT_0) {
                auto commands = control.drain(kHubCommandsPerCycle, clean_eof, protocol_error,
                                              commands_pending);
                if (protocol_error) {
                    for (auto& command : commands) command.clear();
                    result = kProtocolFailure;
                    break;
                }
                for (auto& command : commands) {
                    if (!process_room_command(command, gateway.get(), transport_disabled, options.ready_ms,
                                              source, targets, routes, next_target_id)) {
                        recover_gateway(gateway, restart_count, transport_disabled, job.get(), options,
                                        session, control, source, targets, routes);
                        break;
                    }
                }
                if (clean_eof && !commands_pending) {
                    result = kSuccess;
                    break;
                }
            }
            if (gateway && !transport_disabled &&
                (WaitForSingleObject(gateway->events.event.get(), 0) == WAIT_OBJECT_0 ||
                 (gateway->process.process && WaitForSingleObject(gateway->process.process.get(), 0) == WAIT_OBJECT_0))) {
                if (!process_gateway(*gateway, options, source, targets, routes)) {
                    recover_gateway(gateway, restart_count, transport_disabled, job.get(), options,
                                    session, control, source, targets, routes);
                }
            }
            if (gateway && !transport_disabled && !process_target_timeouts(gateway.get(), source, targets, routes)) {
                recover_gateway(gateway, restart_count, transport_disabled, job.get(), options,
                                session, control, source, targets, routes);
            }
            (void)WaitForSingleObject(control.event(), 25);
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
