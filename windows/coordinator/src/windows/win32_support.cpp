#ifdef _WIN32
#include "win32_support.hpp"
#include <shlobj.h>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace q2dz::win {
namespace {
std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    if(size<=0)return "Windows error";
    std::string out(static_cast<std::size_t>(size),'\0');
    if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),out.data(),size,nullptr,nullptr)!=size)return "Windows error";
    return out;
}
std::wstring environment(const wchar_t* name) {
    std::size_t size=0;
    if(_wgetenv_s(&size,nullptr,0,name)!=0||size==0)return {};
    std::wstring value(size,L'\0');
    if(_wgetenv_s(&size,value.data(),value.size(),name)!=0)return {};
    value.resize(size-1);
    return value;
}
[[noreturn]] void fail(const std::wstring& what) { throw std::runtime_error(utf8(what)); }
bool regular_exe(const std::filesystem::path& p) {
    std::error_code ec; return std::filesystem::is_regular_file(p,ec) && (p.extension()==L".exe" || p.extension()==L".EXE");
}
std::filesystem::path full(const std::filesystem::path& p) {
    std::error_code ec; auto x=std::filesystem::absolute(p,ec); return ec?p:x;
}
std::wstring guid_text() {
    GUID g{}; if(CoCreateGuid(&g)!=S_OK) fail(L"CoCreateGuid failed");
    wchar_t b[64]{}; if(!StringFromGUID2(g,b,64)) fail(L"StringFromGUID2 failed");
    std::wstring s(b); for(auto& c:s) if(c==L'{'||c==L'}') c=L'-'; return s;
}
}

std::wstring last_error_message(DWORD code) {
    wchar_t* p=nullptr;
    const DWORD n=FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,nullptr,code,0,reinterpret_cast<wchar_t*>(&p),0,nullptr);
    std::wstring s=n?std::wstring(p,n):L"Windows error "+std::to_wstring(code);
    if(p) LocalFree(p); while(!s.empty()&&(s.back()==L'\r'||s.back()==L'\n')) s.pop_back(); return s;
}

std::wstring quote_arg(std::wstring_view a) {
    if(a.empty()) return L"\"\"";
    if(a.find_first_of(L" \t\n\v\"")==std::wstring_view::npos) return std::wstring(a);
    std::wstring out=L"\""; std::size_t slashes=0;
    for(wchar_t c:a) {
        if(c==L'\\') { ++slashes; continue; }
        if(c==L'\"') { out.append(slashes*2+1,L'\\'); out.push_back(L'\"'); slashes=0; continue; }
        out.append(slashes,L'\\'); slashes=0; out.push_back(c);
    }
    out.append(slashes*2,L'\\'); out.push_back(L'\"'); return out;
}
std::wstring command_line(const std::filesystem::path& exe,const std::vector<std::wstring>& args) {
    std::wstring out=quote_arg(exe.wstring()); for(const auto& a:args){out.push_back(L' '); out+=quote_arg(a);} return out;
}

Pipe make_pipe(bool ri,bool wi) {
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE}; HANDLE r=nullptr,w=nullptr;
    if(!CreatePipe(&r,&w,&sa,0)) fail(L"CreatePipe: "+last_error_message());
    Pipe p{Handle(r),Handle(w)};
    if(!ri && !SetHandleInformation(p.read.get(),HANDLE_FLAG_INHERIT,0)) fail(L"SetHandleInformation(read)");
    if(!wi && !SetHandleInformation(p.write.get(),HANDLE_FLAG_INHERIT,0)) fail(L"SetHandleInformation(write)");
    return p;
}
Handle open_null(bool write) {
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    HANDLE h=CreateFileW(L"NUL",write?GENERIC_WRITE:GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE) fail(L"open NUL: "+last_error_message()); return Handle(h);
}

ChildProcess launch_suspended(const std::filesystem::path& exe,const std::vector<std::wstring>& args,const std::filesystem::path& cwd,HANDLE in,HANDLE out,HANDLE err,const std::vector<HANDLE>& inherited,DWORD flags) {
    SIZE_T bytes=0; InitializeProcThreadAttributeList(nullptr,1,0,&bytes);
    std::vector<unsigned char> storage(bytes);
    auto* attrs=reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if(!InitializeProcThreadAttributeList(attrs,1,0,&bytes)) fail(L"InitializeProcThreadAttributeList: "+last_error_message());
    struct Guard{PPROC_THREAD_ATTRIBUTE_LIST p;~Guard(){DeleteProcThreadAttributeList(p);}} guard{attrs};
    if(!inherited.empty() && !UpdateProcThreadAttribute(attrs,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,const_cast<HANDLE*>(inherited.data()),inherited.size()*sizeof(HANDLE),nullptr,nullptr)) fail(L"UpdateProcThreadAttribute: "+last_error_message());
    STARTUPINFOEXW sx{}; sx.StartupInfo.cb=sizeof(sx); sx.lpAttributeList=attrs; sx.StartupInfo.dwFlags=STARTF_USESTDHANDLES; sx.StartupInfo.hStdInput=in; sx.StartupInfo.hStdOutput=out; sx.StartupInfo.hStdError=err;
    PROCESS_INFORMATION pi{}; auto cmd=command_line(exe,args); std::vector<wchar_t> mutable_cmd(cmd.begin(),cmd.end()); mutable_cmd.push_back(0);
    const DWORD create=EXTENDED_STARTUPINFO_PRESENT|CREATE_SUSPENDED|CREATE_UNICODE_ENVIRONMENT|flags;
    if(!CreateProcessW(exe.c_str(),mutable_cmd.data(),nullptr,nullptr,TRUE,create,nullptr,cwd.empty()?nullptr:cwd.c_str(),&sx.StartupInfo,&pi)) fail(L"CreateProcessW("+exe.wstring()+L"): "+last_error_message());
    return {Handle(pi.hProcess),Handle(pi.hThread),pi.dwProcessId};
}

