#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

#include "webview_bridge_protocol.h"
#include "web_message_dispatch.hpp"

#include <wrl.h>
#include <unknwn.h>
#include <WebView2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT udp_message = WM_APP + 1;
constexpr DWORD sio_udp_connreset = _WSAIOW(IOC_VENDOR, 12);
constexpr BOOL controller_visible = TRUE;
constexpr char page_ready[] =
    "{\"type\":\"state\",\"value\":\"page-ready\",\"reason\":\"\"}";
std::atomic<HWND> gateway_window{nullptr};

BOOL WINAPI console_control_handler(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT &&
        event != CTRL_CLOSE_EVENT) {
        return FALSE;
    }
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
    void operator()(wchar_t *value) const noexcept { CoTaskMemFree(value); }
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

std::string read_configuration() {
    constexpr std::size_t maximum = 256 * 1024;
    std::string value;
    std::array<char, 8192> buffer{};
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    require(input && input != INVALID_HANDLE_VALUE, "configuration stdin is unavailable");
    for (;;) {
        DWORD length = 0;
        if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &length, nullptr)) {
            require(GetLastError() == ERROR_BROKEN_PIPE, "configuration read failed");
            break;
        }
        if (length == 0) break;
        require(value.size() + length <= maximum, "configuration is too large");
        value.append(buffer.data(), length);
    }
    require(value.find('\0') == std::string::npos, "configuration contains NUL");
    const auto first = value.find_first_not_of(" \t\r\n");
    const auto last = value.find_last_not_of(" \t\r\n");
    require(first != std::string::npos && value[first] == '{' && value[last] == '}',
        "configuration must be one JSON object");
    return value;
}

