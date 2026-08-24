#ifdef _WIN32
#include "win32_support.hpp"
#include "q2dz/child_protocol.hpp"
#include "q2dz/session_state.hpp"
#include <windows.h>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs=std::filesystem;
namespace {
using q2dz::win::Handle;
Handle g_stop_event;
BOOL WINAPI ctrl_handler(DWORD t){if(t==CTRL_C_EVENT||t==CTRL_BREAK_EVENT||t==CTRL_CLOSE_EVENT||t==CTRL_LOGOFF_EVENT||t==CTRL_SHUTDOWN_EVENT){if(g_stop_event)SetEvent(g_stop_event.get());return TRUE;}return FALSE;}

struct Options{fs::path gateway;std::optional<fs::path> q2;std::optional<fs::path> root;DWORD ready_ms=60000;DWORD graceful_ms=2500;};
[[noreturn]] void usage(){throw std::runtime_error("usage: q2dz-session --gateway <exe> [--q2 <Q2PRO-X.exe>] [--session-root <dir>] [--ready-timeout-ms N] [--graceful-timeout-ms N]; room JSON must be supplied on stdin");}
DWORD parse_ms(const wchar_t* s){wchar_t* e=nullptr;unsigned long v=wcstoul(s,&e,10);if(!s[0]||*e||v<100||v>600000)usage();return static_cast<DWORD>(v);}
Options parse(int argc,wchar_t** argv){Options o;for(int i=1;i<argc;++i){std::wstring a=argv[i];auto val=[&]()->const wchar_t*{if(++i>=argc)usage();return argv[i];};if(a==L"--gateway")o.gateway=val();else if(a==L"--q2")o.q2=fs::path(val());else if(a==L"--session-root")o.root=fs::path(val());else if(a==L"--ready-timeout-ms")o.ready_ms=parse_ms(val());else if(a==L"--graceful-timeout-ms")o.graceful_ms=parse_ms(val());else usage();}if(o.gateway.empty())usage();return o;}

struct Queue{std::mutex m;q2dz::ChildEventFramer framer;Handle event{CreateEventW(nullptr,TRUE,FALSE,nullptr)};bool feed(std::string_view bytes){std::lock_guard l(m);const bool keep=framer.feed(bytes);SetEvent(event.get());return keep;}void finish(){std::lock_guard l(m);(void)framer.finish();SetEvent(event.get());}std::deque<std::string> drain(){std::lock_guard l(m);auto x=framer.drain();ResetEvent(event.get());return x;}};
void reader(HANDLE h,Queue& q){char b[1024];for(;;){DWORD n=0;if(!ReadFile(h,b,sizeof(b),&n,nullptr)||!n)break;if(!q.feed(std::string_view(b,n)))break;}q.finish();}

void assign_job(HANDLE job,HANDLE process){if(!AssignProcessToJobObject(job,process))throw std::runtime_error("AssignProcessToJobObject failed");}
void resume(HANDLE thread){if(ResumeThread(thread)==static_cast<DWORD>(-1))throw std::runtime_error("ResumeThread failed");}
bool json_shape(const std::vector<unsigned char>& v){auto first=std::find_if(v.begin(),v.end(),[](unsigned char c){return c!=' '&&c!='\t'&&c!='\r'&&c!='\n';});auto last=std::find_if(v.rbegin(),v.rend(),[](unsigned char c){return c!=' '&&c!='\t'&&c!='\r'&&c!='\n';});return first!=v.end()&&last!=v.rend()&&*first=='{'&&*last=='}';}
void log_state(const q2dz::SessionStateMachine& m){const auto state=q2dz::to_string(m.state());const auto cause=q2dz::to_string(m.cause());std::wcout<<L"q2dz-session: state="<<std::wstring(state.begin(),state.end())<<L" cause="<<std::wstring(cause.begin(),cause.end())<<L"\n";}
}

