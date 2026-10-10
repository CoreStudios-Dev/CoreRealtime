// Copyright (c) 2026, the Luxon contributors
// SPDX-License-Identifier: BSD-3-Clause

#include <array>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <luxon/common_codes.hpp>
#include <luxon/enet_peer.hpp>
#include <luxon/internal_codes.hpp>
#include <luxon/ser_gp_binary_v18.hpp>
#include <luxon/visualizer.hpp>

#ifdef _WIN32
#include <stdexcept>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

using namespace luxon;

namespace {

constexpr std::string_view kColorReset = "\033[0m";
constexpr std::string_view kColorClient = "\033[36m";
constexpr std::string_view kColorServer = "\033[33m";
constexpr std::string_view kColorSystem = "\033[90m";
constexpr std::string_view kColorError = "\033[31m";

constexpr uint8_t kRemoteAddressParameter = 230;
constexpr std::size_t kMaximumUdpDatagramSize = 65'536;

volatile std::sig_atomic_t g_running = 1;

void signal_handler_int(int) { g_running = 0; }

#ifdef _WIN32

class WinsockRuntime {
public:
    WinsockRuntime() {
        WSADATA data{};
        const int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) {
            throw std::runtime_error(std::format("WSAStartup failed with error {}", result));
        }

        initialized_ = true;
    }

    ~WinsockRuntime() {
        if (initialized_) {
            WSACleanup();
        }
    }

    WinsockRuntime(const WinsockRuntime&) = delete;
    WinsockRuntime& operator=(const WinsockRuntime&) = delete;
    WinsockRuntime(WinsockRuntime&&) = delete;
    WinsockRuntime& operator=(WinsockRuntime&&) = delete;

private:
    bool initialized_ = false;
};

void enable_windows_ansi_colors() {
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == INVALID_HANDLE_VALUE || output == nullptr) {
        return;
    }

    DWORD mode = 0;
    if (!GetConsoleMode(output, &mode)) {
        return;
    }

    SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

#endif

std::string resolve_hostname(const std::string& hostname) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    addrinfo *results = nullptr;
    const int result = getaddrinfo(hostname.c_str(), nullptr, &hints, &results);

    if (result != 0 || results == nullptr) {
        std::println("{}[DNS] Failed to resolve '{}'; using it unchanged.{}", kColorError, hostname, kColorReset);
        return hostname;
    }

    struct AddrInfoDeleter {
        void operator()(addrinfo *value) const noexcept {
            if (value != nullptr) {
                freeaddrinfo(value);
            }
        }
    };

    const std::unique_ptr<addrinfo, AddrInfoDeleter> result_guard(results);

    const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(results->ai_addr);

    std::array<char, INET_ADDRSTRLEN> address_buffer{};
    const char *converted = inet_ntop(AF_INET, &ipv4->sin_addr, address_buffer.data(), static_cast<socklen_t>(address_buffer.size()));

    if (converted == nullptr) {
        std::println("{}[DNS] Failed to convert resolved address for '{}'; "
                     "using it unchanged.{}",
                     kColorError, hostname, kColorReset);
        return hostname;
    }

    std::string resolved_address(converted);

    std::println("{}[DNS] Resolved {} -> {}{}", kColorSystem, hostname, resolved_address, kColorReset);

    return resolved_address;
}

std::optional<uint16_t> parse_port(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }

    uint32_t port = 0;

    for (const char character : value) {
        if (!std::isdigit(static_cast<unsigned char>(character))) {
            return std::nullopt;
        }

        port = (port * 10U) + static_cast<uint32_t>(character - '0');
        if (port > 65'535U) {
            return std::nullopt;
        }
    }

    if (port == 0) {
        return std::nullopt;
    }

    return static_cast<uint16_t>(port);
}

} // namespace

struct ProxyConfig {
    std::string target_host;
    uint16_t target_port = 0;
    uint16_t listen_port = 0;
};

class ProxySession final : public std::enable_shared_from_this<ProxySession> {
public:
    using RequestProxyEndpointFn = std::function<void(std::string host, uint16_t port)>;