void secure_clear(std::string &value) {
    if (!value.empty()) SecureZeroMemory(value.data(), value.size());
    value.clear();
    value.shrink_to_fit();
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

class Gateway {
public:
    Gateway(Options options, std::string configuration)
        : options_(std::move(options)), configuration_(std::move(configuration)) {}

    ~Gateway() { shutdown(); }

    int run(HINSTANCE instance) {
        instance_ = instance;
        create_window();
        watch_owner();
        create_socket();
        start_webview();

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return exit_code_;
    }

    LRESULT on_window_message(UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == udp_message) {
            if (WSAGETSELECTERROR(lparam) != 0) {
                fail("udp_async_error");
            } else if (WSAGETSELECTEVENT(lparam) == FD_READ) {
                receive_udp();
            }
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

    void create_socket() {
        WSADATA data{};
        require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "WSAStartup failed");
        winsock_started_ = true;
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        require(socket_ != INVALID_SOCKET, "UDP socket creation failed");
        BOOL report_connection_reset = FALSE;
        DWORD bytes_returned = 0;
        require(WSAIoctl(socket_, sio_udp_connreset, &report_connection_reset,
                    sizeof(report_connection_reset), nullptr, 0, &bytes_returned,
                    nullptr, nullptr) == 0,
            "UDP connection-reset mode failed");

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        require(bind(socket_, reinterpret_cast<const sockaddr *>(&address),
                     sizeof(address)) == 0, "UDP bind failed");
        require(WSAAsyncSelect(socket_, window_, udp_message, FD_READ) == 0,
            "UDP async select failed");

        int length = sizeof(address);
        require(getsockname(socket_, reinterpret_cast<sockaddr *>(&address), &length) == 0,
            "UDP endpoint readback failed");
        bound_port_ = ntohs(address.sin_port);
        std::cerr << "GATEWAY_LISTENING bind=127.0.0.1:" << bound_port_ << "\n";
    }

    void start_webview() {
        user_data_ = options_.user_data;
        std::filesystem::create_directories(user_data_);

        LPWSTR browser_version = nullptr;
        require_hr(GetAvailableCoreWebView2BrowserVersionString(nullptr, &browser_version),
            "WebView2 Evergreen Runtime is unavailable");
        std::wcerr << L"WEBVIEW2_RUNTIME version=" << browser_version
                   << L" controller_visible="
                   << (controller_visible ? L"true" : L"false")
                   << L" diagnostic_background_timer_flag_exact="
                   << (diagnostic_background_timer_flag_exact() ? L"true" : L"false")
                   << L"\n";
        CoTaskMemFree(browser_version);

        require_hr(CreateCoreWebView2EnvironmentWithOptions(nullptr,
            user_data_.c_str(), nullptr,
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
        require_hr(settings->put_AreDefaultContextMenusEnabled(FALSE),
            "context menu disable failed");
        require_hr(settings->put_AreDefaultScriptDialogsEnabled(FALSE),
            "script dialog disable failed");
        require_hr(settings->put_IsStatusBarEnabled(FALSE), "status bar disable failed");
        require_hr(settings->put_AreHostObjectsAllowed(FALSE), "host objects disable failed");

        ComPtr<ICoreWebView2_3> webview3;
        require_hr(webview_.As(&webview3), "WebView2 virtual host API unavailable");
        require_hr(webview3->SetVirtualHostNameToFolderMapping(
            L"q2doszone.local", options_.assets.c_str(),
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS),
            "virtual host mapping failed");

        EventRegistrationToken navigation_starting_token{};
        require_hr(webview_->add_NavigationStarting(
            Callback<ICoreWebView2NavigationStartingEventHandler>(
                [](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                    LPWSTR uri = nullptr;
                    if (FAILED(args->get_Uri(&uri)) || !uri) return E_FAIL;
                    const std::wstring_view value(uri);
                    const bool allowed = value.rfind(L"https://q2doszone.local/", 0) == 0;
                    CoTaskMemFree(uri);
                    return args->put_Cancel(!allowed);
                }).Get(), &navigation_starting_token),
            "NavigationStarting registration failed");
        EventRegistrationToken new_window_requested_token{};
        require_hr(webview_->add_NewWindowRequested(
            Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
                    return args->put_Handled(TRUE);
                }).Get(), &new_window_requested_token),
            "NewWindowRequested registration failed");
        EventRegistrationToken permission_requested_token{};
        require_hr(webview_->add_PermissionRequested(
            Callback<ICoreWebView2PermissionRequestedEventHandler>(
                [](ICoreWebView2 *, ICoreWebView2PermissionRequestedEventArgs *args) -> HRESULT {
                    return args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                }).Get(), &permission_requested_token),
            "PermissionRequested registration failed");
        EventRegistrationToken process_failed_token{};
        require_hr(webview_->add_ProcessFailed(
            Callback<ICoreWebView2ProcessFailedEventHandler>(
                [this](ICoreWebView2 *, ICoreWebView2ProcessFailedEventArgs *) -> HRESULT {
                    fail("webview_process_failed");
                    return S_OK;
                }).Get(), &process_failed_token),
            "ProcessFailed registration failed");
        EventRegistrationToken web_message_received_token{};
        require_hr(webview_->add_WebMessageReceived(
            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [this](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args)
                    -> HRESULT {
                    LPWSTR raw = nullptr;
                    if (FAILED(args->get_WebMessageAsJson(&raw)) || !raw) {
                        fail("web_message_read_failed");
                        return S_OK;
                    }
                    try {
                        with_owned_web_message(raw, CoTaskMemString::deleter_type{},
                            [this](wchar_t *value) {
                                handle_web_message(wide_to_utf8(value));
                            });
                    } catch (const std::exception &) {
                        fail("invalid_web_message");
                    }
                    return S_OK;
                }).Get(), &web_message_received_token),
            "WebMessageReceived registration failed");

        require_hr(webview_->Navigate(L"https://q2doszone.local/transport.html"),
            "transport navigation failed");
    }

    void log_cadence(const CadenceSnapshot &js) {
        const auto native = std::exchange(native_cadence_, NativeCadence{});
        std::ostringstream line;
        line << "Q2DZ_CADENCE"
             << " timerTicks=" << js.timer_ticks
             << " timerMaxGapMs=" << js.timer_max_gap_ms
             << " waitCalls=" << js.wait_calls
             << " aliasPollIterations=" << js.alias_poll_iterations
             << " probeSendCount=" << js.probe_send_count
             << " probeAckCount=" << js.probe_ack_count
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

    void handle_web_message(const std::string &message) {
        if (message == page_ready) {
            require_hr(webview_->PostWebMessageAsJson(
                utf8_to_wide(configuration_).c_str()),
                "start message failed");
            secure_clear(configuration_);
            return;
        }
        if (const auto peer = parse_transport_ready_peer(message)) {
            transport_ready_ = true;
            std::cout << "Q2DZ/1 TRANSPORT_READY udp=127.0.0.1:" << bound_port_
                      << " peer=" << *peer << std::endl;
            return;
        }
        if (const auto reason = parse_room_closed_reason(message)) {
            std::cout << "Q2DZ/1 ROOM_CLOSED reason=" << *reason << std::endl;
            if (window_) PostMessageW(window_, WM_CLOSE, 0, 0);
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
        if (message.rfind("{\"type\":\"udp\",", 0) == 0) {
            const auto datagram = decode_datagram_message(message);
            ++native_cadence_.web_to_native_datagrams;
            record_accepted_datagram(datagram.size(), max_transport_to_udp_datagram_);
            if (have_client_) {
                const int sent = sendto(socket_,
                    reinterpret_cast<const char *>(datagram.data()),
                    static_cast<int>(datagram.size()), 0,
                    reinterpret_cast<const sockaddr *>(&client_), sizeof(client_));
                require(sent == static_cast<int>(datagram.size()), "udp_send_failed");
                ++native_cadence_.udp_to_q2_datagrams;
                native_cadence_.udp_to_q2_bytes += datagram.size();
                record_cadence_gap(GetTickCount64(), last_udp_to_q2_,
                                   native_cadence_.udp_to_q2_max_gap_ms);
                ++web_to_udp_;
            }
            return;
        }
        if (record_oversized_transport_drop(message, oversized_transport_drops_)) return;
        if (message == "{\"type\":\"drop\",\"reason\":\"transport_not_ready\"}") {
            ++transport_not_ready_drops_;
            return;
        }
        if (const auto reason = parse_transport_failed_reason(message)) {
            fail(reason->c_str());
            return;
        }
        if (message.find("\"type\":\"state\"") != std::string::npos) {
            if (message.find("\"value\":\"failed\"") != std::string::npos)
                fail("invalid_failed_state");
            else if (message.find("\"value\":\"transport-ready\"") != std::string::npos)
                fail("invalid_ready_state");
            else if (message.find("\"value\":\"room-closed\"") != std::string::npos)
                fail("invalid_room_closed_state");
            else
                fail("unexpected_state");
        } else if (message.find("\"type\":\"udp\"") != std::string::npos) {
            fail("invalid_udp_message");
        } else if (message.find("\"type\":\"drop\"") != std::string::npos) {
            fail("invalid_drop_message");
        } else {
            fail("unexpected_web_message");
        }
    }

    void receive_udp() {
        std::array<std::uint8_t, 65536> buffer{};
        for (;;) {
            sockaddr_in source{};
            int source_length = sizeof(source);
            const int length = recvfrom(socket_, reinterpret_cast<char *>(buffer.data()),
                static_cast<int>(buffer.size()), 0,
                reinterpret_cast<sockaddr *>(&source), &source_length);
            if (length == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) return;
                    fail("udp_receive_failed");
                return;
            }
            if (length < 1 || record_oversized_datagram(
                    static_cast<std::size_t>(length), oversized_udp_drops_)) continue;
                if (!have_client_) {
                    client_ = source;
                    have_client_ = true;
                    std::cerr << "UDP_CLIENT_CAPTURED\n";
            } else if (!same_source(client_, source)) {
                ++second_source_drops_;
                continue;
            }
            ++native_cadence_.udp_from_q2_datagrams;
            native_cadence_.udp_from_q2_bytes += static_cast<std::uint64_t>(length);
            if (!transport_ready_) {
                ++transport_not_ready_drops_;
                continue;
            }
            const std::vector<std::uint8_t> datagram(buffer.begin(), buffer.begin() + length);
            record_accepted_datagram(datagram.size(), max_udp_to_transport_datagram_);
                require_hr(webview_->PostWebMessageAsJson(
                    utf8_to_wide(encode_datagram_message(datagram)).c_str()),
                    "webview_udp_post_failed");
            ++native_cadence_.post_to_web_datagrams;
            record_cadence_gap(GetTickCount64(), last_post_to_web_,
                               native_cadence_.post_to_web_max_gap_ms);
            ++udp_to_web_;
        }
    }

    void fail(const char *reason) {
        if (failed_) return;
        failed_ = true;
        exit_code_ = 2;
        std::cout << "Q2DZ/1 FATAL stage=gateway code=" << reason << std::endl;
        std::cerr << "GATEWAY_ERROR reason=" << reason << "\n";
        if (window_) PostMessageW(window_, WM_CLOSE, 0, 0);
    }

    void shutdown() {
        secure_clear(configuration_);
        if (webview_) webview_->PostWebMessageAsJson(L"{\"type\":\"stop\"}");
        if (controller_) controller_->Close();
        webview_.Reset();
        controller_.Reset();
        environment_.Reset();
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
        if (winsock_started_) {
            WSACleanup();
            winsock_started_ = false;
        }
        if (owner_thread_.joinable()) {
            (void)CancelSynchronousIo(owner_thread_.native_handle());
            owner_thread_.join();
        }
        if (options_.owner) {
            CloseHandle(options_.owner);
            options_.owner = nullptr;
        }
        std::error_code error;
        if (!user_data_.empty()) {
            for (int attempt = 0; attempt < 100 && std::filesystem::exists(user_data_);
                 ++attempt) {
                error.clear();
                std::filesystem::remove_all(user_data_, error);
                if (std::filesystem::exists(user_data_)) Sleep(50);
            }
        }
        std::cerr << "GATEWAY_STOP udp_to_web=" << udp_to_web_
                  << " web_to_udp=" << web_to_udp_
                  << " max_udp_to_transport_datagram=" << max_udp_to_transport_datagram_
                  << " max_transport_to_udp_datagram=" << max_transport_to_udp_datagram_
                  << " oversized_udp_drops=" << oversized_udp_drops_
                  << " oversized_transport_drops=" << oversized_transport_drops_
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
    SOCKET socket_ = INVALID_SOCKET;
    bool winsock_started_ = false;
    Options options_;
    std::string configuration_;
    std::filesystem::path user_data_;
    std::thread owner_thread_;
    std::uint16_t bound_port_ = 0;
    sockaddr_in client_{};
    bool have_client_ = false;
    bool transport_ready_ = false;
    bool failed_ = false;
    int exit_code_ = 0;
    std::uint64_t udp_to_web_ = 0;
    std::uint64_t web_to_udp_ = 0;
    std::uint64_t transport_not_ready_drops_ = 0;
    std::size_t max_udp_to_transport_datagram_ = 0;
    std::size_t max_transport_to_udp_datagram_ = 0;
    std::uint64_t oversized_udp_drops_ = 0;
    std::uint64_t oversized_transport_drops_ = 0;
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
        Gateway gateway(parse_options(argc, argv), read_configuration());
        const int result = gateway.run(GetModuleHandleW(nullptr));
        SetConsoleCtrlHandler(console_control_handler, FALSE);
        CoUninitialize();
        return result;
    } catch (const std::exception &error) {
        std::cout << "Q2DZ/1 FATAL stage=startup code=startup_failed" << std::endl;
        std::cerr << "GATEWAY_ERROR reason=" << error.what() << "\n";
        CoUninitialize();
        return 2;
    }
}
