#ifdef _WIN32
#include "win32_support.hpp"
#include "q2dz/child_protocol.hpp"
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <memory>
#include <vector>

namespace fs=std::filesystem;
namespace {
using q2dz::win::Handle;
Handle g_stop_event;
BOOL WINAPI ctrl_handler(DWORD t){if(t==CTRL_C_EVENT||t==CTRL_BREAK_EVENT||t==CTRL_CLOSE_EVENT||t==CTRL_LOGOFF_EVENT||t==CTRL_SHUTDOWN_EVENT){if(g_stop_event)SetEvent(g_stop_event.get());return TRUE;}return FALSE;}

struct Options{fs::path gateway;std::optional<fs::path> q2;std::optional<fs::path> root;DWORD ready_ms=60000;DWORD graceful_ms=2500;};
[[noreturn]] void usage(){throw std::runtime_error("usage: q2dz-session --gateway <exe> [--q2 <Q2PRO-X.exe>] [--session-root <dir>] [--ready-timeout-ms N] [--graceful-timeout-ms N]; one or two room JSON objects must be supplied on stdin");}
DWORD parse_ms(const wchar_t* s){wchar_t* e=nullptr;unsigned long v=wcstoul(s,&e,10);if(!s[0]||*e||v<100||v>600000)usage();return static_cast<DWORD>(v);}
Options parse(int argc,wchar_t** argv){Options o;for(int i=1;i<argc;++i){std::wstring a=argv[i];auto val=[&]()->const wchar_t*{if(++i>=argc)usage();return argv[i];};if(a==L"--gateway")o.gateway=val();else if(a==L"--q2")o.q2=fs::path(val());else if(a==L"--session-root")o.root=fs::path(val());else if(a==L"--ready-timeout-ms")o.ready_ms=parse_ms(val());else if(a==L"--graceful-timeout-ms")o.graceful_ms=parse_ms(val());else usage();}if(o.gateway.empty())usage();return o;}

struct Queue{std::mutex m;q2dz::ChildEventFramer framer;Handle event{CreateEventW(nullptr,TRUE,FALSE,nullptr)};bool feed(std::string_view bytes){std::lock_guard l(m);const bool keep=framer.feed(bytes);SetEvent(event.get());return keep;}void finish(){std::lock_guard l(m);(void)framer.finish();SetEvent(event.get());}std::deque<std::string> drain(){std::lock_guard l(m);auto x=framer.drain();ResetEvent(event.get());return x;}};
void reader(HANDLE h,Queue& q){char b[1024];for(;;){DWORD n=0;if(!ReadFile(h,b,sizeof(b),&n,nullptr)||!n)break;if(!q.feed(std::string_view(b,n)))break;}q.finish();}

void terminate_unassigned(q2dz::win::ChildProcess& child) noexcept {
    if(child.process) {
        (void)TerminateProcess(child.process.get(),3);
        (void)WaitForSingleObject(child.process.get(),5000);
    }
    child.thread.reset();
    child.process.reset();
}
void assign_job(HANDLE job,q2dz::win::ChildProcess& child,const wchar_t* role){
    bool injected=false;
    wchar_t value[16]{};
    const DWORD length=GetEnvironmentVariableW(L"Q2DZ_TEST_FAIL_ASSIGNMENT",value,static_cast<DWORD>(std::size(value)));
    if(length>0 && length<std::size(value)) {
        const std::wstring mode(value,length);
        injected=mode==L"1" || mode==role;
    }
    if(injected || !AssignProcessToJobObject(job,child.process.get())) {
        const DWORD error=injected?ERROR_ACCESS_DENIED:GetLastError();
        terminate_unassigned(child);
        throw std::runtime_error("AssignProcessToJobObject failed: "+std::to_string(error));
    }
}
Handle make_kill_job(){
    Handle job(CreateJobObjectW(nullptr,nullptr));
    if(!job) throw std::runtime_error("CreateJobObject failed");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if(!SetInformationJobObject(job.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits))) {
        throw std::runtime_error("SetInformationJobObject failed");
    }
    return job;
}
void resume(HANDLE thread){if(ResumeThread(thread)==static_cast<DWORD>(-1))throw std::runtime_error("ResumeThread failed");}

