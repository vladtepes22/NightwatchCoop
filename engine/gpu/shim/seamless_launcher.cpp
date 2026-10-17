// SPDX-License-Identifier: GPL-3.0-or-later
#include "seamless_launcher.h"
#include "party/party_status.h"
#include "net/json.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <windows.h>
extern "C" void bbgpu_request_quit(void);

namespace SeamlessLauncher {
void Poll() {
    using namespace party::status;
    static auto next = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now < next || !Enabled()) return;
    next = now + std::chrono::milliseconds(500);
    const char* user = std::getenv("BB_GPU_USER_DIR");
    if (!user || !*user) return;
    const std::filesystem::path root(user);
    const auto command = root / "party_command.json";
    std::error_code error;
    if (std::filesystem::exists(command, error) && std::filesystem::file_size(command, error) < 4096) {
        std::ifstream input(command, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(input), {}};
        input.close();
        std::filesystem::remove(command, error);
        json::Value value;
        std::string parse_error;
        if (json::parse(text, value, parse_error)) {
            const auto* action = value.find("action");
            const auto* name = value.find("name");
            if (action && action->type == json::Value::Type::String) {
                if (action->string == "leave") RequestLeave();
                else if (action->string == "rejoin") RequestRejoin();
                else if (action->string == "quit") bbgpu_request_quit();
                else if (action->string == "add_guest_code" && name && name->type == json::Value::Type::String)
                    RequestAddGuestCode(name->string);
                else if (action->string == "kick" && name && name->type == json::Value::Type::String)
                    KickMember(name->string);
            }
        }
    }
    const Board board = Snapshot();
    auto value = json::Value::make_object();
    value.set("role", RoleName(board.role));
    value.set("state", StateName(board.state));
    value.set("detail", board.detail);
    value.set("network", board.network);
    value.set("event", board.last_event);
    value.set("alert", board.alert);
    value.set("guest_code", board.guest_code);
    auto members = json::Value::make_array();
    for (const auto& member : board.members) {
        auto item = json::Value::make_object();
        item.set("name", member.name);
        item.set("connected", member.connected);
        item.set("in_world", member.in_world);
        item.set("local", member.local);
        item.set("ping_ms", member.ping_ms);
        item.set("area", member.area);
        members.push(std::move(item));
    }
    value.set("members", std::move(members));
    const auto temp = root / "party_status.json.tmp";
    const auto path = root / "party_status.json";
    std::ofstream output(temp, std::ios::binary | std::ios::trunc);
    output << json::dump(value);
    output.close();
    if (output) MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
}
