#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

std::wstring env(const wchar_t* name) {
    const wchar_t* value = _wgetenv(name);
    return value == nullptr ? L"" : value;
}

void wipe(std::string& value) noexcept {
    if (!value.empty()) SecureZeroMemory(value.data(), value.size());
    value.clear();
    value.shrink_to_fit();
}

void write_marker(const std::filesystem::path& directory, const std::wstring& name,
                  std::string_view value = "1") {
    if (directory.empty()) return;
    std::ofstream output((directory / (name + L".marker")).string(), std::ios::binary);
    if (output) output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

void write_pid(const std::filesystem::path& directory) {
    if (directory.empty()) return;
    std::ofstream output((directory / L"gateway.pid").string(), std::ios::binary);
    if (output) output << GetCurrentProcessId();
}

std::string room_id(std::string_view json) {
    constexpr std::string_view key = "\"roomId\":\"";
    const auto start = json.find(key);
    if (start == std::string_view::npos) return {};
    const auto first = start + key.size();
    const auto last = json.find('"', first);
    return last == std::string_view::npos ? std::string{} : std::string(json.substr(first, last - first));
}

std::wstring mode_for(std::string_view room) {
    std::wstring suffix;
    if (room == "room-a") suffix = L"_A";
    else if (room == "room-b") suffix = L"_B";
    if (!suffix.empty()) {
        const auto selected = env((L"Q2DZ_FAKE_GATEWAY_MODE" + suffix).c_str());
        if (!selected.empty()) return selected;
    }
    const auto selected = env(L"Q2DZ_FAKE_GATEWAY_MODE");
    return selected.empty() ? L"normal" : selected;
}

bool parse_id(std::string_view value, std::uint64_t& id) {
    if (value.empty()) return false;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), id);
    return error == std::errc{} && end == value.data() + value.size() && id != 0;
}

enum class CommandType { ice, put, drop, stop, invalid };

struct Command {
    CommandType type = CommandType::invalid;
    std::uint64_t id = 0;
    std::string json;
};

Command parse_command(std::string line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line == "Q2DZ/HUB/2 STOP") return {CommandType::stop, 0, {}};
    if (line.starts_with("Q2DZ/HUB/2 ICE generation=")) {
        const auto first = std::string("Q2DZ/HUB/2 ICE generation=").size();
        const auto end = line.find(' ', first);
        std::uint64_t id = 0;
        if (end == std::string::npos || !parse_id(std::string_view(line).substr(first, end - first), id) ||
            line.compare(end, 6, " json=") != 0) return {};
        return {CommandType::ice, id, {}};
    }
    if (line.starts_with("Q2DZ/HUB/2 PUT id=")) {
        const auto first = std::string("Q2DZ/HUB/2 PUT id=").size();
        const auto end = line.find(' ', first);
        std::uint64_t id = 0;
        if (end == std::string::npos || !parse_id(std::string_view(line).substr(first, end - first), id) ||
            line.compare(end, 6, " json=") != 0) return {};
        return {CommandType::put, id, line.substr(end + 6U)};
    }
    if (line.starts_with("Q2DZ/HUB/2 DROP id=")) {
        const auto first = std::string("Q2DZ/HUB/2 DROP id=").size();
        std::uint64_t id = 0;
        if (!parse_id(std::string_view(line).substr(first), id)) return {};
        return {CommandType::drop, id, {}};
    }
    return {};
}

void emit(std::string_view line) {
    std::cout << line << '\n' << std::flush;
}

void emit_ready(std::uint64_t id) {
    emit("Q2DZ/HUB/2 READY id=" + std::to_string(id) + " udp=127.0.0.1:" +
         std::to_string(27910U + static_cast<unsigned>(id % 30000U)) +
         " peer=" + std::to_string(100U + (id % 100000U)));
}

void emit_failed(std::uint64_t id, std::string_view code) {
    emit("Q2DZ/HUB/2 FAILED id=" + std::to_string(id) + " stage=transport code=" + std::string(code));
}