void spawn_cleanup_guardian(const fs::path& self,const fs::path& root,const fs::path& session) {
    Handle owner(OpenProcess(SYNCHRONIZE,FALSE,GetCurrentProcessId()));
    if(!owner) throw std::runtime_error("OpenProcess for cleanup guardian failed");
    if(!SetHandleInformation(owner.get(),HANDLE_FLAG_INHERIT,HANDLE_FLAG_INHERIT)) {
        throw std::runtime_error("SetHandleInformation for cleanup guardian failed");
    }
    auto nullin=q2dz::win::open_null(false);
    auto nullout=q2dz::win::open_null(true);
    auto nullerr=q2dz::win::open_null(true);
    const auto owner_value=std::to_wstring(reinterpret_cast<std::uintptr_t>(owner.get()));
    const std::vector<std::wstring> args={L"--cleanup-session",root.wstring(),session.wstring(),L"--owner-handle",owner_value};
    const std::vector<HANDLE> inherited={owner.get(),nullin.get(),nullout.get(),nullerr.get()};
    auto guardian=q2dz::win::launch_suspended(self,args,self.parent_path(),nullin.get(),nullout.get(),nullerr.get(),inherited,CREATE_NO_WINDOW);
    try {
        resume(guardian.thread.get());
    } catch(...) {
        terminate_unassigned(guardian);
        throw;
    }
    guardian.thread.reset();
    guardian.process.reset();
}

bool json_shape(const std::vector<unsigned char>& v){auto first=std::find_if(v.begin(),v.end(),[](unsigned char c){return c!=' '&&c!='\t'&&c!='\r'&&c!='\n';});auto last=std::find_if(v.rbegin(),v.rend(),[](unsigned char c){return c!=' '&&c!='\t'&&c!='\r'&&c!='\n';});return first!=v.end()&&last!=v.rend()&&*first=='{'&&*last=='}';}
constexpr std::size_t hub_max_rooms=2;
constexpr std::size_t hub_max_line_bytes=64*1024;

std::vector<std::vector<unsigned char>> split_hub_config(std::vector<unsigned char> data) {
    std::vector<std::vector<unsigned char>> result;
    std::size_t start=0;
    for (;;) {
        const auto found=std::find(data.begin()+static_cast<std::ptrdiff_t>(start),data.end(),static_cast<unsigned char>('\n'));
        const std::size_t end=found==data.end()?data.size():static_cast<std::size_t>(std::distance(data.begin(),found));
        if(end-start>hub_max_line_bytes) { q2dz::win::secure_clear(data); throw std::runtime_error("hub room line exceeds 64 KiB"); }
        std::size_t first=start,last=end;
        while(first<last && (data[first]==' '||data[first]=='\t'||data[first]=='\r')) ++first;
        while(last>first && (data[last-1]==' '||data[last-1]=='\t'||data[last-1]=='\r')) --last;
        if(first==last) {
            if(end==data.size() && start==data.size()) break;
            if(end==data.size()) { q2dz::win::secure_clear(data); throw std::runtime_error("hub stdin contains an empty room line"); }
            q2dz::win::secure_clear(data); throw std::runtime_error("hub stdin contains an empty room line");
        }
        std::vector<unsigned char> line(data.begin()+static_cast<std::ptrdiff_t>(first),data.begin()+static_cast<std::ptrdiff_t>(last));
        if(!json_shape(line)) { q2dz::win::secure_clear(data); throw std::runtime_error("hub stdin lines must contain JSON room objects"); }
        result.push_back(std::move(line));
        if(result.size()>hub_max_rooms) { q2dz::win::secure_clear(data); throw std::runtime_error("hub accepts at most two room objects"); }
        if(end==data.size()) break;
        start=end+1;
        if(start==data.size()) break;
    }
    q2dz::win::secure_clear(data);
    if(result.empty()) throw std::runtime_error("hub stdin must contain one or two room objects");
    return result;
}

struct HubGateway {
    fs::path directory;
    Handle job;
    q2dz::win::Pipe owner;
    Handle event_read;
    q2dz::win::ChildProcess process;
    std::optional<std::jthread> reader;
    Queue events;
    std::optional<q2dz::LoopbackEndpoint> endpoint;
    bool closed=false;
};

void stop_hub_gateway(HubGateway& room,DWORD graceful_ms) noexcept {
    if(room.closed) return;
    room.owner.write.reset();
    if(room.process.process) {
        (void)WaitForSingleObject(room.process.process.get(),graceful_ms);
    }
    // The gateway is in this child Job after the shared root Job. Closing the
    // child Job kills only this gateway and descendants such as WebView2.
    room.job.reset();
    if(room.process.process) (void)WaitForSingleObject(room.process.process.get(),5000);
    if(room.reader) {
        (void)room.reader->request_stop();
        if(room.reader->joinable()) {
            (void)CancelSynchronousIo(room.reader->native_handle());
            room.reader->join();
        }
        room.reader.reset();
    }
    (void)room.events.drain();
    room.event_read.reset();
    room.process.thread.reset();
    room.process.process.reset();
    room.closed=true;
}

