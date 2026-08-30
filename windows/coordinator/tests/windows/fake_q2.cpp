#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
std::wstring env(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) return {};
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
    if (written == 0 || written >= required) return {};
    value.resize(written);
    return value;
}

bool write_marker(const std::filesystem::path& directory,
                  const std::wstring& name,
                  std::string value = "1") {
    if (directory.empty()) return true;
    std::ofstream output((directory / (name + L".marker")).string(), std::ios::binary);
    if (!output) return false;
    output << value;
    return static_cast<bool>(output);
}

bool parse_source_url(const std::wstring& value, unsigned short& port) {
    constexpr std::wstring_view prefix = L"http://127.0.0.1:";
    constexpr std::wstring_view suffix = L"/servers";
    if (!value.starts_with(prefix) || !value.ends_with(suffix)) return false;
    const auto text = value.substr(prefix.size(), value.size() - prefix.size() - suffix.size());
    if (text.empty() || text.size() > 5) return false;
    const std::wstring number(text);
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long value_port = wcstoul(number.c_str(), &end, 10);
    if (errno != 0 || end != number.c_str() + number.size() || value_port < 1024 || value_port > 65535) return false;
    port = static_cast<unsigned short>(value_port);
    return true;
}

bool endpoint_line(std::string_view value) {
    constexpr std::string_view prefix = "127.0.0.1:";
    if (!value.starts_with(prefix)) return false;
    const auto text = value.substr(prefix.size());
    if (text.empty() || text.size() > 5) return false;
    const std::string number(text);
    char* end = nullptr;
    errno = 0;
    const unsigned long port = std::strtoul(number.c_str(), &end, 10);
    return errno == 0 && end != nullptr && *end == '\0' && port >= 1024 && port <= 65535;
}

bool read_source(unsigned short port, std::vector<std::string>& endpoints) {
    SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (connect(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        closesocket(socket_handle);
        return false;
    }
    constexpr char request[] = "GET /servers HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    if (send(socket_handle, request, static_cast<int>(sizeof(request) - 1), 0) == SOCKET_ERROR) {
        closesocket(socket_handle);
        return false;
    }
    std::string response;
    char buffer[4096];
    for (;;) {
        const int count = recv(socket_handle, buffer, sizeof(buffer), 0);
        if (count == 0) break;
        if (count == SOCKET_ERROR) {
            closesocket(socket_handle);
            return false;
        }
        response.append(buffer, static_cast<std::size_t>(count));
        if (response.size() > 64 * 1024) {
            closesocket(socket_handle);
            return false;
        }
    }
    closesocket(socket_handle);

    const auto separator = response.find("\r\n\r\n");
    if (separator == std::string::npos || response.rfind("HTTP/1.1 200 OK", 0) != 0) return false;
    const auto header_end = separator + 4;
    const auto length_begin = response.find("\r\nContent-Length: ");
    if (length_begin == std::string::npos || length_begin >= separator) return false;
    const auto number_begin = length_begin + std::string("\r\nContent-Length: ").size();
    const auto number_end = response.find("\r\n", number_begin);
    if (number_end == std::string::npos || number_end > separator) return false;
    const std::string length_text = response.substr(number_begin, number_end - number_begin);
    char* length_end = nullptr;
    errno = 0;
    const auto length = std::strtoul(length_text.c_str(), &length_end, 10);
    if (errno != 0 || length_end == nullptr || *length_end != '\0' || response.size() - header_end != length) return false;
    const auto body = std::string_view(response).substr(header_end);
    endpoints.clear();
    std::size_t start = 0;
    while (start < body.size()) {
        const auto end_line = body.find('\n', start);
        const auto end = end_line == std::string_view::npos ? body.size() : end_line;
        std::string_view line = body.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty()) {
            if (!endpoint_line(line) || endpoints.size() == 2) return false;
            endpoints.emplace_back(line);
        }
        if (end_line == std::string_view::npos) break;
        start = end_line + 1;
    }
    return true;
}

bool alive(unsigned long pid) {
    if (pid == 0) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    DWORD code = 0;
    const bool result = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return result;
}
}

int wmain(int argc, wchar_t** argv) {
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 20;
    const std::filesystem::path marker_directory = env(L"Q2DZ_PID_DIR");
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input == nullptr || input == INVALID_HANDLE_VALUE || GetFileType(input) == FILE_TYPE_PIPE) {
        WSACleanup();
        return 30;
    }

    const std::vector<std::wstring> arguments(argv + 1, argv + argc);
    if (arguments.size() != 7 || arguments[0] != L"+set" || arguments[1] != L"game" ||
        arguments[2] != L"3zb2" || arguments[3] != L"+set" ||
        arguments[4] != L"cl_serverbrowser_custom_source" ||
        arguments[6] != L"+serverbrowser_open_modern") {
        WSACleanup();
        return 30;
    }
    unsigned short source_port = 0;
    if (!parse_source_url(arguments[5], source_port)) {
        WSACleanup();
        return 30;
    }
    write_marker(marker_directory, L"q2.pid", std::to_string(GetCurrentProcessId()));
    write_marker(marker_directory, L"q2-started");
    const auto mode = env(L"Q2DZ_FAKE_Q2_MODE").empty() ? L"exit-normal" : env(L"Q2DZ_FAKE_Q2_MODE");

    if (mode == L"no-source-request") {
        Sleep(600000);
        WSACleanup();
        return 0;
    }

    std::vector<std::string> endpoints;
    bool source_ready = false;
    for (int attempt = 0; attempt < 100 && !source_ready; ++attempt) {
        source_ready = read_source(source_port, endpoints);
        if (!source_ready) Sleep(25);
    }
    if (!source_ready) {
        WSACleanup();
        return 31;
    }
    std::size_t observed_count = endpoints.size();
    write_marker(marker_directory, L"q2-source-empty", observed_count == 0 ? "1" : "0");
    write_marker(marker_directory, L"q2-source-" + std::to_wstring(observed_count));
    std::string source_url;
    source_url.reserve(arguments[5].size());
    for (const wchar_t character : arguments[5]) source_url.push_back(static_cast<char>(character));
    write_marker(marker_directory, L"q2-source-url", source_url);

    if (mode == L"hub-room-close" || mode == L"hub-room-crash") {
        for (int attempt = 0; attempt < 200 && observed_count < 1; ++attempt) {
            Sleep(25);
            if (read_source(source_port, endpoints)) observed_count = endpoints.size();
        }
        const auto marker = marker_directory / L"gateway-room-b.pid";
        std::ifstream stream(marker.string());
        unsigned long pid = 0;
        stream >> pid;
        if (alive(pid)) write_marker(marker_directory, L"hub-q2-observed-b-alive");
        if (mode == L"hub-room-crash") TerminateProcess(GetCurrentProcess(), 31);
        Sleep(250);
        WSACleanup();
        return 0;
    }
    if (mode == L"exit-during-burst") {
        write_marker(marker_directory, L"q2-burst-ready");
        Sleep(100);
        TerminateProcess(GetCurrentProcess(), 31);
    }
    if (mode == L"exit-normal") {
        Sleep(400);
        WSACleanup();
        return 0;
    }
    if (mode == L"crash") {
        Sleep(300);
        TerminateProcess(GetCurrentProcess(), 31);
    }

    while (true) {
        Sleep(50);
        if (!read_source(source_port, endpoints)) continue;
        if (endpoints.size() != observed_count) {
            observed_count = endpoints.size();
            write_marker(marker_directory, L"q2-source-" + std::to_wstring(observed_count));
        }
    }
}
#endif