    ProxySession(std::shared_ptr<enet::EnetPeer> client_peer, ProxyConfig config, RequestProxyEndpointFn request_proxy_endpoint)
        : client_peer_(std::move(client_peer)), config_(std::move(config)), request_proxy_endpoint_(std::move(request_proxy_endpoint)) {
        enet::EnetPeerConfig peer_config;
        peer_config.crc_enabled = true;

        server_peer_ = std::make_unique<enet::EnetPeer>(peer_config);
        server_socket_ = std::make_unique<enet::UdpSocket>();
    }

    ~ProxySession() { std::println("{}[Proxy:{}] Session destroyed.{}", kColorSystem, config_.listen_port, kColorReset); }

    ProxySession(const ProxySession&) = delete;
    ProxySession& operator=(const ProxySession&) = delete;
    ProxySession(ProxySession&&) = delete;
    ProxySession& operator=(ProxySession&&) = delete;

    void start() {
        const std::weak_ptr<ProxySession> weak_self = weak_from_this();

        client_peer_->on_state_changed = [weak_self](enet::EnetConnectionState state) {
            const auto self = weak_self.lock();
            if (!self) {
                return;
            }

            if (state == enet::EnetConnectionState::Disconnected) {
                std::println("{}[Proxy:{}] Client disconnected.{}", kColorSystem, self->config_.listen_port, kColorReset);

                self->stop(false, true);
            }
        };

        client_peer_->on_payload_command = [weak_self](const enet::EnetCommand& command) {
            if (const auto self = weak_self.lock()) {
                self->handle_client_to_server(command);
            }
        };

        server_peer_->on_state_changed = [weak_self](enet::EnetConnectionState state) {
            const auto self = weak_self.lock();
            if (!self) {
                return;
            }

            if (state == enet::EnetConnectionState::Connected) {
                std::println("{}[Proxy:{}] Connected to real server.{}", kColorSystem, self->config_.listen_port, kColorReset);

                self->server_connected_ = true;
                self->flush_pending_client_queue();

                self->client_peer_->sync_local_time_to_remote_dynamic(*self->server_peer_);
                return;
            }

            if (state == enet::EnetConnectionState::Disconnected) {
                std::println("{}[Proxy:{}] Real server disconnected.{}", kColorSystem, self->config_.listen_port, kColorReset);

                self->server_connected_ = false;
                self->stop(true, false);
            }
        };

        server_peer_->on_payload_command = [weak_self](const enet::EnetCommand& command) {
            if (const auto self = weak_self.lock()) {
                self->handle_server_to_client(command);
            }
        };

        std::println("{}[Proxy:{}] Connecting to real server ({}:{})...{}", kColorSystem, config_.listen_port, config_.target_host, config_.target_port,
                     kColorReset);

        if (!server_peer_->connect(*server_socket_, config_.target_host, config_.target_port)) {
            std::println("{}[Proxy:{}] Failed to connect to target server.{}", kColorError, config_.listen_port, kColorReset);

            stop(true, true);
        }
    }

    void service() {
        if (!running_ || !server_peer_ || !server_socket_) {
            return;
        }

        receive_server_datagrams();

        if (!running_) {
            return;
        }

        server_peer_->service();

        while (server_peer_->dispatch_one()) {
            // Drain all queued events
        }
    }

    [[nodiscard]]
    bool is_alive() const noexcept {
        return running_;
    }

private:
    struct ParsedEndpoint {
        std::string scheme_prefix;
        std::string host;
        uint16_t port = 0;
    };

