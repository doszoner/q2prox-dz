#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

#include "q2dz/child_protocol.hpp"
#include "web_message_dispatch.hpp"
#include "webview_bridge_protocol.h"

#include <wrl.h>
#include <unknwn.h>
#include <WebView2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT udp_message = WM_APP + 1;
constexpr UINT control_message = WM_APP + 2;
constexpr UINT retire_socket_message = WM_APP + 3;
constexpr UINT_PTR heartbeat_timer = 1;
constexpr DWORD sio_udp_connreset = _WSAIOW(IOC_VENDOR, 12);
constexpr BOOL controller_visible = TRUE;
constexpr std::size_t max_targets = 256;
constexpr std::size_t max_datagram = 1400;
constexpr std::size_t max_udp_packets_per_message = 64;
constexpr std::size_t max_control_bytes = 64 * 1024;
constexpr std::size_t max_control_commands_per_tick = 32;
// ICE + 256 PUTs + STOP may arrive before the page-ready callback.
constexpr std::size_t max_control_queue_commands = max_targets + 2;
constexpr std::size_t max_control_queue_bytes = max_control_bytes * max_control_queue_commands;
constexpr std::size_t max_legacy_configuration = 256 * 1024;
constexpr char page_ready[] =
    "{\"type\":\"state\",\"value\":\"page-ready\",\"reason\":\"\"}";
std::atomic<HWND> gateway_window{nullptr};

std::string_view transport_failure_stage(std::string_view reason) noexcept {
    if (reason == "peer_bind_failed" || reason == "alias_timeout" ||
        reason == "duplicate_remote_peer") return "resolve";
    return "transport";
}

BOOL WINAPI console_control_handler(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT &&
        event != CTRL_CLOSE_EVENT) return FALSE;
    const HWND window = gateway_window.load();
    return window && PostMessageW(window, WM_CLOSE, 0, 0);
}

struct Options {
    std::filesystem::path assets;
    std::filesystem::path user_data;
    HANDLE owner = nullptr;
};

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void require_hr(HRESULT result, const char *message) {
    if (FAILED(result)) throw std::runtime_error(message);
}

struct CoTaskMemDeleter {
    void operator()(wchar_t *value) const noexcept {
        if (value) SecureZeroMemory(value, (std::wcslen(value) + 1U) * sizeof(*value));
        CoTaskMemFree(value);
    }
};
using CoTaskMemString = std::unique_ptr<wchar_t, CoTaskMemDeleter>;

std::string wide_to_utf8(std::wstring_view input) {
    if (input.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    require(size > 0, "invalid UTF-16 input");
    std::string output(size, '\0');
    require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        input.data(), static_cast<int>(input.size()), output.data(), size,
        nullptr, nullptr) == size, "UTF-16 conversion failed");
    return output;
}

std::wstring utf8_to_wide(std::string_view input) {
    if (input.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        input.data(), static_cast<int>(input.size()), nullptr, 0);
    require(size > 0, "invalid UTF-8 input");
    std::wstring output(size, L'\0');
    require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        input.data(), static_cast<int>(input.size()), output.data(), size) == size,
        "UTF-8 conversion failed");
    return output;
}

Options parse_options(int argc, wchar_t **argv) {
    Options options;
    bool managed = false;
    bool have_port = false;
    for (int index = 1; index < argc; ++index) {
        const std::wstring key = argv[index];
        if (key == L"--managed-session") {
            require(!managed, "duplicate managed-session option");
            managed = true;
            continue;
        }
        require(++index < argc, "missing option value");
        const std::wstring value = argv[index];
        if (key == L"--udp-port") {
            require(!have_port && value == L"0", "managed UDP port must be zero");
            have_port = true;
        } else if (key == L"--owner-handle") {
            require(options.owner == nullptr, "duplicate owner handle");
            wchar_t *end = nullptr;
            const auto raw = _wcstoui64(value.c_str(), &end, 10);
            require(raw != 0 && end && *end == L'\0', "invalid owner handle");
            options.owner = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(raw));
        } else if (key == L"--user-data-dir") {
            require(options.user_data.empty(), "duplicate user-data directory");
            options.user_data = value;
        } else {
            throw std::runtime_error("unknown option");
        }
    }
    require(managed && have_port && options.owner && !options.user_data.empty(),
        "incomplete managed gateway options");
    DWORD handle_flags = 0;
    require(GetHandleInformation(options.owner, &handle_flags) != 0 &&
            GetFileType(options.owner) == FILE_TYPE_PIPE,
        "owner handle must be an inherited pipe");
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
        static_cast<DWORD>(executable.size()));
    require(length > 0 && length < executable.size(), "gateway path lookup failed");
    executable.resize(length);
    options.assets = std::filesystem::path(executable).parent_path() / L"assets";
    require(std::filesystem::is_directory(options.assets), "packaged assets are missing");
    options.user_data = std::filesystem::absolute(options.user_data);
    return options;
}

void secure_clear(std::string &value) {
    if (!value.empty()) SecureZeroMemory(value.data(), value.size());
    value.clear();
    value.shrink_to_fit();
}

void secure_clear(std::wstring &value) {
    if (!value.empty()) SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
    value.clear();
    value.shrink_to_fit();
}

bool loopback_source(const sockaddr_in &source) {
    return source.sin_family == AF_INET &&
           source.sin_addr.s_addr == htonl(INADDR_LOOPBACK);
}

bool same_source(const sockaddr_in &left, const sockaddr_in &right) {
    return left.sin_family == right.sin_family && left.sin_port == right.sin_port &&
           left.sin_addr.s_addr == right.sin_addr.s_addr;
}

