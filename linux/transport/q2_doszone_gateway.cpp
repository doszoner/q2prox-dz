#include "gateway_options.h"
#include "probe_options.h"

#include "humblenet_p2p.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

volatile std::sig_atomic_t running = 1;

void stop(int) { running = 0; }

std::runtime_error humblenet_error() {
    const char *message = humblenet_get_error();
    return std::runtime_error(message ? message : "unknown HumbleNet error");
}

bool same_client(const sockaddr_in &left, const sockaddr_in &right) {
    return left.sin_family == right.sin_family &&
           left.sin_port == right.sin_port &&
           left.sin_addr.s_addr == right.sin_addr.s_addr;
}

struct Shutdown {
    int socket = -1;
    ~Shutdown() {
        if (socket >= 0) close(socket);
        humblenet_shutdown();
    }
};

}  // namespace

int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "Usage: q2dz-webrtc-gateway --bind 127.0.0.1:PORT "
                         "--signal wss://net.js-dos.com:444 --ice-file PATH "
                         "--alias q2.ID --max-datagram 1400\n";
            return 0;
        }
        const GatewayOptions options = parse_gateway_options(argc, argv);
        std::ifstream ice_file(options.ice_file, std::ios::binary);
        const std::string ice_json{std::istreambuf_iterator<char>(ice_file), {}};
        if (!ice_file || ice_json.size() > 65536 || !valid_ice_json(ice_json)) {
            throw std::runtime_error("invalid ICE file");
        }
        if (!humblenet_set_iceservers(ice_json.c_str()) || !humblenet_init() ||
            !humblenet_p2p_init(options.signal.c_str(), "quake2", "quake2-secret",
                                nullptr)) {
            throw humblenet_error();
        }

        Shutdown shutdown;
        shutdown.socket = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        if (shutdown.socket < 0) throw std::runtime_error(std::strerror(errno));
        const sockaddr_in local{AF_INET, htons(options.port), {htonl(INADDR_LOOPBACK)}, {}};
        if (bind(shutdown.socket, reinterpret_cast<const sockaddr *>(&local),
                 sizeof(local)) < 0) {
            throw std::runtime_error(std::strerror(errno));
        }
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        std::cout.setf(std::ios::unitbuf);

        PeerId own_peer = 0;
        PeerId remote_peer = 0;
        sockaddr_in client{};
        bool have_client = false;
        std::uint64_t udp_to_webrtc = 0;
        std::uint64_t webrtc_to_udp = 0;
        std::size_t max_udp_to_transport_datagram = 0;
        std::size_t max_transport_to_udp_datagram = 0;
        std::uint64_t oversized_udp_drops = 0;
        std::uint64_t oversized_transport_drops = 0;
        std::vector<unsigned char> buffer(static_cast<std::size_t>(options.max_datagram));

        while (running) {
            humblenet_p2p_wait(1);
            if (own_peer == 0) {
                own_peer = humblenet_p2p_get_my_peer_id();
                if (own_peer != 0) {
                    remote_peer = humblenet_p2p_virtual_peer_for_alias(
                        options.alias.c_str());
                    if (remote_peer == 0) throw std::runtime_error("alias failed");
                    std::cout << "GATEWAY_READY bind=127.0.0.1:" << options.port
                              << " own_peer_id=" << own_peer << " alias="
                              << options.alias << '\n';
                }
            }

            sockaddr_in source{};
            iovec iov{buffer.data(), buffer.size()};
            msghdr message{};
            message.msg_name = &source;
            message.msg_namelen = sizeof(source);
            message.msg_iov = &iov;
            message.msg_iovlen = 1;
            const ssize_t udp_length = recvmsg(
                shutdown.socket, &message, MSG_DONTWAIT | MSG_TRUNC);
            if (udp_length > 0) {
                if (udp_length > options.max_datagram) {
                    ++oversized_udp_drops;
                } else if (have_client && !same_client(client, source)) {
                    std::cout << "DROP second_udp_source\n";
                } else if (remote_peer != 0) {
                    if (!have_client) {
                        client = source;
                        have_client = true;
                        std::cout << "UDP_CLIENT_CAPTURED\n";
                    }
                    const int sent = humblenet_p2p_sendto(
                        buffer.data(), static_cast<std::uint32_t>(udp_length),
                        remote_peer, SEND_RELIABLE, 0);
                    if (sent < 0) throw humblenet_error();
                    if (sent == udp_length) {
                        ++udp_to_webrtc;
                        max_udp_to_transport_datagram = std::max(
                            max_udp_to_transport_datagram,
                            static_cast<std::size_t>(udp_length));
                    }
                }
            } else if (udp_length < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                throw std::runtime_error(std::strerror(errno));
            }

            PeerId from_peer = 0;
            const int web_length = humblenet_p2p_recvfrom(
                buffer.data(), static_cast<std::uint32_t>(buffer.size()), &from_peer, 0);
            const int receive_exit = gateway_receive_exit_code(web_length, from_peer);
            if (receive_exit == 20) {
                std::cerr << "GATEWAY_ROOM_CLOSED peer-closed\n";
                return receive_exit;
            }
            if (receive_exit != 0) throw humblenet_error();
            if (web_length > options.max_datagram) {
                ++oversized_transport_drops;
            } else if (web_length > 0 && have_client && from_peer != 0) {
                const ssize_t sent = sendto(
                    shutdown.socket, buffer.data(), static_cast<std::size_t>(web_length), 0,
                    reinterpret_cast<const sockaddr *>(&client), sizeof(client));
                if (sent != web_length) throw std::runtime_error(std::strerror(errno));
                ++webrtc_to_udp;
                max_transport_to_udp_datagram = std::max(
                    max_transport_to_udp_datagram,
                    static_cast<std::size_t>(web_length));
            }
        }
        std::cout << "GATEWAY_STOP udp_to_webrtc=" << udp_to_webrtc
                  << " webrtc_to_udp=" << webrtc_to_udp
                  << " max_udp_to_transport_datagram=" << max_udp_to_transport_datagram
                  << " max_transport_to_udp_datagram=" << max_transport_to_udp_datagram
                  << " oversized_udp_drops=" << oversized_udp_drops
                  << " oversized_transport_drops=" << oversized_transport_drops << '\n';
        return 0;
    } catch (const std::exception &error) {
        const int exit_code = gateway_error_exit_code(error.what());
        if (exit_code == 20) {
            std::cerr << "GATEWAY_ROOM_CLOSED peer-closed\n";
            return exit_code;
        }
        std::cerr << "GATEWAY_ERROR " << error.what() << '\n';
        return exit_code;
    }
}