    static std::optional<ParsedEndpoint> parse_endpoint(std::string_view address) {
        ParsedEndpoint endpoint;
        std::string_view remaining = address;

        if (const auto scheme_position = remaining.find("://"); scheme_position != std::string_view::npos) {
            endpoint.scheme_prefix = std::string(remaining.substr(0, scheme_position + 3));

            remaining = remaining.substr(scheme_position + 3);
        }

        if (remaining.empty()) {
            return std::nullopt;
        }

        std::string_view host_part;
        std::string_view port_part;

        if (remaining.front() == '[') {
            const auto closing_bracket = remaining.find(']');
            if (closing_bracket == std::string_view::npos) {
                return std::nullopt;
            }

            host_part = remaining.substr(1, closing_bracket - 1);

            if (closing_bracket + 1 >= remaining.size() || remaining[closing_bracket + 1] != ':') {
                return std::nullopt;
            }

            port_part = remaining.substr(closing_bracket + 2);
        } else {
            const auto separator = remaining.rfind(':');
            if (separator == std::string_view::npos) {
                return std::nullopt;
            }

            host_part = remaining.substr(0, separator);
            port_part = remaining.substr(separator + 1);
        }

        if (host_part.empty()) {
            return std::nullopt;
        }

        const auto port = parse_port(port_part);
        if (!port) {
            return std::nullopt;
        }

        endpoint.host = std::string(host_part);
        endpoint.port = *port;
        return endpoint;
    }

    void rewrite_endpoint_and_auto_proxy(std::string& address) {
        const auto endpoint = parse_endpoint(address);
        if (!endpoint) {
            return;
        }

        if (request_proxy_endpoint_) {
            const std::string resolved_host = resolve_hostname(endpoint->host);

            request_proxy_endpoint_(resolved_host, endpoint->port);

            // Port translations
            switch (endpoint->port) {
            case 5055:
                request_proxy_endpoint_(resolved_host, 27001);
                break;

            case 5056:
                request_proxy_endpoint_(resolved_host, 27002);
                break;

            case 5058:
                request_proxy_endpoint_(resolved_host, 27000);
                break;

            default:
                break;
            }
        }

        address = std::format("{}127.0.0.1:{}", endpoint->scheme_prefix, endpoint->port);
    }

    void receive_server_datagrams() {
        std::array<uint8_t, kMaximumUdpDatagramSize> buffer{};

        while (running_) {
            enet::EnetEndpoint sender;
            const std::size_t length = server_socket_->recv_from(buffer.data(), buffer.size(), sender);

            if (length == 0) {
                break;
            }

            ser::ByteArray datagram(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(length));

            server_peer_->handle_incoming_datagram(datagram);
        }
    }

    void handle_client_to_server(const enet::EnetCommand& command) {
        if (!running_) {
            return;
        }

        if (!server_connected_) {
            pending_client_commands_.push_back(command);
            return;
        }

        process_packet_direction(command, true);
    }

    void flush_pending_client_queue() {
        if (!running_ || !server_connected_) {
            return;
        }

        std::vector<enet::EnetCommand> pending;
        pending.swap(pending_client_commands_);

        for (const auto& command : pending) {
            if (!running_ || !server_connected_) {
                break;
            }

            process_packet_direction(command, true);
        }
    }

    void handle_server_to_client(const enet::EnetCommand& command) {
        if (running_) {
            process_packet_direction(command, false);
        }
    }