int wmain(int argc,wchar_t** argv){
    DWORD graceful_ms=2500;
    Handle job;
    q2dz::win::Pipe owner_pipe;
    Handle event_read;
    q2dz::win::ChildProcess gateway;
    std::optional<q2dz::win::ChildProcess> q2p;
    std::optional<q2dz::win::ChildProcess> pending_q2;
    std::optional<std::jthread> reader_thread;
    Queue q;
    q2dz::SessionStateMachine sm;
    fs::path session;
    Handle session_lock;
    bool session_created=false;
    bool gateway_in_job=false;
    bool ctrl_handler_set=false;

    const auto cleanup=[&]() noexcept {
        owner_pipe.write.reset();
        if(pending_q2 && pending_q2->process){TerminateProcess(pending_q2->process.get(),3);WaitForSingleObject(pending_q2->process.get(),5000);}
        if(gateway.process && !gateway_in_job){TerminateProcess(gateway.process.get(),3);WaitForSingleObject(gateway.process.get(),5000);}
        if(job && gateway.process && WaitForSingleObject(gateway.process.get(),graceful_ms)==WAIT_TIMEOUT)TerminateJobObject(job.get(),3);
        owner_pipe.read.reset();
        if(reader_thread){(void)reader_thread->request_stop();if(reader_thread->joinable()){(void)CancelSynchronousIo(reader_thread->native_handle());reader_thread->join();}reader_thread.reset();}
        event_read.reset();
        if(q2p && q2p->process)WaitForSingleObject(q2p->process.get(),5000);
        if(gateway.process)WaitForSingleObject(gateway.process.get(),5000);
        if(job)job.reset();
        pending_q2.reset();q2p.reset();gateway.thread.reset();gateway.process.reset();
        if(ctrl_handler_set){SetConsoleCtrlHandler(ctrl_handler,FALSE);ctrl_handler_set=false;}
        session_lock.reset();
        if(session_created){bool clean=false;try{clean=q2dz::win::remove_tree_with_retry(session,10000);}catch(...){clean=false;}(void)sm.apply(clean?q2dz::SessionEvent::cleanup_complete:q2dz::SessionEvent::cleanup_failed);session_created=false;}
    };

    try{
        const auto o=parse(argc,argv);graceful_ms=o.graceful_ms;
        const fs::path self=[](){std::wstring b(32768,L'\0');DWORD n=GetModuleFileNameW(nullptr,b.data(),static_cast<DWORD>(b.size()));b.resize(n);return fs::path(b);}();
        auto q2=q2dz::win::locate_q2(o.q2,self);if(!q2)throw std::runtime_error("Q2PRO-X.exe not found; pass --q2 or set Q2PROX_EXE");
        if(!fs::is_regular_file(o.gateway))throw std::runtime_error("gateway executable not found");
        auto config=q2dz::win::read_all_stdin(256*1024);if(!json_shape(config)){q2dz::win::secure_clear(config);throw std::runtime_error("stdin must contain one UTF-8 JSON room configuration object");}

        g_stop_event.reset(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!g_stop_event)throw std::runtime_error("CreateEvent failed");SetConsoleCtrlHandler(ctrl_handler,TRUE);ctrl_handler_set=true;
        job.reset(CreateJobObjectW(nullptr,nullptr));if(!job)throw std::runtime_error("CreateJobObject failed");JOBOBJECT_EXTENDED_LIMIT_INFORMATION ji{};ji.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;if(!SetInformationJobObject(job.get(),JobObjectExtendedLimitInformation,&ji,sizeof(ji)))throw std::runtime_error("SetInformationJobObject failed");

        const fs::path root=o.root.value_or(q2dz::win::default_session_root());q2dz::win::cleanup_stale_sessions(root);session=q2dz::win::create_session_directory(root,session_lock);session_created=true;const fs::path webview=session/L"webview2";fs::create_directories(webview);
        (void)sm.apply(q2dz::SessionEvent::begin);log_state(sm);

        auto config_pipe=q2dz::win::make_pipe(true,false);auto event_pipe=q2dz::win::make_pipe(false,true);owner_pipe=q2dz::win::make_pipe(true,false);auto nullerr=q2dz::win::open_null(true);
        std::vector<std::wstring> gargs={L"--managed-session",L"--udp-port",L"0",L"--owner-handle",std::to_wstring(reinterpret_cast<std::uintptr_t>(owner_pipe.read.get())),L"--user-data-dir",webview.wstring()};
        std::vector<HANDLE> inherit={config_pipe.read.get(),event_pipe.write.get(),owner_pipe.read.get(),nullerr.get()};
        gateway=q2dz::win::launch_suspended(o.gateway,gargs,o.gateway.parent_path(),config_pipe.read.get(),event_pipe.write.get(),nullerr.get(),inherit,CREATE_NO_WINDOW|CREATE_NEW_PROCESS_GROUP);
        assign_job(job.get(),gateway.process.get());gateway_in_job=true;config_pipe.read.reset();event_pipe.write.reset();owner_pipe.read.reset();resume(gateway.thread.get());gateway.thread.reset();(void)sm.apply(q2dz::SessionEvent::gateway_started);log_state(sm);
        q2dz::win::write_all(config_pipe.write.get(),config.data(),config.size());config_pipe.write.reset();q2dz::win::secure_clear(config);

        event_read=std::move(event_pipe.read);reader_thread.emplace([h=event_read.get(),&q](std::stop_token){reader(h,q);});
        std::optional<q2dz::LoopbackEndpoint> endpoint;const ULONGLONG deadline=GetTickCount64()+o.ready_ms;bool done=false;
        auto consume_events=[&](std::deque<std::string> lines){
            for(auto& line:lines){
                auto parsed=q2dz::parse_child_event(line);if(!parsed.event){(void)sm.apply(q2dz::SessionEvent::fatal_error);done=true;return true;}
                const auto& e=*parsed.event;
                if(e.type==q2dz::ChildEventType::transport_ready){
                    if(sm.state()!=q2dz::SessionState::waiting_transport){(void)sm.apply(q2dz::SessionEvent::fatal_error);done=true;return true;}
                    endpoint=q2dz::parse_loopback_endpoint(e.fields.at("udp"));(void)sm.apply(q2dz::SessionEvent::transport_ready);log_state(sm);
                    const auto endpoint_text=endpoint->text();std::vector<std::wstring> qargs={L"+set",L"game",L"3zb2",L"+connect",std::wstring(endpoint_text.begin(),endpoint_text.end())};pending_q2.emplace(q2dz::win::launch_suspended(*q2,qargs,q2->parent_path(),GetStdHandle(STD_INPUT_HANDLE),GetStdHandle(STD_OUTPUT_HANDLE),GetStdHandle(STD_ERROR_HANDLE),{},0));assign_job(job.get(),pending_q2->process.get());resume(pending_q2->thread.get());pending_q2->thread.reset();q2p.emplace(std::move(*pending_q2));pending_q2.reset();(void)sm.apply(q2dz::SessionEvent::q2_started);log_state(sm);
                }else if(e.type==q2dz::ChildEventType::room_closed){(void)sm.apply(q2dz::SessionEvent::room_closed);done=true;return true;
                }else if(e.type==q2dz::ChildEventType::fatal){std::cout<<"q2dz-session: gateway fatal stage="<<e.fields.at("stage")<<" code="<<e.fields.at("code")<<"\n";(void)sm.apply(q2dz::SessionEvent::fatal_error);done=true;return true;}
            }
            return false;
        };
        while(!done){
            HANDLE hs[4]={g_stop_event.get(),gateway.process.get(),q.event.get(),q2p?q2p->process.get():nullptr};DWORD count=q2p?4:3;DWORD timeout=INFINITE;if(sm.state()==q2dz::SessionState::waiting_transport){auto now=GetTickCount64();timeout=now>=deadline?0:static_cast<DWORD>(deadline-now);}DWORD w=WaitForMultipleObjects(count,hs,FALSE,timeout);
            if(w==WAIT_TIMEOUT){(void)sm.apply(q2dz::SessionEvent::timeout);done=true;continue;}
            if(w==WAIT_OBJECT_0){(void)sm.apply(q2dz::SessionEvent::owner_stop);done=true;continue;}
            if(w==WAIT_OBJECT_0+1){
                owner_pipe.write.reset();
                if(reader_thread&&reader_thread->joinable())reader_thread->join();
                event_read.reset();
                if(!consume_events(q.drain())) {(void)sm.apply(q2dz::SessionEvent::gateway_exited);done=true;}
                continue;
            }
            if(w==WAIT_OBJECT_0+3&&q2p){(void)sm.apply(q2dz::SessionEvent::q2_exited);done=true;continue;}
            if(w==WAIT_OBJECT_0+2){
                consume_events(q.drain());
            }
        }
        log_state(sm);cleanup();return q2dz::exit_code_for(sm.cause());
    }catch(const std::exception& e){(void)sm.apply(q2dz::SessionEvent::fatal_error);cleanup();std::cerr<<"q2dz-session: "<<e.what()<<"\n";const auto code=q2dz::exit_code_for(sm.cause());return code==q2dz::exit_code_for(q2dz::ShutdownCause::cleanup_error)?code:2;
    }catch(...){(void)sm.apply(q2dz::SessionEvent::fatal_error);cleanup();std::cerr<<"q2dz-session: unknown failure\n";const auto code=q2dz::exit_code_for(sm.cause());return code==q2dz::exit_code_for(q2dz::ShutdownCause::cleanup_error)?code:2;}
}
#endif
