#include "q2dz/child_protocol.hpp"
#include "q2dz/session_state.hpp"
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
int failures=0;
#define CHECK(x) do { if(!(x)){ std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #x "\n"; ++failures; } } while(0)

void protocol_tests() {
    auto ok=q2dz::parse_child_event("Q2DZ/1 TRANSPORT_READY udp=127.0.0.1:27911 peer=42\r\n");
    CHECK(ok.event.has_value());
    CHECK(ok.event && ok.event->type==q2dz::ChildEventType::transport_ready);
    CHECK(ok.event && ok.event->fields.at("udp")=="127.0.0.1:27911");
    CHECK(q2dz::parse_loopback_endpoint("127.0.0.1:1024").has_value());
    CHECK(q2dz::parse_loopback_endpoint("127.0.0.1:65535").has_value());
    CHECK(!q2dz::parse_loopback_endpoint("0.0.0.0:27910"));
    CHECK(!q2dz::parse_loopback_endpoint("127.0.0.1:80"));
    CHECK(!q2dz::parse_loopback_endpoint("127.0.0.1:65536"));
    CHECK(!q2dz::parse_child_event("Q2DZ/1 TRANSPORT_READY udp=8.8.8.8:27910").event);
    CHECK(!q2dz::parse_child_event("Q2DZ/1 FATAL credential=leak").event);
    CHECK(!q2dz::parse_child_event("Q2DZ/1 FATAL stage=gateway").event);
    CHECK(q2dz::parse_child_event("Q2DZ/1 FATAL stage=gateway code=connect_failed").event);
    CHECK(!q2dz::parse_child_event("Q2DZ/1 FATAL stage=ice stage=two").event);
    auto closed=q2dz::parse_child_event("Q2DZ/1 ROOM_CLOSED reason=probe%20timeout");
    CHECK(closed.event && closed.event->fields.at("reason")=="probe timeout");
}

void framer_tests() {
    constexpr auto line_too_long = "Q2DZ/1 FATAL stage=protocol code=line_too_long\n";
    constexpr auto queue_overflow = "Q2DZ/1 FATAL stage=protocol code=queue_overflow\n";

    q2dz::ChildEventFramer no_newline;
    CHECK(!no_newline.feed(std::string(100 * 1024, 'x')));
    CHECK(no_newline.error()==q2dz::ChildEventFramer::Error::line_too_long);
    CHECK(no_newline.pending_byte_count()<=q2dz::ChildEventFramer::max_line_bytes);
    auto no_newline_events=no_newline.drain();
    CHECK(no_newline_events.size()==1);
    CHECK(no_newline_events.front()==line_too_long);
    CHECK(!no_newline.feed("Q2DZ/1 HEARTBEAT\n"));
    CHECK(no_newline.drain().empty());

    q2dz::ChildEventFramer oversized;
    std::string oversized_line(q2dz::ChildEventFramer::max_line_bytes, 'x');
    oversized_line.push_back('\n');
    CHECK(!oversized.feed(oversized_line));
    CHECK(oversized.error()==q2dz::ChildEventFramer::Error::line_too_long);
    auto oversized_events=oversized.drain();
    CHECK(oversized_events.size()==1);
    CHECK(oversized_events.front()==line_too_long);
    CHECK(oversized.drain().empty());

    q2dz::ChildEventFramer legal;
    CHECK(legal.feed("Q2DZ/1 HEARTBEAT\nQ2DZ/1 HEARTBEAT\n"));
    CHECK(legal.queued_line_count()==2);
    CHECK(legal.queued_byte_count()==34);
    auto legal_events=legal.drain();
    CHECK(legal_events.size()==2);
    CHECK(legal_events.front()=="Q2DZ/1 HEARTBEAT\n");
    CHECK(legal_events.back()=="Q2DZ/1 HEARTBEAT\n");
    CHECK(legal.queued_line_count()==0);
    CHECK(legal.queued_byte_count()==0);

    q2dz::ChildEventFramer by_count;
    for (std::size_t i=0; i<q2dz::ChildEventFramer::max_queued_lines; ++i) {
        if (!by_count.feed("Q2DZ/1 HEARTBEAT\n")) break;
    }
    CHECK(by_count.error()==q2dz::ChildEventFramer::Error::queue_overflow);
    CHECK(by_count.queued_line_count()==q2dz::ChildEventFramer::max_queued_lines);
    CHECK(by_count.queued_byte_count()<=q2dz::ChildEventFramer::max_queued_bytes);
    auto count_events=by_count.drain();
    CHECK(count_events.size()==q2dz::ChildEventFramer::max_queued_lines);
    CHECK(count_events.back()==queue_overflow);
    CHECK(by_count.queued_byte_count()==0);

    q2dz::ChildEventFramer by_bytes;
    std::string large_line(q2dz::ChildEventFramer::max_line_bytes-1, 'x');
    large_line.push_back('\n');
    for (std::size_t i=0; i<q2dz::ChildEventFramer::max_queued_lines; ++i) {
        if (!by_bytes.feed(large_line)) break;
    }
    CHECK(by_bytes.error()==q2dz::ChildEventFramer::Error::queue_overflow);
    CHECK(by_bytes.queued_byte_count()<=q2dz::ChildEventFramer::max_queued_bytes);
    auto byte_events=by_bytes.drain();
    CHECK(byte_events.size()<q2dz::ChildEventFramer::max_queued_lines);
    CHECK(byte_events.back()==queue_overflow);
    CHECK(by_bytes.queued_byte_count()==0);
}

void state_tests() {
    using E=q2dz::SessionEvent; using S=q2dz::SessionState;
    q2dz::SessionStateMachine m;
    CHECK(m.apply(E::begin).current==S::starting_gateway);
    CHECK(m.apply(E::gateway_started).current==S::waiting_transport);
    CHECK(m.apply(E::transport_ready).current==S::starting_q2);
    CHECK(m.apply(E::q2_started).current==S::active);
    auto c=m.apply(E::q2_exited);
    CHECK(c.current==S::closing);
    CHECK(c.cause==q2dz::ShutdownCause::q2_exit);
    CHECK(m.apply(E::cleanup_complete).current==S::closed);

    q2dz::SessionStateMachine n;
    CHECK(!n.apply(E::transport_ready).accepted);
    CHECK(n.state()==S::idle);
    CHECK(n.apply(E::begin).accepted);
    CHECK(n.apply(E::fatal_error).current==S::closing);
    CHECK(n.apply(E::cleanup_failed).current==S::failed);

    CHECK(q2dz::exit_code_for(q2dz::ShutdownCause::q2_exit)==0);
    CHECK(q2dz::exit_code_for(q2dz::ShutdownCause::owner_stop)==0);
    CHECK(q2dz::exit_code_for(q2dz::ShutdownCause::room_close)==10);
    CHECK(q2dz::exit_code_for(q2dz::ShutdownCause::gateway_exit)==11);
    CHECK(q2dz::exit_code_for(q2dz::ShutdownCause::timeout)==12);
    CHECK(q2dz::exit_code_for(q2dz::ShutdownCause::fatal_error)==13);
    CHECK(q2dz::exit_code_for(q2dz::ShutdownCause::cleanup_error)==14);
}
}

int main() {
    protocol_tests(); framer_tests(); state_tests();
    if (failures) { std::cerr << failures << " test(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "q2dz-core-tests: PASS\n";
    return EXIT_SUCCESS;
}
