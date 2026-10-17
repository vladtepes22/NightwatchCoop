#include "seamless_launcher.h"
#include "party/party_status.h"
#include "net/json.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <thread>
#include <windows.h>

static int quit_requests;
extern "C" void bbgpu_request_quit() { ++quit_requests; }
int main() {
    namespace fs=std::filesystem;
    using namespace party::status;
    char temp[MAX_PATH]; assert(GetTempPathA(sizeof(temp),temp));
    const fs::path root=fs::path(temp)/("bb-party-launcher-"+std::to_string(GetCurrentProcessId()));
    fs::create_directories(root);
    _putenv_s("BB_PARTY","host");
    _putenv_s("BB_GPU_USER_DIR",root.string().c_str());
    SetRole(Role::Host);
    SetState(State::Hosting,"Waiting for guests");
    SetMembers({{"Host",true,true,0,0,"Dream",true},{"Guest",true,false,30,1,"Cathedral",false}});
    SeamlessLauncher::Poll();
    std::ifstream status(root/"party_status.json");
    const std::string text{std::istreambuf_iterator<char>(status),{}};
    json::Value value; std::string error;
    assert(json::parse(text,value,error));
    assert(json::str(value,"role")=="host");
    const auto& members=json::arr(value,"members");
    assert(members.size()==2 && json::str(members[1],"name")=="Guest");
    assert(json::num(members[1],"ping_ms")==30 && !json::flag(members[1],"in_world"));
    std::ofstream(root/"party_command.json") << "{\"action\":\"kick\",\"name\":\"Guest\"}";
    std::this_thread::sleep_for(std::chrono::milliseconds(550));
    SeamlessLauncher::Poll();
    Command command;
    assert(PopCommand(&command) && command.type==CommandType::Kick && command.name=="Guest");
    assert(!fs::exists(root/"party_command.json"));
    std::ofstream(root/"party_command.json") << "{\"action\":\"add_guest_code\",\"name\":\"BBG1-test\"}";
    std::this_thread::sleep_for(std::chrono::milliseconds(550));
    SeamlessLauncher::Poll();
    assert(PopCommand(&command) && command.type==CommandType::AddGuestCode && command.name=="BBG1-test");
    std::ofstream(root/"party_command.json") << "{\"action\":\"quit\"}";
    std::this_thread::sleep_for(std::chrono::milliseconds(550));
    SeamlessLauncher::Poll();
    assert(quit_requests==1);
    status.close(); fs::remove_all(root);
    puts("PASS: live roster serialization, member removal and graceful quit command");
}
