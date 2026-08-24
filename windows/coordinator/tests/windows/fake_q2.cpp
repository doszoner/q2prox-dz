#ifdef _WIN32
#include <windows.h>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <string>
int wmain(int argc,wchar_t** argv){
    if(argc<6||std::wstring(argv[argc-5])!=L"+set"||std::wstring(argv[argc-4])!=L"game"||std::wstring(argv[argc-3])!=L"3zb2"||std::wstring(argv[argc-2])!=L"+connect")return 30;
    for(int i=1;i<argc-5;++i)if(std::wstring(argv[i])==L"+connect")return 30;
    const std::wstring endpoint=argv[argc-1],prefix=L"127.0.0.1:";
    if(!endpoint.starts_with(prefix))return 30;
    wchar_t* end=nullptr;errno=0;const unsigned long port=wcstoul(endpoint.c_str()+prefix.size(),&end,10);
    if(errno||!end||*end||port<1024||port>65535)return 30;
    if(const wchar_t* p=_wgetenv(L"Q2DZ_PID_DIR")){std::wofstream(std::wstring(p)+L"\\q2.pid")<<GetCurrentProcessId();}
    const std::wstring mode=_wgetenv(L"Q2DZ_FAKE_Q2_MODE")?_wgetenv(L"Q2DZ_FAKE_Q2_MODE"):L"exit-normal";
    if(mode==L"exit-normal"){Sleep(400);return 0;}if(mode==L"crash"){Sleep(300);TerminateProcess(GetCurrentProcess(),31);}Sleep(600000);return 0;
}
#endif