std::wstring hub_source(const std::vector<std::unique_ptr<HubGateway>>& rooms) {
    std::string source;
    for(const auto& room:rooms) if(!room->closed && room->endpoint) {
        if(!source.empty()) source.push_back(' ');
        source += "quake2://"+room->endpoint->text();
    }
    if(source.empty()) throw std::runtime_error("hub has no ready room endpoint");
    return std::wstring(source.begin(),source.end());
}

std::vector<HANDLE> hub_wait_handles(const std::vector<std::unique_ptr<HubGateway>>& rooms,
                                     std::vector<std::pair<HubGateway*,bool>>& refs,
                                     HANDLE stop,HANDLE q2) {
    std::vector<HANDLE> handles{stop};
    refs.clear();
    refs.emplace_back(nullptr,false);
    if(q2) { handles.push_back(q2); refs.emplace_back(nullptr,true); }
    for(const auto& room:rooms) if(!room->closed) {
        if(room->events.event) { handles.push_back(room->events.event.get()); refs.emplace_back(room.get(),false); }
        if(room->process.process) { handles.push_back(room->process.process.get()); refs.emplace_back(room.get(),true); }
    }
    return handles;
}

bool consume_hub_events(HubGateway& room) {
    bool terminal=false;
    for(auto& line:room.events.drain()) {
        const auto parsed=q2dz::parse_child_event(line);
        if(!parsed.event) { std::cerr<<"q2dz-session: hub gateway protocol failure\n"; terminal=true; break; }
        const auto& event=*parsed.event;
        if(event.type==q2dz::ChildEventType::transport_ready) {
            if(room.endpoint) { terminal=true; break; }
            room.endpoint=q2dz::parse_loopback_endpoint(event.fields.at("udp"));
            if(!room.endpoint) { terminal=true; break; }
        } else if(event.type==q2dz::ChildEventType::room_closed) {
            std::cout<<"q2dz-session: hub room closed\n";
            terminal=true; break;
        } else if(event.type==q2dz::ChildEventType::fatal) {
            std::cout<<"q2dz-session: hub gateway fatal stage="<<event.fields.at("stage")<<" code="<<event.fields.at("code")<<"\n";
            terminal=true; break;
        }
    }
    return terminal;
}

bool reap_hub_gateway_if_exited(HubGateway& room,DWORD graceful_ms) {
    if(room.closed || !room.process.process ||
       WaitForSingleObject(room.process.process.get(),0)!=WAIT_OBJECT_0) return false;
    (void)consume_hub_events(room);
    stop_hub_gateway(room,graceful_ms);
    return true;
}