    void process_packet_direction(const enet::EnetCommand& command, bool is_client_to_server) {
        const auto delivery_mode = enet::EnetCommandGetEnetDeliveryMode(command);

        auto& input_protocol =
            is_client_to_server ? reinterpret_cast<ser::IProtocol&>(proto_client_side_) : reinterpret_cast<ser::IProtocol&>(proto_server_side_);

        auto& output_protocol =
            is_client_to_server ? reinterpret_cast<ser::IProtocol&>(proto_server_side_) : reinterpret_cast<ser::IProtocol&>(proto_client_side_);

        const std::string_view color = is_client_to_server ? kColorClient : kColorServer;

        const std::string label = std::format("{}:{} (flags: {}, reserved: {})", config_.listen_port, is_client_to_server ? "C->S" : "S->C",
                                              static_cast<int>(command.header.flags), static_cast<int>(command.header.reserved));

        auto message_result = input_protocol.Deserialize(command.get_payload());

        if (!message_result) {
            std::println("{}== {} Msg =={}", color, label, kColorReset);

            if (!visualizer::print_http_message(command.get_payload())) {
                std::println("Message not understood: {}\nHex dump:", message_result.error().message);

                visualizer::helpers::print_hex_dump(command.get_payload(), 0);
            }

            std::println();

            forward_packet(command.get_payload(), command.header.channel_id, delivery_mode, is_client_to_server);

            return;
        }

        ser::Message message = std::move(*message_result);

        if (intercept_encryption_handshake(message, command, delivery_mode, is_client_to_server, color, label)) {
            return;
        }

        if (!is_client_to_server) {
            rewrite_response_endpoints(message);
        }

        std::println("{}== {} Msg =={}", color, label, kColorReset);

        visualizer::print_ser_message(message, 0);
        std::println();

        auto serialized = output_protocol.Serialize(message);
        if (!serialized) {
            std::println("{}Warning: Could not re-serialize packet: {}{}", kColorError, serialized.error().message, kColorReset);

            forward_packet(command.get_payload(), command.header.channel_id, delivery_mode, is_client_to_server);

            return;
        }

        forward_packet(*serialized, command.header.channel_id, delivery_mode, is_client_to_server);
    }

    bool intercept_encryption_handshake(ser::Message& message, const enet::EnetCommand& command, enet::EnetDeliveryMode delivery_mode, bool is_client_to_server,
                                        std::string_view color, const std::string& label) {
        if (is_client_to_server) {
            const auto *request = message.get_if<ser::InternalOperationRequestMessage>();

            if (request == nullptr || request->operation_code != ICodes::IOpInitEncryption) {
                return false;
            }

            std::println("{}[{}] Intercepting InitEncryption request...{}", color, label, kColorReset);

            auto client_response = proto_client_side_.HandleInitEncryptionRequest(*request);

            if (!client_response) {
                std::println("{}[{}] Failed to handle client encryption request: {}{}", kColorError, label, client_response.error().message, kColorReset);

                forward_packet(command.get_payload(), command.header.channel_id, delivery_mode, true);

                return true;
            }

            pending_client_init_enc_response_ = *client_response;

            auto server_request = proto_server_side_.CreateInitEncryptionRequest();

            if (!server_request) {
                std::println("{}[{}] Failed to create server encryption request: {}{}", kColorError, label, server_request.error().message, kColorReset);

                stop(true, true);
                return true;
            }

            enet::EnetSendOptions options;
            options.channel = command.header.channel_id;
            options.mode = enet::EnetDeliveryMode::Reliable;

            server_peer_->send_payload(*server_request, options);
            return true;
        }

        const auto *response = message.get_if<ser::InternalOperationResponseMessage>();

        if (response == nullptr || response->operation_code != ICodes::IOpInitEncryption) {
            return false;
        }

        std::println("{}[{}] Intercepting InitEncryption response...{}", color, label, kColorReset);

        auto server_result = proto_server_side_.HandleInitEncryptionResponse(*response);

        if (!server_result) {
            std::println("{}[{}] Failed to handle server encryption response: {}{}", kColorError, label, server_result.error().message, kColorReset);

            stop(true, true);
            return true;
        }

        if (!pending_client_init_enc_response_) {
            std::println("{}[{}] Received an unexpected InitEncryption response.{}", kColorError, label, kColorReset);

            stop(true, true);
            return true;
        }

        ser::Message client_response_message = *pending_client_init_enc_response_;

        auto client_response = proto_client_side_.Serialize(client_response_message);

        if (!client_response) {
            std::println("{}[{}] Failed to serialize client encryption response: {}{}", kColorError, label, client_response.error().message, kColorReset);

            stop(true, true);
            return true;
        }

        enet::EnetSendOptions options;
        options.channel = command.header.channel_id;
        options.mode = enet::EnetDeliveryMode::Reliable;

        client_peer_->send_payload(*client_response, options);
        pending_client_init_enc_response_.reset();

        std::println("{}[Proxy:{}] Encryption MitM established.{}", kColorSystem, config_.listen_port, kColorReset);

        return true;
    }