void emit_closed(std::uint64_t id, std::string_view reason) {
    emit("Q2DZ/HUB/2 CLOSED id=" + std::to_string(id) + " reason=" + std::string(reason));
}

} // namespace

int wmain() {
    const std::filesystem::path marker_directory = env(L"Q2DZ_PID_DIR");
    write_pid(marker_directory);
    write_marker(marker_directory, L"gateway-started");

    std::map<std::uint64_t, std::string> targets;
    std::map<std::string, std::uint64_t> dropped;
    std::vector<std::uint64_t> shuffled_ready;
    std::uint64_t ice_count = 0;
    std::uint64_t put_count = 0;
    std::uint64_t drop_count = 0;
    std::string pending;
    const auto ready = [&](std::uint64_t id) {
        emit_ready(id);
        write_marker(marker_directory, L"gateway-last-ready-id", std::to_string(id));
    };
    char buffer[4096];
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer), &count, nullptr) || count == 0) break;
        pending.append(buffer, static_cast<std::size_t>(count));
        for (;;) {
            const auto newline = pending.find('\n');
            if (newline == std::string::npos) break;
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1U);
            Command command = parse_command(std::move(line));
            if (command.type == CommandType::stop) {
                wipe(command.json);
                wipe(pending);
                return 0;
            }
            if (command.type == CommandType::invalid) {
                wipe(command.json);
                wipe(pending);
                return 30;
            }
            if (command.type == CommandType::ice) {
                ++ice_count;
                write_marker(marker_directory, L"gateway-ice-count", std::to_string(ice_count));
                wipe(command.json);
                continue;
            }
            if (command.type == CommandType::drop) {
                const auto target = targets.find(command.id);
                if (target != targets.end()) {
                    dropped[target->second] = target->first;
                    targets.erase(target);
                    ++drop_count;
                    write_marker(marker_directory, L"gateway-drop-count", std::to_string(drop_count));
                }
                wipe(command.json);
                continue;
            }
            const auto id = command.id;
            const auto room = room_id(command.json);
            if (room.empty()) {
                wipe(command.json);
                wipe(pending);
                return 31;
            }
            ++put_count;
            write_marker(marker_directory, L"gateway-put-count", std::to_string(put_count));
            write_marker(marker_directory, L"gateway-last-put-id", std::to_string(command.id));
            const auto mode = mode_for(room);
            targets[id] = room;
            if (mode == L"never-ready") {
                wipe(command.json);
                continue;
            }
            if (mode == L"fatal-immediate") {
                emit_failed(id, "immediate_test");
                targets.erase(id);
                wipe(command.json);
                continue;
            }
            if (mode == L"room-close-immediate") {
                emit_closed(id, "immediate_test");
                targets.erase(id);
                wipe(command.json);
                continue;
            }
            if (mode == L"shuffled-ready") {
                shuffled_ready.push_back(id);
                if (shuffled_ready.size() >= 2U) {
                    for (auto it = shuffled_ready.rbegin(); it != shuffled_ready.rend(); ++it) ready(*it);
                    shuffled_ready.clear();
                }
                wipe(command.json);
                continue;
            }
            ready(id);
            if (mode == L"failed-after-ready") {
                emit_failed(id, "post_ready_test");
                targets.erase(id);
                wipe(command.json);
                continue;
            }
            if (mode == L"room-close-after-ready") {
                Sleep(100);
                emit_closed(id, "test");
                targets.erase(id);
            } else if (mode == L"exit-after-ready") {
                Sleep(100);
                wipe(command.json);
                wipe(pending);
                return 21;
            } else if (mode == L"stale-after-drop") {
                const auto old = dropped.find(room);
                if (old != dropped.end()) {
                    emit_failed(old->second, "stale");
                    ready(old->second);
                    dropped.erase(old);
                }
            }
            wipe(command.json);
        }
    }
    wipe(pending);
    return 0;
}

#endif