int run_hub(const Options& options,const fs::path& q2,const fs::path& self,std::vector<unsigned char> input) {
    q2dz::win::Handle job;
    q2dz::win::Handle session_lock;
    q2dz::win::Handle stop_event;
    std::vector<std::unique_ptr<HubGateway>> rooms;
    std::optional<q2dz::win::ChildProcess> q2_process;
    fs::path session;
    bool session_created=false;
    bool ctrl_handler_set=false;
    int result=13;
    auto cleanup=[&]() noexcept {
        for(auto& room:rooms) stop_hub_gateway(*room,options.graceful_ms);
        if(q2_process && q2_process->process) {
            if(WaitForSingleObject(q2_process->process.get(),options.graceful_ms)==WAIT_TIMEOUT) {
                if(job) TerminateJobObject(job.get(),3);
                (void)WaitForSingleObject(q2_process->process.get(),5000);
            }
        }
        if(job) job.reset();
        q2_process.reset();
        if(ctrl_handler_set){SetConsoleCtrlHandler(ctrl_handler,FALSE);ctrl_handler_set=false;}
        g_stop_event.reset();
        session_lock.reset();
        if(session_created){
            if(!q2dz::win::remove_tree_with_retry(session,10000)) result=14;
            session_created=false;
        }
    };
    try {
        auto configs=split_hub_config(std::move(input));
        if(!fs::is_regular_file(options.gateway)) throw std::runtime_error("gateway executable not found");
        stop_event.reset(CreateEventW(nullptr,TRUE,FALSE,nullptr));
        if(!stop_event) throw std::runtime_error("CreateEvent failed");
        g_stop_event=std::move(stop_event);
        SetConsoleCtrlHandler(ctrl_handler,TRUE);ctrl_handler_set=true;
        job=make_kill_job();
        const fs::path root=options.root.value_or(q2dz::win::default_session_root());
        q2dz::win::cleanup_stale_sessions(root);
        session=q2dz::win::create_session_directory(root,session_lock);session_created=true;
        // Keep the cleanup guardian outside the hub Job. It waits on the
        // coordinator's inherited process handle and removes this session
        // directory if the owner is terminated before normal cleanup runs.
        spawn_cleanup_guardian(self,root,session);
        for(std::size_t i=0;i<configs.size();++i) {
            auto room=std::make_unique<HubGateway>();
            room->job=make_kill_job();
            room->directory=session/(L"room-"+std::to_wstring(i+1));
            fs::create_directories(room->directory/L"webview2");
            auto config_pipe=q2dz::win::make_pipe(true,false);
            auto event_pipe=q2dz::win::make_pipe(false,true);
            room->owner=q2dz::win::make_pipe(true,false);
            auto nullerr=q2dz::win::open_null(true);
            std::vector<std::wstring> args={L"--managed-session",L"--udp-port",L"0",L"--owner-handle",std::to_wstring(reinterpret_cast<std::uintptr_t>(room->owner.read.get())),L"--user-data-dir",(room->directory/L"webview2").wstring()};
            std::vector<HANDLE> inherited={config_pipe.read.get(),event_pipe.write.get(),room->owner.read.get(),nullerr.get()};
            room->process=q2dz::win::launch_suspended(options.gateway,args,options.gateway.parent_path(),config_pipe.read.get(),event_pipe.write.get(),nullerr.get(),inherited,CREATE_NO_WINDOW|CREATE_NEW_PROCESS_GROUP);
            assign_job(job.get(),room->process,L"gateway");
            // Root first, child second: this is the documented Windows nested
            // Job order and gives each room a selectively killable process tree.
            assign_job(room->job.get(),room->process,L"gateway-room");
            config_pipe.read.reset();event_pipe.write.reset();room->owner.read.reset();resume(room->process.thread.get());room->process.thread.reset();
            q2dz::win::write_all(config_pipe.write.get(),configs[i].data(),configs[i].size());config_pipe.write.reset();
            q2dz::win::secure_clear(configs[i]);
            room->event_read=std::move(event_pipe.read);
            room->reader.emplace([h=room->event_read.get(),queue=&room->events](std::stop_token){reader(h,*queue);});
            rooms.push_back(std::move(room));
        }
        q2dz::win::secure_clear(input);
        const ULONGLONG deadline=GetTickCount64()+options.ready_ms;
        for(;;) {
            bool reaped=false;
            for(auto& room:rooms) if(reap_hub_gateway_if_exited(*room,options.graceful_ms)) reaped=true;
            if(reaped) continue;
            std::size_t live=0,ready=0;
            for(const auto& room:rooms) if(!room->closed){++live;if(room->endpoint)++ready;}
            if(live==ready) { if(ready==0) throw std::runtime_error("hub has no ready rooms"); break; }
            auto refs=std::vector<std::pair<HubGateway*,bool>>{};
            auto handles=hub_wait_handles(rooms,refs,g_stop_event.get(),nullptr);
            auto now=GetTickCount64();const DWORD timeout=now>=deadline?0:static_cast<DWORD>(deadline-now);
            const DWORD wait=WaitForMultipleObjects(static_cast<DWORD>(handles.size()),handles.data(),FALSE,timeout);
            if(wait==WAIT_TIMEOUT) {
                for(auto& room:rooms) if(!room->closed&&!room->endpoint) stop_hub_gateway(*room,options.graceful_ms);
                for(auto& room:rooms) (void)reap_hub_gateway_if_exited(*room,options.graceful_ms);
                for(const auto& room:rooms) if(!room->closed&&room->endpoint) { ready=1; }
                if(ready==0) { result=12; cleanup(); return result; }
                break;
            }
            if(wait==WAIT_OBJECT_0) { result=0; cleanup(); return result; }
            const auto index=static_cast<std::size_t>(wait-WAIT_OBJECT_0);
            if(index>=refs.size()) throw std::runtime_error("hub wait result out of range");
            auto* room=refs[index].first;
            if(!room) continue;
            if(refs[index].second) {
                if(WaitForSingleObject(room->process.process.get(),0)==WAIT_OBJECT_0) {
                    (void)consume_hub_events(*room);stop_hub_gateway(*room,options.graceful_ms);
                }
            } else if(consume_hub_events(*room) ||
                      reap_hub_gateway_if_exited(*room,options.graceful_ms)) {
                if(!room->closed) stop_hub_gateway(*room,options.graceful_ms);
            }
        }
        const auto source=hub_source(rooms);
        std::vector<std::wstring> qargs={L"+set",L"game",L"3zb2",L"+set",L"cl_serverbrowser_custom_source",source,L"+serverbrowser_open_modern"};
        auto pending=q2dz::win::launch_suspended(q2,qargs,q2.parent_path(),GetStdHandle(STD_INPUT_HANDLE),GetStdHandle(STD_OUTPUT_HANDLE),GetStdHandle(STD_ERROR_HANDLE),{},0);
        assign_job(job.get(),pending,L"q2");resume(pending.thread.get());pending.thread.reset();q2_process.emplace(std::move(pending));
        for(;;) {
            auto refs=std::vector<std::pair<HubGateway*,bool>>{};
            auto handles=hub_wait_handles(rooms,refs,g_stop_event.get(),q2_process->process.get());
            const DWORD wait=WaitForMultipleObjects(static_cast<DWORD>(handles.size()),handles.data(),FALSE,INFINITE);
            if(wait==WAIT_OBJECT_0){result=0;break;}
            if(wait==WAIT_OBJECT_0+1){
                DWORD code=0;
                result=GetExitCodeProcess(q2_process->process.get(),&code) && code==0 ? 0 : 74;
                break;
            }
            const auto index=static_cast<std::size_t>(wait-WAIT_OBJECT_0);
            if(index>=refs.size()) { result=13; break; }
            auto* room=refs[index].first;
            if(!room) continue;
            if(refs[index].second) { (void)consume_hub_events(*room);stop_hub_gateway(*room,options.graceful_ms); }
            else if(consume_hub_events(*room)) stop_hub_gateway(*room,options.graceful_ms);
        }
        cleanup();return result;
    } catch(const std::exception& error) {
        q2dz::win::secure_clear(input);cleanup();std::cerr<<"q2dz-session: "<<error.what()<<"\n";return result;
    } catch(...) {
        q2dz::win::secure_clear(input);cleanup();std::cerr<<"q2dz-session: unknown failure\n";return result;
    }
}