    void rewrite_response_endpoints(ser::Message& message) {
        auto *response = message.get_if<ser::OperationResponseMessage>();

        if (response == nullptr) {
            return;
        }

        try {
            auto& endpoint = response->parameters.at(kRemoteAddressParameter);

            if (auto *address = endpoint.get_ptr<std::string>()) {
                rewrite_endpoint_and_auto_proxy(*address);
            }

            if (auto *addresses = endpoint.get_ptr<std::vector<std::string>>()) {
                for (std::string& address : *addresses) {
                    rewrite_endpoint_and_auto_proxy(address);
                }
            }
        } catch (const std::exception&) {
            // The remote-address parameter is optional
        }
    }

    void forward_packet(std::span<const uint8_t> payload, uint8_t channel, enet::EnetDeliveryMode mode, bool to_server) {
        enet::EnetSendOptions options;
        options.channel = channel;
        options.mode = mode;

        if (to_server) {
            if (server_peer_) {
                server_peer_->send_payload(payload, options);
            }
            return;
        }

        if (client_peer_) {
            client_peer_->send_payload(payload, options);
        }
    }

    void stop(bool disconnect_client, bool disconnect_server) {
        if (!running_) {
            return;
        }

        running_ = false;
        server_connected_ = false;
        pending_client_commands_.clear();
        pending_client_init_enc_response_.reset();

        if (disconnect_server && server_peer_) {
            server_peer_->disconnect();
        }

        if (disconnect_client && client_peer_) {
            client_peer_->disconnect();
        }
    }

    std::shared_ptr<enet::EnetPeer> client_peer_;
    std::unique_ptr<enet::EnetPeer> server_peer_;
    std::unique_ptr<enet::UdpSocket> server_socket_;

    ProxyConfig config_;
    RequestProxyEndpointFn request_proxy_endpoint_;

    bool running_ = true;
    bool server_connected_ = false;

    std::vector<enet::EnetCommand> pending_client_commands_;

    ser::GpBinaryV18 proto_client_side_{};
    ser::GpBinaryV18 proto_server_side_{};

    std::optional<ser::InternalOperationResponseMessage> pending_client_init_enc_response_;
};

class ProxyManager final {
public:
    explicit ProxyManager(std::string target_host) : target_host_(std::move(target_host)) {
        server_config_.disconnect_timeout_ms = 10'000;
        server_config_.crc_enabled = true;
    }

    ProxyManager(const ProxyManager&) = delete;
    ProxyManager& operator=(const ProxyManager&) = delete;
    ProxyManager(ProxyManager&&) = delete;
    ProxyManager& operator=(ProxyManager&&) = delete;

    void request_listen(std::string host, uint16_t port) {
        pending_target_by_port_[port] = std::move(host);

        if (pending_ports_set_.insert(port).second) {
            pending_ports_.push_back(port);
        }
    }

    void request_listen(uint16_t port) { request_listen(target_host_, port); }

    void process_pending_listeners() {
        std::vector<uint16_t> ports;
        std::unordered_map<uint16_t, std::string> targets;

        ports.swap(pending_ports_);
        targets.swap(pending_target_by_port_);
        pending_ports_set_.clear();

        for (const uint16_t port : ports) {
            const auto target = targets.find(port);

            std::string host = target != targets.end() ? std::move(target->second) : target_host_;

            add_or_update_listener(port, std::move(host));
        }
    }

    void service_listeners() {
        for (auto& [port, listener] : listeners_) {
            static_cast<void>(port);
            listener->service();
        }
    }