bool diagnostic_background_timer_flag_exact() {
    constexpr std::wstring_view expected = L"--disable-background-timer-throttling";
    std::array<wchar_t, 64> value{};
    const DWORD length = GetEnvironmentVariableW(
        L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", value.data(),
        static_cast<DWORD>(value.size()));
    return length == expected.size() &&
           std::wstring_view(value.data(), length) == expected;
}

enum class ControlMode { unknown, legacy, hub };

struct TargetCounters {
    std::uint64_t udp_from_q2_datagrams = 0;
    std::uint64_t udp_from_q2_bytes = 0;
    std::uint64_t udp_to_q2_datagrams = 0;
    std::uint64_t udp_to_q2_bytes = 0;
    std::uint64_t post_to_web_datagrams = 0;
    std::uint64_t web_to_native_datagrams = 0;
    std::uint64_t dropped_packets = 0;
    std::uint64_t oversized_udp_drops = 0;
    std::uint64_t second_source_drops = 0;
    std::uint64_t transport_not_ready_drops = 0;
    std::size_t max_udp_to_transport_datagram = 0;
    std::size_t max_transport_to_udp_datagram = 0;
};

struct TargetRuntime {
    SOCKET socket = INVALID_SOCKET;
    sockaddr_in client{};
    bool have_client = false;
    bool ready = false;
    TargetCounters counters;
};

class Gateway {
public:
    explicit Gateway(Options options) : options_(std::move(options)) {}
    ~Gateway() { shutdown(); }

    int run(HINSTANCE instance) {
        instance_ = instance;
        create_window();
        start_control_reader();
        watch_owner();
        start_winsock();
        start_webview();

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return exit_code_;
    }

    LRESULT on_window_message(UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == retire_socket_message) {
            const SOCKET value = static_cast<SOCKET>(wparam);
            const auto retired = retiring_sockets_.find(value);
            if (retired != retiring_sockets_.end()) {
                retiring_sockets_.erase(retired);
                closesocket(value);
            }
            return 0;
        }
        if (message == udp_message) {
            if (WSAGETSELECTERROR(lparam) != 0) {
                const auto target = socket_targets_.find(static_cast<SOCKET>(wparam));
                if (target != socket_targets_.end()) {
                    fail_target(target->second, "udp", "udp_async_error");
                } else {
                    ++unknown_peer_drops_;
                }
            } else if (WSAGETSELECTEVENT(lparam) == FD_READ) {
                receive_udp(static_cast<SOCKET>(wparam));
            }
            return 0;
        }
        if (message == control_message) {
            drain_control();
            return 0;
        }
        if (message == WM_TIMER && wparam == heartbeat_timer) {
            emit_heartbeat();
            return 0;
        }
        if (message == WM_CLOSE) {
            DestroyWindow(window_);
            return 0;
        }
        if (message == WM_DESTROY) {
            gateway_window.store(nullptr);
            window_ = nullptr;
            PostQuitMessage(exit_code_);
            return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }

private:
    void create_window() {
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = instance_;
        window_class.lpszClassName = L"Q2DosZoneWebView2Gateway";
        RegisterClassW(&window_class);
        window_ = CreateWindowExW(0, window_class.lpszClassName, L"Q2 Dos.Zone Gateway",
            WS_OVERLAPPED, 0, 0, 1, 1, nullptr, nullptr, instance_, this);
        require(window_ != nullptr, "window creation failed");
        require(SetTimer(window_, heartbeat_timer, 1000, nullptr) != 0,
            "heartbeat timer failed");
        gateway_window.store(window_);
    }

    void watch_owner() {
        owner_thread_ = std::thread([this] {
            char value = 0;
            DWORD length = 0;
            (void)ReadFile(options_.owner, &value, 1, &length, nullptr);
            const HWND window = gateway_window.load();
            if (window) PostMessageW(window, WM_CLOSE, 0, 0);
        });
    }

