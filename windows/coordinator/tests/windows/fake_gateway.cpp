#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <atomic>
#include <cstdlib>
#include <cwctype>
#include <iostream>
#include <string>
#include <thread>
#pragma comment(lib,"ws2_32.lib")

std::wstring env(const wchar_t* name){const wchar_t* value=_wgetenv(name);return value?value:L"";}
std::wstring room_id(const std::string& cfg){const std::string key="\"roomId\":\"";const auto start=cfg.find(key);if(start==std::string::npos)return L"";const auto first=start+key.size();const auto last=cfg.find('"',first);if(last==std::string::npos)return L"";std::wstring result;for(std::size_t i=first;i<last;++i)result.push_back(static_cast<unsigned char>(cfg[i]));return result;}
std::wstring safe_name(std::wstring value){for(auto& c:value)if(!(std::iswalnum(c)||c==L'-'||c==L'_'))c=L'_';return value.empty()?L"unknown":value;}
std::wstring mode_for(const std::wstring& id){if(!id.empty()){const std::wstring suffix=id==L"room-a"?L"_A":id==L"room-b"?L"_B":L"";if(!suffix.empty()){const auto selected=env((std::wstring(L"Q2DZ_FAKE_GATEWAY_MODE")+suffix).c_str());if(!selected.empty())return selected;}}const auto selected=env(L"Q2DZ_FAKE_GATEWAY_MODE");return selected.empty()?L"normal":selected;}

int wmain(int argc,wchar_t** argv){HANDLE owner=nullptr;for(int i=1;i+1<argc;++i)if(std::wstring(argv[i])==L"--owner-handle")owner=reinterpret_cast<HANDLE>(_wcstoui64(argv[++i],nullptr,10));std::string cfg;char b[4096];for(;;){DWORD n=0;if(!ReadFile(GetStdHandle(STD_INPUT_HANDLE),b,sizeof(b),&n,nullptr)||!n)break;cfg.append(b,n);}if(cfg.empty())return 20;const auto id=room_id(cfg);if(const wchar_t* p=_wgetenv(L"Q2DZ_PID_DIR")){const auto name=id.empty()?L"gateway.pid":L"gateway-"+safe_name(id)+L".pid";std::wstring f=std::wstring(p)+L"\\"+name;HANDLE h=CreateFileW(f.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,0,nullptr);if(h!=INVALID_HANDLE_VALUE){auto s=std::to_string(GetCurrentProcessId());DWORD n;WriteFile(h,s.data(),static_cast<DWORD>(s.size()),&n,nullptr);CloseHandle(h);}}
const std::wstring mode=mode_for(id);if(mode==L"never-ready"){Sleep(600000);return 0;}if(mode==L"malformed-ready"){std::cout<<"Q2DZ/1 TRANSPORT_READY udp=0.0.0.0:27910\n"<<std::flush;Sleep(600000);return 0;}if(mode==L"no-newline-flood"){std::cout<<std::string(100*1024,'x')<<std::flush;return 0;}if(mode==L"line-flood"){const std::string line="Q2DZ/1 HEARTBEAT\n";std::string flood;for(int i=0;i<64;++i)flood+=line;DWORD written=0;if(!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),flood.data(),static_cast<DWORD>(flood.size()),&written,nullptr))return 21;return written==static_cast<DWORD>(flood.size())?0:21;}WSADATA wd{};WSAStartup(MAKEWORD(2,2),&wd);SOCKET s=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=0;bind(s,reinterpret_cast<sockaddr*>(&a),sizeof(a));int len=sizeof(a);getsockname(s,reinterpret_cast<sockaddr*>(&a),&len);std::cout<<"Q2DZ/1 TRANSPORT_READY udp=127.0.0.1:"<<ntohs(a.sin_port)<<"\n"<<std::flush;if(mode==L"delete-q2-after-ready"){const wchar_t* path=_wgetenv(L"Q2DZ_TEST_DELETE_Q2_AFTER_READY");if(!path||!DeleteFileW(path))return 22;}if(mode==L"exit-after-ready"){Sleep(300);return 21;}if(mode==L"room-close"||mode==L"room-close-after-ready"){Sleep(300);std::cout<<"Q2DZ/1 ROOM_CLOSED reason=test\n"<<std::flush;Sleep(300);return 0;}if(mode==L"room-close-immediate"){std::cout<<"Q2DZ/1 ROOM_CLOSED reason=immediate_test\n"<<std::flush;return 0;}if(mode==L"fatal-immediate"){std::cout<<"Q2DZ/1 FATAL stage=transport code=immediate_test\n"<<std::flush;return 0;}char x;DWORD n=0;if(owner)ReadFile(owner,&x,1,&n,nullptr);closesocket(s);WSACleanup();return 0;}
#endif