    void service_sessions() {
        for (auto iterator = sessions_.begin(); iterator != sessions_.end();) {
            (*iterator)->service();

            if (!(*iterator)->is_alive()) {
                iterator = sessions_.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

    [[nodiscard]]
    bool has_any_listeners() const noexcept {
        return !listeners_.empty();
    }

private:
    struct TargetEndpoint {
        std::string host;
        uint16_t port = 0;
    };

    void add_or_update_listener(uint16_t port, std::string host) {
        target_by_listen_port_[port] = TargetEndpoint{std::move(host), port};

        if (listening_ports_.contains(port)) {
            const auto& target = target_by_listen_port_.at(port);

            std::println("{}[Proxy:{}] Updated target mapping -> {}:{}{}", kColorSystem, port, target.host, target.port, kColorReset);

            return;
        }

        auto listener = std::make_unique<enet::EnetServer>(server_config_);

        listener->on_peer_connected = [this, port](std::shared_ptr<enet::EnetPeer> client_peer) {
            std::println("{}[Proxy:{}] New client connection. ID: {}{}", kColorSystem, port, client_peer->peer_id(), kColorReset);

            const auto target = target_by_listen_port_.find(port);

            const TargetEndpoint destination = target != target_by_listen_port_.end() ? target->second : TargetEndpoint{target_host_, port};

            ProxyConfig config;
            config.target_host = destination.host;
            config.target_port = destination.port;
            config.listen_port = port;

            auto session = std::make_shared<ProxySession>(std::move(client_peer), std::move(config), [this](std::string endpoint_host, uint16_t endpoint_port) {
                request_listen(std::move(endpoint_host), endpoint_port);
            });

            session->start();
            sessions_.push_back(std::move(session));
        };

        if (!listener->bind(port)) {
            std::println("{}Failed to bind listen port {} "
                         "(the port may already be in use).{}",
                         kColorError, port, kColorReset);
            return;
        }

        const auto& target = target_by_listen_port_.at(port);

        std::println("{}Photon proxy listening on port {} -> {}:{}{}", kColorSystem, port, target.host, target.port, kColorReset);

        listening_ports_.insert(port);
        listeners_.emplace(port, std::move(listener));
    }

    std::string target_host_;
    enet::EnetPeerConfig server_config_{};

    std::unordered_map<uint16_t, std::unique_ptr<enet::EnetServer>> listeners_;

    std::unordered_set<uint16_t> listening_ports_;
    std::vector<std::shared_ptr<ProxySession>> sessions_;

    std::unordered_map<uint16_t, TargetEndpoint> target_by_listen_port_;

    std::vector<uint16_t> pending_ports_;
    std::unordered_set<uint16_t> pending_ports_set_;
    std::unordered_map<uint16_t, std::string> pending_target_by_port_;
};

int main(int argc, char *argv[]) {
    try {
#ifdef _WIN32
        WinsockRuntime winsock;
        enable_windows_ansi_colors();
#endif

        if (argc < 3) {
            std::println("Usage: {} <target_host> <port1> "
                         "[port2] [port3] ...",
                         argv[0]);
            return 1;
        }

        const std::string target_host = resolve_hostname(argv[1]);

        std::vector<uint16_t> ports;
        ports.reserve(static_cast<std::size_t>(argc - 2));

        for (int index = 2; index < argc; ++index) {
            const auto port = parse_port(argv[index]);
            if (!port) {
                std::println("{}Invalid port number: {}{}", kColorError, argv[index], kColorReset);
                return 1;
            }

            ports.push_back(*port);
        }

        std::signal(SIGINT, signal_handler_int);

        ProxyManager manager(target_host);

        for (const uint16_t port : ports) {
            manager.request_listen(port);
        }

        manager.process_pending_listeners();

        if (!manager.has_any_listeners()) {
            std::println("{}No listeners were created. Exiting.{}", kColorError, kColorReset);
            return 1;
        }

        while (g_running != 0) {
            manager.process_pending_listeners();
            manager.service_listeners();
            manager.service_sessions();

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::println("{}Shutting down...{}", kColorSystem, kColorReset);

        return 0;
    } catch (const std::exception& exception) {
        std::println("{}Fatal error: {}{}", kColorError, exception.what(), kColorReset);
        return 1;
    } catch (...) {
        std::println("{}Fatal error: unknown exception.{}", kColorError, kColorReset);
        return 1;
    }
}