    void start_winsock() {
        WSADATA data{};
        require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "WSAStartup failed");
        winsock_started_ = true;
    }

    void post_control_message() {
        const HWND window = gateway_window.load();
        if (window) PostMessageW(window, control_message, 0, 0);
    }

    void set_control_error(const char *code) {
        {
            std::lock_guard lock(control_mutex_);
            if (control_error_) return;
            control_error_ = true;
            control_error_code_ = code;
        }
        post_control_message();
    }

    bool enqueue_control_line(std::string line) {
        if (line.size() > max_control_bytes) {
            set_control_error("control_line_too_long");
            secure_clear(line);
            return false;
        }
        std::lock_guard lock(control_mutex_);
        if (control_lines_.size() >= max_control_queue_commands ||
            queued_control_bytes_ > max_control_queue_bytes - line.size()) {
            control_error_ = true;
            control_error_code_ = "control_queue_overflow";
            secure_clear(line);
            return false;
        }
        queued_control_bytes_ += line.size();
        control_lines_.emplace_back(std::move(line));
        return true;
    }

    void read_hub_bytes(std::string_view bytes, std::string &pending) {
        pending.append(bytes.data(), bytes.size());
        while (const auto newline = pending.find('\n'); newline != std::string::npos) {
            std::string line = pending.substr(0, newline);
            std::string remainder = pending.substr(newline + 1U);
            secure_clear(pending);
            pending = std::move(remainder);
            secure_clear(remainder);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const bool queued = enqueue_control_line(std::move(line));
            secure_clear(line);
            if (!queued) return;
        }
        if (pending.size() > max_control_bytes) set_control_error("control_line_too_long");
    }

    void start_control_reader() {
        control_thread_ = std::thread([this] {
            const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
            if (!input || input == INVALID_HANDLE_VALUE) {
                set_control_error("control_input_unavailable");
                return;
            }
            ControlMode mode = ControlMode::unknown;
            std::string pending;
            std::array<char, 8192> buffer{};
            struct ReaderCleanup {
                std::string &pending;
                std::array<char, 8192> &buffer;
                ~ReaderCleanup() {
                    secure_clear(pending);
                    SecureZeroMemory(buffer.data(), buffer.size());
                }
            } cleanup{pending, buffer};
            for (;;) {
                DWORD length = 0;
                if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()),
                              &length, nullptr)) {
                    if (GetLastError() != ERROR_BROKEN_PIPE) {
                        set_control_error("control_read_failed");
                        return;
                    }
                    break;
                }
                if (length == 0) break;
                pending.append(buffer.data(), length);
                if (mode == ControlMode::unknown) {
                    const auto first = pending.find_first_not_of(" \t\r\n");
                    if (first == std::string::npos) {
                        if (pending.size() > max_legacy_configuration) {
                            set_control_error("configuration_too_large");
                            return;
                        }
                        continue;
                    }
                    if (pending[first] == '{') {
                        mode = ControlMode::legacy;
                        std::lock_guard lock(control_mutex_);
                        control_mode_ = mode;
                    } else if (pending.substr(first).starts_with("Q2DZ/HUB/2")) {
                        mode = ControlMode::hub;
                        std::lock_guard lock(control_mutex_);
                        control_mode_ = mode;
                    } else {
                        set_control_error("unsupported_control_protocol");
                        return;
                    }
                }
                if (mode == ControlMode::legacy) {
                    if (pending.size() > max_legacy_configuration) {
                        set_control_error("configuration_too_large");
                        return;
                    }
                    std::lock_guard lock(control_mutex_);
                    secure_clear(legacy_configuration_);
                    legacy_configuration_ = pending;
                } else {
                    read_hub_bytes(pending, hub_pending_);
                    secure_clear(pending);
                    post_control_message();
                }
            }

            if (mode == ControlMode::unknown) {
                set_control_error("empty_control");
                return;
            }
            if (mode == ControlMode::legacy) {
                std::lock_guard lock(control_mutex_);
                const auto first = legacy_configuration_.find_first_not_of(" \t\r\n");
                const auto last = legacy_configuration_.find_last_not_of(" \t\r\n");
                if (legacy_configuration_.find('\0') != std::string::npos ||
                    first == std::string::npos || legacy_configuration_[first] != '{' ||
                    legacy_configuration_[last] != '}') {
                    control_error_ = true;
                    control_error_code_ = "invalid_configuration";
                } else {
                    legacy_ready_ = true;
                }
            } else {
                if (!hub_pending_.empty()) {
                    if (!enqueue_control_line(std::move(hub_pending_))) {
                        post_control_message();
                        return;
                    }
                    secure_clear(hub_pending_);
                }
                std::lock_guard lock(control_mutex_);
                control_eof_ = true;
            }
            post_control_message();
        });
    }

    void start_webview() {
        user_data_ = options_.user_data;
        std::filesystem::create_directories(user_data_);

        LPWSTR browser_version_raw = nullptr;
        const HRESULT browser_version_result =
            GetAvailableCoreWebView2BrowserVersionString(nullptr, &browser_version_raw);
        CoTaskMemString browser_version(browser_version_raw);
        require_hr(browser_version_result,
            "WebView2 Evergreen Runtime is unavailable");
        require(browser_version != nullptr, "WebView2 version is unavailable");
        std::wcerr << L"WEBVIEW2_RUNTIME version=" << browser_version.get()
                   << L" controller_visible=" << (controller_visible ? L"true" : L"false")
                   << L" diagnostic_background_timer_flag_exact="
                   << (diagnostic_background_timer_flag_exact() ? L"true" : L"false") << L"\n";

        require_hr(CreateCoreWebView2EnvironmentWithOptions(nullptr, user_data_.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this](HRESULT result, ICoreWebView2Environment *environment) -> HRESULT {
                    if (FAILED(result) || !environment) {
                        fail("webview_environment_failed");
                        return S_OK;
                    }
                    environment_ = environment;
                    environment->CreateCoreWebView2Controller(window_,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [this](HRESULT controller_result,
                                   ICoreWebView2Controller *controller) -> HRESULT {
                                if (FAILED(controller_result) || !controller) {
                                    fail("webview_controller_failed");
                                    return S_OK;
                                }
                                try {
                                    configure_webview(controller);
                                } catch (const std::exception &) {
                                    fail("webview_configuration_failed");
                                }
                                return S_OK;
                            }).Get());
                    return S_OK;
                }).Get()), "WebView2 environment startup failed");
    }

    void configure_webview(ICoreWebView2Controller *controller) {
        controller_ = controller;
        RECT bounds{0, 0, 1, 1};
        require_hr(controller_->put_Bounds(bounds), "WebView bounds failed");
        require_hr(controller_->put_IsVisible(controller_visible), "WebView visibility failed");
        require_hr(controller_->get_CoreWebView2(&webview_), "WebView lookup failed");

        ComPtr<ICoreWebView2Settings> settings;
        require_hr(webview_->get_Settings(&settings), "WebView settings failed");
        require_hr(settings->put_AreDevToolsEnabled(FALSE), "DevTools disable failed");
        require_hr(settings->put_AreDefaultContextMenusEnabled(FALSE), "context menu disable failed");
        require_hr(settings->put_AreDefaultScriptDialogsEnabled(FALSE), "script dialog disable failed");
        require_hr(settings->put_IsStatusBarEnabled(FALSE), "status bar disable failed");
        require_hr(settings->put_AreHostObjectsAllowed(FALSE), "host objects disable failed");

        ComPtr<ICoreWebView2_3> webview3;
        require_hr(webview_.As(&webview3), "WebView2 virtual host API unavailable");
        require_hr(webview3->SetVirtualHostNameToFolderMapping(
            L"q2doszone.local", options_.assets.c_str(),
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS), "virtual host mapping failed");

        EventRegistrationToken navigation_starting_token{};
        require_hr(webview_->add_NavigationStarting(
            Callback<ICoreWebView2NavigationStartingEventHandler>(
                [](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                    LPWSTR uri = nullptr;
                    const HRESULT uri_result = args->get_Uri(&uri);
                    CoTaskMemString owned_uri(uri);
                    if (FAILED(uri_result) || !owned_uri) return E_FAIL;
                    const std::wstring_view value(owned_uri.get());
                    const bool allowed = value.rfind(L"https://q2doszone.local/", 0) == 0;
                    return args->put_Cancel(!allowed);
                }).Get(), &navigation_starting_token), "NavigationStarting registration failed");
        EventRegistrationToken new_window_requested_token{};
        require_hr(webview_->add_NewWindowRequested(
            Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
                    return args->put_Handled(TRUE);
                }).Get(), &new_window_requested_token), "NewWindowRequested registration failed");
        EventRegistrationToken permission_requested_token{};
        require_hr(webview_->add_PermissionRequested(
            Callback<ICoreWebView2PermissionRequestedEventHandler>(
                [](ICoreWebView2 *, ICoreWebView2PermissionRequestedEventArgs *args) -> HRESULT {
                    return args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                }).Get(), &permission_requested_token), "PermissionRequested registration failed");
        EventRegistrationToken process_failed_token{};
        require_hr(webview_->add_ProcessFailed(
            Callback<ICoreWebView2ProcessFailedEventHandler>(
                [this](ICoreWebView2 *, ICoreWebView2ProcessFailedEventArgs *) -> HRESULT {
                    fail("webview_process_failed");
                    return S_OK;
                }).Get(), &process_failed_token), "ProcessFailed registration failed");
        EventRegistrationToken web_message_received_token{};
        require_hr(webview_->add_WebMessageReceived(
            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [this](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args)
                    -> HRESULT {
                    LPWSTR raw = nullptr;
                    const HRESULT message_result = args->get_WebMessageAsJson(&raw);
                    CoTaskMemString owned_raw(raw);
                    if (FAILED(message_result) || !owned_raw) {
                        fail("web_message_read_failed");
                        return S_OK;
                    }
                    try {
                        with_owned_web_message(owned_raw.release(), CoTaskMemDeleter{},
                            [this](wchar_t *value) { handle_web_message(wide_to_utf8(value)); });
                    } catch (const std::exception &) {
                        fail("invalid_web_message");
                    }
                    return S_OK;
                }).Get(), &web_message_received_token), "WebMessageReceived registration failed");

        require_hr(webview_->Navigate(L"https://q2doszone.local/transport.html"),
            "transport navigation failed");
    }

    void post_bridge_json(std::string json) {
        std::wstring wide;
        try {
            require(webview_ != nullptr, "webview is unavailable");
            wide = utf8_to_wide(json);
            require_hr(webview_->PostWebMessageAsJson(wide.c_str()), "webview message failed");
        } catch (...) {
            secure_clear(wide);
            secure_clear(json);
            throw;
        }
        secure_clear(wide);
        secure_clear(json);
    }

    void post_legacy_configuration(std::string json) {
        std::wstring wide;
        try {
            require(webview_ != nullptr, "webview is unavailable");
            wide = utf8_to_wide(json);
            require_hr(webview_->PostWebMessageAsJson(wide.c_str()), "start message failed");
        } catch (...) {
            secure_clear(wide);
            secure_clear(json);
            throw;
        }
        secure_clear(wide);
        secure_clear(json);
    }

    void post_hub_command(const q2dz::HubCommand &command) {
        std::string payload;
        if (!command.json.empty()) {
            payload.assign(reinterpret_cast<const char *>(command.json.data()),
                           command.json.size());
        }
        std::string json;
        switch (command.type) {
            case q2dz::HubCommandType::ice:
                json = "{\"type\":\"ice\",\"generation\":\"" +
                       std::to_string(command.id_or_generation) +
                       "\",\"iceServers\":" + payload + "}";
                break;
            case q2dz::HubCommandType::put:
                json = "{\"type\":\"put\",\"targetId\":\"" +
                       std::to_string(command.id_or_generation) +
                       "\",\"config\":" + payload + "}";
                break;
            case q2dz::HubCommandType::drop:
                json = "{\"type\":\"drop\",\"targetId\":\"" +
                       std::to_string(command.id_or_generation) + "\"}";
                break;
            case q2dz::HubCommandType::stop:
                json = "{\"type\":\"stop\"}";
                break;
        }
        secure_clear(payload);
        try {
            post_bridge_json(std::move(json));
        } catch (...) {
            secure_clear(json);
            throw;
        }
        secure_clear(json);
    }

    bool bind_target_socket(std::uint64_t target_id) {
        SOCKET value = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (value == INVALID_SOCKET) return false;
        BOOL report_connection_reset = FALSE;
        DWORD bytes_returned = 0;
        if (WSAIoctl(value, sio_udp_connreset, &report_connection_reset,
                     sizeof(report_connection_reset), nullptr, 0, &bytes_returned,
                     nullptr, nullptr) != 0) {
            closesocket(value);
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(value, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 ||
            WSAAsyncSelect(value, window_, udp_message, FD_READ) != 0) {
            closesocket(value);
            return false;
        }
        int length = sizeof(address);
        if (getsockname(value, reinterpret_cast<sockaddr *>(&address), &length) != 0 ||
            ntohs(address.sin_port) < 1024) {
            closesocket(value);
            return false;
        }
        TargetRuntime runtime;
        runtime.socket = value;
        if (!targets_.emplace(target_id, std::move(runtime)).second) {
            closesocket(value);
            return false;
        }
        socket_targets_.emplace(value, target_id);
        return true;
    }

    void close_target(std::uint64_t target_id) {
        const auto target = targets_.find(target_id);
        if (target == targets_.end()) return;
        if (target->second.socket != INVALID_SOCKET) {
            WSAAsyncSelect(target->second.socket, window_, 0, 0);
            socket_targets_.erase(target->second.socket);
            retiring_sockets_.insert(target->second.socket);
            if (!window_ || !PostMessageW(window_, retire_socket_message,
                                          static_cast<WPARAM>(target->second.socket), 0)) {
                retiring_sockets_.erase(target->second.socket);
                closesocket(target->second.socket);
            }
        }
        targets_.erase(target);
    }

    void fail_target(std::uint64_t target_id, std::string_view stage,
                     std::string_view code) {
        if (targets_.find(target_id) == targets_.end()) return;
        if (control_mode_ == ControlMode::hub) {
            q2dz::HubCommand drop;
            drop.type = q2dz::HubCommandType::drop;
            drop.id_or_generation = target_id;
            try {
                post_hub_command(drop);
            } catch (...) {
                close_target(target_id);
                fail("webview_drop_failed");
                return;
            }
        }
        emit_failed(target_id, stage, code);
        close_target(target_id);
        if (legacy_mode_) {
            failed_ = true;
            exit_code_ = 2;
            if (window_) PostMessageW(window_, WM_CLOSE, 0, 0);
        }
    }

    void emit_ready(std::uint64_t target_id, const TargetRuntime &target, std::uint32_t peer) {
        int length = sizeof(sockaddr_in);
        sockaddr_in address{};
        require(getsockname(target.socket, reinterpret_cast<sockaddr *>(&address), &length) == 0,
            "UDP endpoint readback failed");
        if (legacy_mode_) {
            std::cout << "Q2DZ/1 TRANSPORT_READY udp=127.0.0.1:" << ntohs(address.sin_port)
                      << " peer=" << peer << std::endl;
        } else {
            std::cout << "Q2DZ/HUB/2 READY id=" << target_id
                      << " udp=127.0.0.1:" << ntohs(address.sin_port)
                      << " peer=" << peer << std::endl;
        }
    }

    void emit_closed(std::uint64_t target_id, std::string_view reason) {
        if (legacy_mode_) {
            std::cout << "Q2DZ/1 ROOM_CLOSED reason=" << reason << std::endl;
        } else {
            std::cout << "Q2DZ/HUB/2 CLOSED id=" << target_id
                      << " reason=" << reason << std::endl;
        }
    }

    void emit_failed(std::uint64_t target_id, std::string_view stage,
                     std::string_view code) {
        if (legacy_mode_) {
            std::cout << "Q2DZ/1 FATAL stage=" << stage << " code=" << code << std::endl;
        } else {
            std::cout << "Q2DZ/HUB/2 FAILED id=" << target_id
                      << " stage=" << stage << " code=" << code << std::endl;
        }
    }

    void handle_hub_command(q2dz::HubCommand command) {
        switch (command.type) {
            case q2dz::HubCommandType::ice:
                post_hub_command(command);
                break;
            case q2dz::HubCommandType::put: {
                const auto id = command.id_or_generation;
                if (targets_.contains(id)) break;
                if (targets_.size() >= max_targets) {
                    emit_failed(id, "bind", "target_capacity_exceeded");
                    break;
                }
                if (!bind_target_socket(id)) {
                    emit_failed(id, "bind", "udp_socket_failed");
                    break;
                }
                try {
                    post_hub_command(command);
                } catch (...) {
                    close_target(id);
                    throw;
                }
                break;
            }
            case q2dz::HubCommandType::drop:
                post_hub_command(command);
                close_target(command.id_or_generation);
                break;
            case q2dz::HubCommandType::stop:
                post_hub_command(command);
                stop_requested_ = true;
                if (window_) PostMessageW(window_, WM_CLOSE, 0, 0);
                break;
        }
    }

    void drain_control() {
        ControlMode mode;
        bool legacy_ready;
        bool eof;
        bool error;
        std::string error_code;
        std::string legacy;
        std::deque<std::string> lines;
        {
            std::lock_guard lock(control_mutex_);
            mode = control_mode_;
            legacy_ready = legacy_ready_;
            eof = control_eof_;
            error = control_error_;
            error_code = control_error_code_;
            if (page_ready_ && mode == ControlMode::legacy && legacy_ready && !legacy_sent_) {
                legacy = std::move(legacy_configuration_);
                secure_clear(legacy_configuration_);
                legacy_sent_ = true;
            }
            if (page_ready_ && mode == ControlMode::hub) {
                while (!control_lines_.empty() && lines.size() < max_control_commands_per_tick) {
                    queued_control_bytes_ -= control_lines_.front().size();
                    lines.emplace_back(std::move(control_lines_.front()));
                    control_lines_.pop_front();
                }
            }
        }
        if (page_ready_) legacy_mode_ = mode == ControlMode::legacy;
        if (error) {
            fail(error_code.empty() ? "control_failed" : error_code.c_str());
            return;
        }
        if (!page_ready_) return;
        if (!legacy.empty()) {
            if (!targets_.contains(1) && !bind_target_socket(1)) {
                fail("udp_socket_failed");
                secure_clear(legacy);
                return;
            }
            try {
                post_legacy_configuration(std::move(legacy));
            } catch (...) {
                secure_clear(legacy);
                close_target(1);
                fail("control_dispatch_failed");
            }
            secure_clear(legacy);
            return;
        }
        for (auto &line : lines) {
            auto parsed = hub_parser_.parse(line);
            if (!parsed.command) {
                secure_clear(line);
                fail("invalid_hub_command");
                return;
            }
            secure_clear(line);
            try {
                handle_hub_command(std::move(*parsed.command));
            } catch (const std::exception &) {
                fail("control_dispatch_failed");
                return;
            }
            if (stop_requested_) return;
        }
        {
            std::lock_guard lock(control_mutex_);
            if (control_error_) {
                error = true;
                error_code = control_error_code_;
            }
            eof = control_eof_ && control_lines_.empty();
        }
        if (error) {
            fail(error_code.empty() ? "control_failed" : error_code.c_str());
        } else if (eof && !stop_requested_) {
            q2dz::HubCommand stop;
            stop.type = q2dz::HubCommandType::stop;
            post_hub_command(stop);
            stop_requested_ = true;
            if (window_) PostMessageW(window_, WM_CLOSE, 0, 0);
        } else {
            bool more = false;
            {
                std::lock_guard lock(control_mutex_);
                more = !control_lines_.empty();
            }
            if (more) post_control_message();
        }
    }

    void handle_web_message(const std::string &message) {
        if (message == page_ready) {
            {
                std::lock_guard lock(control_mutex_);
                page_ready_ = true;
                legacy_mode_ = control_mode_ == ControlMode::legacy;
            }
            drain_control();
            return;
        }
        if (const auto cadence = parse_cadence_message(message)) {
            log_cadence(*cadence);
            return;
        }
        if (message.rfind("{\"type\":\"cadence\"", 0) == 0) {
            ++invalid_cadence_drops_;
            return;
        }
        if (message.rfind("{\"type\":\"udp\"", 0) == 0) {
            const auto tagged = decode_tagged_datagram_message(message);
            const auto target = targets_.find(tagged.target_id);
            if (target == targets_.end()) {
                ++unknown_peer_drops_;
                return;
            }
            auto &runtime = target->second;
            ++native_cadence_.web_to_native_datagrams;
            ++runtime.counters.web_to_native_datagrams;
            record_accepted_datagram(tagged.datagram.size(), max_transport_to_udp_datagram_);
            record_accepted_datagram(tagged.datagram.size(),
                                     runtime.counters.max_transport_to_udp_datagram);
            if (!runtime.have_client || !runtime.ready) {
                ++transport_not_ready_drops_;
                ++runtime.counters.transport_not_ready_drops;
                ++runtime.counters.dropped_packets;
                return;
            }
            const int sent = sendto(runtime.socket,
                reinterpret_cast<const char *>(tagged.datagram.data()),
                static_cast<int>(tagged.datagram.size()), 0,
                reinterpret_cast<const sockaddr *>(&runtime.client),
                sizeof(runtime.client));
            if (sent != static_cast<int>(tagged.datagram.size())) {
                ++runtime.counters.dropped_packets;
                fail_target(tagged.target_id, "udp", "udp_send_failed");
                return;
            }
            ++native_cadence_.udp_to_q2_datagrams;
            native_cadence_.udp_to_q2_bytes += tagged.datagram.size();
            ++runtime.counters.udp_to_q2_datagrams;
            runtime.counters.udp_to_q2_bytes += tagged.datagram.size();
            record_cadence_gap(GetTickCount64(), last_udp_to_q2_,
                               native_cadence_.udp_to_q2_max_gap_ms);
            ++web_to_udp_;
            return;
        }
        if (record_unknown_peer_drop(message, unknown_peer_drops_)) return;
        if (record_oversized_transport_drop(message, oversized_transport_drops_)) return;
        if (message.find("\"type\":\"drop\"") != std::string::npos) return;
        if (const auto ready = parse_transport_ready_message(message)) {
            const auto target = targets_.find(ready->target_id);
            if (target == targets_.end()) {
                ++unknown_peer_drops_;
                return;
            }
            if (target->second.ready) {
                fail("invalid_ready_state");
                return;
            }
            target->second.ready = true;
            emit_ready(ready->target_id, target->second, ready->peer);
            return;
        }
        if (const auto state = parse_target_state_message(message)) {
            const auto target = targets_.find(state->target_id);
            if (target == targets_.end()) {
                ++unknown_peer_drops_;
                return;
            }
            const bool was_legacy = legacy_mode_;
            if (state->value == "room-closed") emit_closed(state->target_id, state->reason);
            else if (state->value == "failed") {
                emit_failed(state->target_id, transport_failure_stage(state->reason), state->reason);
            }
            close_target(state->target_id);
            if (was_legacy && state->value == "failed") {
                failed_ = true;
                exit_code_ = 2;
            }
            if (was_legacy && window_) PostMessageW(window_, WM_CLOSE, 0, 0);
            return;
        }
        if (legacy_mode_) {
            if (const auto peer = parse_transport_ready_peer(message)) {
                const auto target = targets_.find(1);
                if (target == targets_.end() || target->second.ready) {
                    fail("invalid_ready_state");
                    return;
                }
                target->second.ready = true;
                emit_ready(1, target->second, *peer);
                return;
            }
            if (const auto reason = parse_room_closed_reason(message)) {
                emit_closed(1, *reason);
                close_target(1);
                if (window_) PostMessageW(window_, WM_CLOSE, 0, 0);
                return;
            }
            if (const auto reason = parse_transport_failed_reason(message)) {
                emit_failed(1, transport_failure_stage(*reason), *reason);
                close_target(1);
                failed_ = true;
                exit_code_ = 2;
                if (window_) PostMessageW(window_, WM_CLOSE, 0, 0);
                return;
            }
        }
        if (message.find("\"type\":\"state\"") != std::string::npos) fail("invalid_state_message");
        else if (message.find("\"type\":\"udp\"") != std::string::npos) fail("invalid_udp_message");
        else fail("unexpected_web_message");
    }

    void receive_udp(SOCKET socket_value) {
        const auto found = socket_targets_.find(socket_value);
        if (found == socket_targets_.end()) return;
        const auto id = found->second;
        std::array<std::uint8_t, 65536> buffer{};
        for (std::size_t packet = 0; packet < max_udp_packets_per_message; ++packet) {
            auto target = targets_.find(id);
            if (target == targets_.end()) return;
            sockaddr_in source{};
            int source_length = sizeof(source);
            const int length = recvfrom(socket_value, reinterpret_cast<char *>(buffer.data()),
                static_cast<int>(buffer.size()), 0,
                reinterpret_cast<sockaddr *>(&source), &source_length);
            if (length == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) return;
                fail_target(id, "udp", "udp_receive_failed");
                return;
            }
            if (length < 0) {
                ++target->second.counters.dropped_packets;
                fail_target(id, "transport", "connection_closed");
                return;
            }
            if (length < 1) continue;
            if (record_oversized_datagram(static_cast<std::size_t>(length), oversized_udp_drops_)) {
                ++target->second.counters.oversized_udp_drops;
                ++target->second.counters.dropped_packets;
                continue;
            }
            if (!loopback_source(source)) {
                ++target->second.counters.dropped_packets;
                continue;
            }
            if (!target->second.have_client) {
                target->second.client = source;
                target->second.have_client = true;
            } else if (!same_source(target->second.client, source)) {
                ++second_source_drops_;
                ++target->second.counters.second_source_drops;
                ++target->second.counters.dropped_packets;
                continue;
            }
            ++native_cadence_.udp_from_q2_datagrams;
            native_cadence_.udp_from_q2_bytes += static_cast<std::uint64_t>(length);
            ++target->second.counters.udp_from_q2_datagrams;
            target->second.counters.udp_from_q2_bytes += static_cast<std::uint64_t>(length);
            if (!target->second.ready) {
                ++transport_not_ready_drops_;
                ++target->second.counters.transport_not_ready_drops;
                ++target->second.counters.dropped_packets;
                continue;
            }
            const std::vector<std::uint8_t> datagram(buffer.begin(), buffer.begin() + length);
            record_accepted_datagram(datagram.size(), max_udp_to_transport_datagram_);
            record_accepted_datagram(datagram.size(),
                                     target->second.counters.max_udp_to_transport_datagram);
            try {
                post_bridge_json(encode_datagram_message(id, datagram));
            } catch (...) {
                ++target->second.counters.dropped_packets;
                fail_target(id, "bridge", "webview_udp_post_failed");
                return;
            }
            ++native_cadence_.post_to_web_datagrams;
            ++target->second.counters.post_to_web_datagrams;
            record_cadence_gap(GetTickCount64(), last_post_to_web_,
                               native_cadence_.post_to_web_max_gap_ms);
            ++udp_to_web_;
        }
    }

    void log_cadence(const CadenceSnapshot &js) {
        const auto native = std::exchange(native_cadence_, NativeCadence{});
        std::ostringstream line;
        line << "Q2DZ_CADENCE"
             << " timerTicks=" << js.timer_ticks << " timerMaxGapMs=" << js.timer_max_gap_ms
             << " waitCalls=" << js.wait_calls << " aliasPollIterations=" << js.alias_poll_iterations
             << " probeSendCount=" << js.probe_send_count << " probeAckCount=" << js.probe_ack_count
             << " dcToNativeDatagrams=" << js.dc_to_native_datagrams
             << " dcToNativeBytes=" << js.dc_to_native_bytes
             << " dcToNativeMaxGapMs=" << js.dc_to_native_max_gap_ms
             << " nativeToDcDatagrams=" << js.native_to_dc_datagrams
             << " nativeToDcBytes=" << js.native_to_dc_bytes
             << " nativeToDcMaxGapMs=" << js.native_to_dc_max_gap_ms
             << " udpFromQ2Datagrams=" << native.udp_from_q2_datagrams
             << " udpFromQ2Bytes=" << native.udp_from_q2_bytes
             << " udpToQ2Datagrams=" << native.udp_to_q2_datagrams
             << " udpToQ2Bytes=" << native.udp_to_q2_bytes
             << " postToWebDatagrams=" << native.post_to_web_datagrams
             << " webToNativeDatagrams=" << native.web_to_native_datagrams
             << " udpToQ2MaxGapMs=" << native.udp_to_q2_max_gap_ms
             << " postToWebMaxGapMs=" << native.post_to_web_max_gap_ms << "\n";
        std::cerr << line.str();
    }

    void emit_heartbeat() {
        if (!page_ready_ || legacy_mode_ || stopped_ || failed_) return;
        std::size_t ready = 0;
        for (const auto &[id, target] : targets_) {
            static_cast<void>(id);
            if (target.ready) ++ready;
        }
        std::cout << "Q2DZ/HUB/2 HEARTBEAT state=ok targets=" << targets_.size()
                  << " ready=" << ready << std::endl;
    }

    void fail(const char *reason) {
        if (failed_) return;
        failed_ = true;
        exit_code_ = 2;
        ControlMode mode;
        {
            std::lock_guard lock(control_mutex_);
            mode = control_mode_;
        }
        if (mode == ControlMode::hub) {
            std::cout << "Q2DZ/HUB/2 FATAL stage=gateway code=" << reason << std::endl;
        } else if (mode == ControlMode::legacy || mode == ControlMode::unknown || legacy_mode_) {
            std::cout << "Q2DZ/1 FATAL stage=gateway code=" << reason << std::endl;
        } else {
            std::vector<std::uint64_t> ids;
            ids.reserve(targets_.size());
            for (const auto &[id, target] : targets_) {
                static_cast<void>(target);
                ids.push_back(id);
            }
            for (const auto id : ids) {
                emit_failed(id, "gateway", reason);
                close_target(id);
            }
        }
        std::cerr << "GATEWAY_ERROR reason=" << reason << "\n";
        if (window_) PostMessageW(window_, WM_CLOSE, 0, 0);
    }

    void shutdown() {
        if (shutdown_complete_) return;
        shutdown_complete_ = true;
        if (window_) KillTimer(window_, heartbeat_timer);
        if (webview_) {
            try { webview_->PostWebMessageAsJson(L"{\"type\":\"stop\"}"); } catch (...) {}
        }
        if (controller_) controller_->Close();
        webview_.Reset();
        controller_.Reset();
        environment_.Reset();
        const auto active_targets = targets_.size();
        for (const auto &[id, target] : targets_) {
            static_cast<void>(id);
            if (target.socket != INVALID_SOCKET) closesocket(target.socket);
        }
        targets_.clear();
        socket_targets_.clear();
        for (const auto socket : retiring_sockets_) closesocket(socket);
        retiring_sockets_.clear();
        if (winsock_started_) {
            WSACleanup();
            winsock_started_ = false;
        }
        if (control_thread_.joinable()) {
            (void)CancelSynchronousIo(control_thread_.native_handle());
            control_thread_.join();
        }
        if (owner_thread_.joinable()) {
            (void)CancelSynchronousIo(owner_thread_.native_handle());
            owner_thread_.join();
        }
        {
            std::lock_guard lock(control_mutex_);
            for (auto &line : control_lines_) secure_clear(line);
            control_lines_.clear();
            queued_control_bytes_ = 0;
            secure_clear(legacy_configuration_);
            secure_clear(hub_pending_);
            secure_clear(control_error_code_);
        }
        if (options_.owner) {
            CloseHandle(options_.owner);
            options_.owner = nullptr;
        }
        std::error_code error;
        if (!user_data_.empty()) {
            for (int attempt = 0; attempt < 100 && std::filesystem::exists(user_data_); ++attempt) {
                error.clear();
                std::filesystem::remove_all(user_data_, error);
                if (std::filesystem::exists(user_data_)) Sleep(50);
            }
        }
        std::cerr << "GATEWAY_STOP udp_to_web=" << udp_to_web_
                  << " web_to_udp=" << web_to_udp_
                  << " targets=" << active_targets
                  << " max_udp_to_transport_datagram=" << max_udp_to_transport_datagram_
                  << " max_transport_to_udp_datagram=" << max_transport_to_udp_datagram_
                  << " oversized_udp_drops=" << oversized_udp_drops_
                  << " oversized_transport_drops=" << oversized_transport_drops_
                  << " unknown_peer_drops=" << unknown_peer_drops_
                  << " second_source_drops=" << second_source_drops_
                  << " transport_not_ready_drops=" << transport_not_ready_drops_
                  << " invalid_cadence_drops=" << invalid_cadence_drops_ << "\n";
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                        WPARAM wparam, LPARAM lparam) {
        Gateway *gateway = reinterpret_cast<Gateway *>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            gateway = static_cast<Gateway *>(
                reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
            gateway->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(gateway));
        }
        return gateway ? gateway->on_window_message(message, wparam, lparam)
                       : DefWindowProcW(window, message, wparam, lparam);
    }

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    bool winsock_started_ = false;
    bool page_ready_ = false;
    bool legacy_mode_ = false;
    bool legacy_sent_ = false;
    bool stopped_ = false;
    bool failed_ = false;
    bool stop_requested_ = false;
    bool shutdown_complete_ = false;
    int exit_code_ = 0;
    Options options_;
    std::filesystem::path user_data_;
    std::thread owner_thread_;
    std::thread control_thread_;
    std::mutex control_mutex_;
    ControlMode control_mode_ = ControlMode::unknown;
    std::deque<std::string> control_lines_;
    std::size_t queued_control_bytes_ = 0;
    std::string legacy_configuration_;
    std::string hub_pending_;
    std::string control_error_code_;
    bool legacy_ready_ = false;
    bool control_eof_ = false;
    bool control_error_ = false;
    q2dz::HubCommandParser hub_parser_{q2dz::HubCommandChannel::hub};
    std::unordered_map<std::uint64_t, TargetRuntime> targets_;
    std::unordered_map<SOCKET, std::uint64_t> socket_targets_;
    std::unordered_set<SOCKET> retiring_sockets_;
    std::uint64_t udp_to_web_ = 0;
    std::uint64_t web_to_udp_ = 0;
    std::uint64_t transport_not_ready_drops_ = 0;
    std::size_t max_udp_to_transport_datagram_ = 0;
    std::size_t max_transport_to_udp_datagram_ = 0;
    std::uint64_t oversized_udp_drops_ = 0;
    std::uint64_t oversized_transport_drops_ = 0;
    std::uint64_t unknown_peer_drops_ = 0;
    std::uint64_t second_source_drops_ = 0;
    std::uint64_t invalid_cadence_drops_ = 0;
    NativeCadence native_cadence_;
    std::uint64_t last_udp_to_q2_ = 0;
    std::uint64_t last_post_to_web_ = 0;
    ComPtr<ICoreWebView2Environment> environment_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> webview_;
};

}  // namespace

int wmain(int argc, wchar_t **argv) {
    try {
        require_hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED),
            "COM initialization failed");
        require(SetConsoleCtrlHandler(console_control_handler, TRUE) != 0,
            "console control handler setup failed");
        Gateway gateway(parse_options(argc, argv));
        const int result = gateway.run(GetModuleHandleW(nullptr));
        SetConsoleCtrlHandler(console_control_handler, FALSE);
        CoUninitialize();
        return result;
    } catch (const std::exception &error) {
        static_cast<void>(error);
        std::cout << "Q2DZ/1 FATAL stage=startup code=startup_failed" << std::endl;
        std::cerr << "GATEWAY_ERROR reason=startup_failed\n";
        CoUninitialize();
        return 2;
    }
}