std::filesystem::path default_session_root() {
    PWSTR p=nullptr; if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_CREATE,nullptr,&p))) fail(L"SHGetKnownFolderPath failed");
    std::filesystem::path r(p); CoTaskMemFree(p); return r/L"Q2PRO-X Dos.Zone"/L"sessions";
}
std::optional<std::filesystem::path> locate_q2(const std::optional<std::filesystem::path>& explicit_path,const std::filesystem::path& coordinator) {
    std::vector<std::filesystem::path> c;
    if(explicit_path) c.push_back(*explicit_path);
    if(auto e=environment(L"Q2PROX_EXE");!e.empty()) c.emplace_back(std::move(e));
    auto add=[&](const std::filesystem::path& d){c.push_back(d/L"Q2PRO-X.exe"); c.push_back(d/L"q2pro.exe");};
    add(std::filesystem::current_path()); auto d=coordinator.parent_path(); for(int i=0;i<4 && !d.empty();++i){add(d);d=d.parent_path();}
    for(auto& p:c) if(regular_exe(p)) return full(p); return std::nullopt;
}
std::vector<unsigned char> read_all_stdin(std::size_t max) {
    std::vector<unsigned char> out; std::array<unsigned char,8192> b{}; HANDLE h=GetStdHandle(STD_INPUT_HANDLE);
    for(;;){DWORD n=0;if(!ReadFile(h,b.data(),static_cast<DWORD>(b.size()),&n,nullptr)){if(GetLastError()==ERROR_BROKEN_PIPE)break;fail(L"read configuration stdin: "+last_error_message());} if(!n)break; if(out.size()+n>max) fail(L"configuration exceeds size limit"); out.insert(out.end(),b.begin(),b.begin()+n);} return out;
}
void write_all(HANDLE h,const unsigned char* p,std::size_t n){while(n){DWORD w=0;const DWORD ask=static_cast<DWORD>((std::min<std::size_t>)(n,1u<<20));if(!WriteFile(h,p,ask,&w,nullptr))fail(L"write child stdin: "+last_error_message());p+=w;n-=w;}}
void secure_clear(std::vector<unsigned char>& d) noexcept { if(!d.empty()) SecureZeroMemory(d.data(),d.size()); d.clear(); d.shrink_to_fit(); }

std::filesystem::path create_session_directory(const std::filesystem::path& root,Handle& lock) {
    std::filesystem::create_directories(root); for(int i=0;i<20;++i){auto p=root/(L"session"+guid_text());std::error_code ec;if(!std::filesystem::create_directory(p,ec))continue;std::ofstream(p/L"session.v1",std::ios::binary)<<"q2dz-session-v1\n";HANDLE h=CreateFileW((p/L"owner.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_HIDDEN,nullptr);if(h==INVALID_HANDLE_VALUE){std::filesystem::remove_all(p);continue;}lock.reset(h);return p;}fail(L"cannot create unique session directory");}
bool remove_tree_with_retry(const std::filesystem::path& p,DWORD total){const auto start=GetTickCount64();DWORD delay=50;do{std::error_code ec;std::filesystem::remove_all(p,ec);if(!std::filesystem::exists(p,ec))return true;Sleep(delay);delay=(std::min<DWORD>)(delay*2,500);}while(GetTickCount64()-start<total);return false;}
void cleanup_stale_sessions(const std::filesystem::path& root){std::error_code ec;if(!std::filesystem::exists(root,ec))return;for(const auto& e:std::filesystem::directory_iterator(root,ec)){if(ec||!e.is_directory())continue;if(!std::filesystem::exists(e.path()/L"session.v1"))continue;HANDLE h=CreateFileW((e.path()/L"owner.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(h==INVALID_HANDLE_VALUE)continue;CloseHandle(h);(void)remove_tree_with_retry(e.path(),2000);}}
} // namespace q2dz::win
#endif
