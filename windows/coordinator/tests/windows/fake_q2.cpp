#ifdef _WIN32
#include <windows.h>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
namespace {
std::wstring env(const wchar_t* name){const wchar_t* value=_wgetenv(name);return value?value:L"";}
bool valid_endpoint_list(const std::wstring& value){std::size_t count=0,start=0;while(start<value.size()){while(start<value.size()&&value[start]==L' ')++start;if(start==value.size())break;const auto end=value.find(L' ',start);const auto token=value.substr(start,end==std::wstring::npos?std::wstring::npos:end-start);if(!token.starts_with(L"quake2://127.0.0.1:"))return false;const auto port_text=token.substr(std::wstring(L"quake2://127.0.0.1:").size());if(port_text.empty()||port_text.size()>5)return false;wchar_t* end_ptr=nullptr;errno=0;const auto port=wcstoul(port_text.c_str(),&end_ptr,10);if(errno||!end_ptr||*end_ptr||port<1024||port>65535)return false;if(++count>2)return false;if(end==std::wstring::npos)break;start=end+1;}return count>0;}
}
int wmain(int argc,wchar_t** argv){
    bool game=false,custom=false,connect=false,default_source=false,modern=false;
    std::wstring source;
    for(int i=1;i<argc;++i){
        if(std::wstring(argv[i])==L"+connect")connect=true;
        if(std::wstring(argv[i])==L"+serverbrowser_open_modern")modern=true;
        if(std::wstring(argv[i])==L"+set"&&i+2<argc){
            const std::wstring key=argv[i+1],value=argv[i+2];
            if(key==L"game"&&value==L"3zb2")game=true;
            if(key==L"cl_serverbrowser_custom_source"){custom=true;source=value;}
            if(key==L"cl_serverbrowser_default_source")default_source=true;
        }
    }
    if(!game||!custom||!modern||connect||default_source||!valid_endpoint_list(source))return 30;
    const std::wstring pid_dir=env(L"Q2DZ_PID_DIR");
    if(!pid_dir.empty())std::ofstream(std::filesystem::path(pid_dir+L"\\q2.pid"))<<GetCurrentProcessId();
    const std::wstring mode=env(L"Q2DZ_FAKE_Q2_MODE").empty()?L"exit-normal":env(L"Q2DZ_FAKE_Q2_MODE");
    if(mode==L"hub-room-close"||mode==L"hub-room-crash"){
        Sleep(550);
        const std::wstring marker=pid_dir+L"\\gateway-room-b.pid";
        std::ifstream pid_stream{std::filesystem::path(marker)};unsigned long pid_value=0;pid_stream>>pid_value;
        bool alive=false;
        if(pid_value){HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid_value);if(process){DWORD code=0;alive=GetExitCodeProcess(process,&code)&&code==STILL_ACTIVE;CloseHandle(process);}}
        if(alive)std::ofstream(std::filesystem::path(pid_dir+L"\\hub-q2-observed-b-alive"))<<"yes";
        Sleep(250);return 0;
    }
    if(mode==L"exit-normal"){Sleep(400);return 0;}if(mode==L"crash"){Sleep(300);TerminateProcess(GetCurrentProcess(),31);}Sleep(600000);return 0;
}
#endif