int run_cleanup_guardian(int argc,wchar_t** argv) {
    if(argc!=6 || std::wstring(argv[1])!=L"--cleanup-session" ||
       std::wstring(argv[4])!=L"--owner-handle") return 2;
    wchar_t* end=nullptr;
    const auto raw=wcstoull(argv[5],&end,10);
    if(!argv[5][0] || !end || *end || raw==0) return 2;
    const fs::path root(argv[2]);
    const fs::path session(argv[3]);
    std::error_code ec;
    const bool present=fs::exists(session,ec);
    if(ec) return 2;
    if(!present) return 0;
    if(!q2dz::win::validate_session_for_cleanup(root,session)) return 2;
    Handle owner(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(raw)));
    const DWORD wait=WaitForSingleObject(owner.get(),INFINITE);
    if(wait!=WAIT_OBJECT_0) return 2;
    const bool remains=fs::exists(session,ec);
    if(ec) return 2;
    if(!remains) return 0;
    if(!q2dz::win::validate_session_for_cleanup(root,session)) return 2;
    return q2dz::win::remove_tree_with_retry(session,10000)?0:14;
}
}

int wmain(int argc,wchar_t** argv){
    try {
        if(argc>=2 && std::wstring(argv[1])==L"--cleanup-session") return run_cleanup_guardian(argc,argv);
        const auto options=parse(argc,argv);
        const fs::path self=[](){std::wstring b(32768,L'\0');DWORD n=GetModuleFileNameW(nullptr,b.data(),static_cast<DWORD>(b.size()));b.resize(n);return fs::path(b);}();
        const auto q2=q2dz::win::locate_q2(options.q2,self);
        if(!q2) throw std::runtime_error("Q2PRO-X.exe not found; pass --q2 or set Q2PROX_EXE");
        if(!fs::is_regular_file(options.gateway)) throw std::runtime_error("gateway executable not found");
        auto input=q2dz::win::read_all_stdin(256*1024);
        if(!json_shape(input)) { q2dz::win::secure_clear(input); throw std::runtime_error("stdin must contain one or two UTF-8 JSON room configuration objects"); }
        return run_hub(options,*q2,self,std::move(input));
    } catch(const std::exception& error) {
        std::cerr<<"q2dz-session: "<<error.what()<<"\n";
        return 2;
    } catch(...) {
        std::cerr<<"q2dz-session: unknown failure\n";
        return 2;
    }
}
#endif
