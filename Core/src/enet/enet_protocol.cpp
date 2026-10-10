// Copyright (c) 2026, the Luxon contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "enet_protocol.hpp"
#include "debugbreak.h"

#include <bit>
#include <cstring>
#include <algorithm>
#include <stdexcept>

namespace luxon {
namespace enet {
DatagramBufferPool& DatagramBufferPool::instance() {
    static DatagramBufferPool pool;
    return pool;
}

DatagramBufferPool::PooledPtr DatagramBufferPool::acquire() {
    if (!free_.empty()) {
        DatagramBuffer *ptr = free_.back().release();
        free_.pop_back();
        return PooledPtr(ptr, Deleter{this});
    }

    return PooledPtr(new DatagramBuffer{}, Deleter{this});
}

void DatagramBufferPool::release(DatagramBuffer *ptr) noexcept {
    if (free_.size() < 1024)
        free_.emplace_back(ptr);
    else
        delete ptr;
}

EnetCommand::HeapBuffer& EnetCommand::HeapBuffer::operator=(const HeapBuffer& other) {
    if (this != &other) {
        size = other.size;

        if (other.data) {
            if (!data)
                data = DatagramBufferPool::instance().acquire();
            std::copy(other.data->begin(), other.data->begin() + size, data->begin());
        } else {
            data.reset();
        }
    }
    return *this;
}

EnetCommand::HeapBuffer& EnetCommand::HeapBuffer::operator=(HeapBuffer&& other) noexcept {
    if (this != &other) {
        data = std::move(other.data);
        size = other.size;
        other.size = 0;
    }
    return *this;
}

std::span<uint8_t> EnetCommand::get_payload() {
    if (auto *dyn = std::get_if<ByteArray>(&payload_))
        return *dyn;
    if (auto *stat = std::get_if<HeapBuffer>(&payload_)) {
        if (!stat->data)
            return {};
        return {stat->data->begin(), stat->data->begin() + stat->size};
    }
    return {};
}

std::span<const uint8_t> EnetCommand::get_payload() const {
    if (auto *dyn = std::get_if<ByteArray>(&payload_))
        return *dyn;
    if (auto *stat = std::get_if<HeapBuffer>(&payload_)) {
        if (!stat->data)
            return {};
        return {stat->data->cbegin(), stat->data->cbegin() + stat->size};
    }
    return {};
}

size_t EnetCommand::get_payload_size() const {
    if (auto *dyn = std::get_if<ByteArray>(&payload_))
        return dyn->size();
    if (auto *stat = std::get_if<HeapBuffer>(&payload_))
        return stat->size;
    return 0;
}

bool EnetCommand::is_payload_empty() const { return get_payload_size() == 0; }

void EnetCommand::set_payload(DatagramView payload) {
    if (payload.size() > std::tuple_size_v<DatagramBuffer>) {
        set_payload(ByteArray(payload.begin(), payload.end()));
        return;
    }

    auto buffer = DatagramBufferPool::instance().acquire();
    std::copy(payload.begin(), payload.end(), buffer->begin());
    payload_ = HeapBuffer{std::move(buffer), payload.size()};
}

namespace {
inline uint16_t read_u16_be(const uint8_t *p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    if constexpr (std::endian::native == std::endian::little)
        v = std::byteswap(v);
    return v;
}

inline int16_t read_i16_be(const uint8_t *p) { return static_cast<int16_t>(read_u16_be(p)); }

inline uint32_t read_u32_be(const uint8_t *p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    if constexpr (std::endian::native == std::endian::little)
        v = std::byteswap(v);
    return v;
}

template <bool unlocked = false> inline void write_u8(std::span<uint8_t> out, unsigned& position, uint8_t v) {
    if constexpr (!unlocked)
        if (position > out.size() - 1)
            throw std::out_of_range("write_u8: Data write out of range");
    out[position++] = v;
}

template <bool unlocked = false> inline void write_u16_be(std::span<uint8_t> out, unsigned& position, uint16_t v) {
    if constexpr (!unlocked)
        if (position > out.size() - 2)
            throw std::out_of_range("write_u16_be: Data write out of range");

    if constexpr (std::endian::native == std::endian::little)
        v = std::byteswap(v);
    const uint8_t *p = reinterpret_cast<const uint8_t *>(&v);

    out[position++] = p[0];
    out[position++] = p[1];
}

template <bool unlocked = false> inline void write_i16_be(std::span<uint8_t> out, unsigned& position, int16_t v) {
    write_u16_be<unlocked>(out, position, static_cast<uint16_t>(v));
}

template <bool unlocked = false> inline void write_u32_be(std::span<uint8_t> out, unsigned& position, uint32_t v) {
    if constexpr (!unlocked)
        if (position > out.size() - 4)
            throw std::out_of_range("write_u32_be: Data write out of range");

    if constexpr (std::endian::native == std::endian::little)
        v = std::byteswap(v);
    const uint8_t *p = reinterpret_cast<const uint8_t *>(&v);

    out[position++] = p[0];
    out[position++] = p[1];
    out[position++] = p[2];
    out[position++] = p[3];
}

inline void ensure_available(size_t need, size_t have, const char *msg) {
    if (have < need)
        throw ProtocolError(msg);
}

inline const uint32_t *crc_table_ptr() {
    static constexpr auto table = [] {
        std::array<uint32_t, 256> t{};
        constexpr uint32_t poly = 0xEDB88320u;

        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j) {
                c = (c & 1) ? ((c >> 1) ^ poly) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();

    return table.data();
}
} // namespace

inline uint32_t update_crc(uint32_t crc, std::span<const uint8_t> data) {
    const uint32_t *tbl = crc_table_ptr();
    for (uint8_t byte : data)
        crc = (crc >> 8) ^ tbl[byte ^ (crc & 0xFF)];
    return crc;
}

inline uint32_t update_crc_zeros(uint32_t crc, size_t count) {
    const uint32_t *tbl = crc_table_ptr();
    while (count--)
        crc = (crc >> 8) ^ tbl[crc & 0xFF];
    return crc;
}

uint32_t calculate_crc(const uint8_t *data, size_t length) { return update_crc(0xFFFFFFFFu, {data, length}); }

uint32_t calculate_crc(std::span<const DatagramView> buffers) {
    uint32_t crc = 0xFFFFFFFFu;
    for (const auto& buf : buffers)
        crc = update_crc(crc, buf);
    return crc;
}

uint32_t calculate_crc(const uint8_t *data, size_t length, size_t zero_start, size_t zero_end) {
    // Clamp bounds
    zero_start = std::min(zero_start, length);
    zero_end = std::clamp(zero_end, zero_start, length);

    uint32_t crc = 0xFFFFFFFFu;
    // Before zero range
    crc = update_crc(crc, {data, zero_start});
    // Zeroed range
    crc = update_crc_zeros(crc, zero_end - zero_start);
    // After zero range
    crc = update_crc(crc, {data + zero_end, length - zero_end});

    return crc;
}

EnetCommand parse_command(const uint8_t *data, size_t data_len, size_t& inout_offset) {
    ensure_available(inout_offset + kCmdHeaderSize, data_len, "ENet: not enough for command header");

    EnetCommand cmd;
    cmd.header.command_type = static_cast<EnetCommandType>(data[inout_offset + 0]);
    cmd.header.channel_id = data[inout_offset + 1];
    cmd.header.flags = data[inout_offset + 2];
    cmd.header.reserved = data[inout_offset + 3];
    cmd.header.command_length = read_u32_be(data + inout_offset + 4);
    cmd.header.reliable_seq = read_u32_be(data + inout_offset + 8);

    const size_t cmd_start = inout_offset;
    const size_t cmd_len = cmd.header.command_length;

    if (cmd_len < kCmdHeaderSize)
        throw ProtocolError("ENet: invalid command length (<12)");
    if (cmd_start + cmd_len > data_len)
        throw ProtocolError("ENet: command length exceeds datagram size");

    size_t off = cmd_start + kCmdHeaderSize;

    auto remaining_in_cmd = [&]() -> size_t { return (cmd_start + cmd_len) - off; };

    switch (cmd.header.command_type) {
    case EnetCommandType::Acknowledge:
    case EnetCommandType::EgAcknowledgeUnsequenced: {
        ensure_available(8, remaining_in_cmd(), "ENet: ack missing fields");
        cmd.ack_received_reliable_sequence_number = read_u32_be(data + off);
        off += 4;
        cmd.ack_received_sent_time = read_u32_be(data + off);
        off += 4;
        break;
    }
    case EnetCommandType::SendUnreliable: {
        ensure_available(4, remaining_in_cmd(), "ENet: unreliable missing seq");
        cmd.unreliable_seq = read_u32_be(data + off);
        off += 4;
        break;
    }
    case EnetCommandType::SendUnreliableUnsequenced: {
        ensure_available(4, remaining_in_cmd(), "ENet: unsequenced missing group");
        cmd.unsequenced_group_number = read_u32_be(data + off);
        off += 4;
        break;
    }
    case EnetCommandType::SendFragment:
    case EnetCommandType::EgSendFragmentUnsequenced: {
        ensure_available(20, remaining_in_cmd(), "ENet: fragment missing fields");
        cmd.fragment_start_seq = read_u32_be(data + off);
        off += 4;
        cmd.fragment_count = read_u32_be(data + off);
        off += 4;
        cmd.fragment_number = read_u32_be(data + off);
        off += 4;
        cmd.fragment_total_length = read_u32_be(data + off);
        off += 4;
        cmd.fragment_offset = read_u32_be(data + off);
        off += 4;
        break;
    }
    default:
        // No extra fixed fields
        break;
    }

    // Remaining bytes in this command are payload
    const size_t payload_len = (cmd_start + cmd_len) - off;
    if (payload_len > 0)
        cmd.set_payload(DatagramView{data + off, data + off + payload_len});

    inout_offset = cmd_start + cmd_len;
    return cmd;
}

uint32_t compute_command_header_length(const EnetCommand& cmd) {
    uint32_t fres = static_cast<uint32_t>(kCmdHeaderSize);

    switch (cmd.header.command_type) {
    case EnetCommandType::Acknowledge:
    case EnetCommandType::EgAcknowledgeUnsequenced:
        fres += 8;
        break;

    case EnetCommandType::SendUnreliable:
        fres += 4;
        break;

    case EnetCommandType::SendUnreliableUnsequenced:
        fres += 4;
        break;

    case EnetCommandType::SendFragment:
    case EnetCommandType::EgSendFragmentUnsequenced:
        fres += 20;
        break;

    default: {
    }
    }

    return fres;
}

uint32_t compute_command_length(const EnetCommand& cmd) { return compute_command_header_length(cmd) + cmd.get_payload_size(); }

namespace {
template <bool unlocked> void write_command_header_impl(std::span<uint8_t> out, unsigned& position, const EnetCommand& cmd) {
    // Compute total length
    const uint32_t total_len = compute_command_header_length(cmd);

    write_u8<unlocked>(out, position, static_cast<uint8_t>(cmd.header.command_type));
    write_u8<unlocked>(out, position, cmd.header.channel_id);
    write_u8<unlocked>(out, position, cmd.header.flags);
    write_u8<unlocked>(out, position, cmd.header.reserved);
    write_u32_be<unlocked>(out, position, total_len + cmd.get_payload_size());
    write_u32_be<unlocked>(out, position, cmd.header.reliable_seq);

    switch (cmd.header.command_type) {
    case EnetCommandType::Acknowledge:
    case EnetCommandType::EgAcknowledgeUnsequenced:
        write_u32_be<unlocked>(out, position, cmd.ack_received_reliable_sequence_number);
        write_u32_be<unlocked>(out, position, cmd.ack_received_sent_time);
        break;

    case EnetCommandType::SendUnreliable:
        write_u32_be<unlocked>(out, position, cmd.unreliable_seq);
        break;

    case EnetCommandType::SendUnreliableUnsequenced:
        write_u32_be<unlocked>(out, position, cmd.unsequenced_group_number);
        break;

    case EnetCommandType::SendFragment:
    case EnetCommandType::EgSendFragmentUnsequenced:
        write_u32_be<unlocked>(out, position, cmd.fragment_start_seq);
        write_u32_be<unlocked>(out, position, cmd.fragment_count);
        write_u32_be<unlocked>(out, position, cmd.fragment_number);
        write_u32_be<unlocked>(out, position, cmd.fragment_total_length);
        write_u32_be<unlocked>(out, position, cmd.fragment_offset);
        break;

    default:
        break;
    }
}

void write_command_header_unlocked(std::span<uint8_t> out, unsigned& position, const EnetCommand& cmd) {
    return write_command_header_impl<true>(out, position, cmd);
}
} // namespace

void write_command_header(std::span<uint8_t> out, unsigned& position, const EnetCommand& cmd) { return write_command_header_impl<false>(out, position, cmd); }

std::vector<uint8_t> create_command_header(const EnetCommand& cmd) {
    std::vector<uint8_t> fres(compute_command_header_length(cmd));
    unsigned position{};
    write_command_header_unlocked(fres, position, cmd);
    if (position != fres.size())
        throw std::out_of_range("create_command_header: Command header does not match expected size");
    return fres;
}

size_t get_packet_command_count(DatagramView datagram) {
    if (datagram.size() < 12)
        return 0;
    return datagram[3];
}

std::vector<EnetCommand> parse_packet(DatagramView datagram, EnetPacketHeader& out_header) {
    std::vector<EnetCommand> fres(get_packet_command_count(datagram));
    const auto command_count = parse_packet(datagram, out_header, fres);
    fres.resize(command_count);
    return fres;
}

unsigned parse_packet(DatagramView datagram, EnetPacketHeader& out_header, std::span<EnetCommand> out_cmds) {
    if (datagram.size() < 12)
        throw ProtocolError("ENet: datagram too small");

    const uint8_t *p = datagram.data();
    size_t off = 0;

    out_header.peer_id = read_i16_be(p + off);
    off += 2;
    out_header.type = static_cast<EnetUdpHeaderType>(p[off++]);

    // Plain layout:
    // [0..1]=peerId, [2]=0 or 204, [3]=commandCount, [4..7]=sent_time, [8..11]=challenge, [12..15]=crc (optional)
    out_header.command_count = p[off++];
    out_header.sent_time = read_u32_be(p + off);
    off += 4;
    out_header.challenge = read_u32_be(p + off);
    off += 4;

    if (out_header.type == EnetUdpHeaderType::PlainWithCrc) {
        ensure_available(off + 4, datagram.size(), "ENet: CRC flag but no CRC field");
        uint32_t recv_crc = read_u32_be(p + off);
        out_header.crc32 = recv_crc;

        uint32_t calc = calculate_crc(datagram.data(), datagram.size(), off, off + 4);
        if (calc != recv_crc)
            throw CRCError("ENet: CRC mismatch");
        off += 4;
    } else {
        out_header.crc32.reset();
    }

    // Bounds check buffer
    if (out_header.command_count > out_cmds.size() || out_header.command_count > kMaxCmds)
        throw ProtocolError("ENet: Too many commands");

    // Write into buffer
    for (uint8_t i = 0; i < out_header.command_count; ++i)
        out_cmds[i] = parse_command(p, datagram.size(), off);

    return out_header.command_count;
}

namespace {
unsigned compute_min_fragment_count(unsigned command_count) { return 1 + command_count; }
unsigned compute_min_headers_size(unsigned command_count) { return kCmdHeaderSize + command_count * kCmdHeaderSize; }
} // namespace

unsigned compute_fragment_count(const std::vector<EnetCommand>& commands) {
    unsigned count = 1 + commands.size();
    for (const auto& c : commands)
        if (!c.get_payload().empty())
            ++count;

    return count;
}

unsigned compute_headers_size(const EnetPacketHeader& header, const std::vector<EnetCommand>& commands) {
    size_t total_header_size = kCmdHeaderSize + (header.type == EnetUdpHeaderType::PlainWithCrc ? 4 : 0);
    for (const auto& c : commands)
        total_header_size += compute_command_header_length(c);
    return total_header_size;
}

void create_packet(EnetPacketHeader header, const std::vector<EnetCommand>& commands, std::span<std::span<const uint8_t>> fragments,
                   std::span<uint8_t> headers) {
    // Quickly check for API abuse on fragments arg
    if (fragments.size() < compute_min_fragment_count(commands.size()))
        throw std::out_of_range("create_packet: API abuse: fragments span size must not be smaller than result of compute_fragment_count(commands)");

    // Quickly check for API abuse on headers arg
    if (headers.size() < compute_min_headers_size(commands.size()))
        throw std::out_of_range("create_packet: API abuse: headers span size must not be smaller than result of compute_headers_size(header, commands)");
    // Initialize positions
    unsigned headers_position{}, fragment_position{};

    // Write packet header
    write_i16_be<true>(headers, headers_position, header.peer_id);
    write_u8<true>(headers, headers_position, static_cast<uint8_t>(header.type));
    write_u8<true>(headers, headers_position, static_cast<uint8_t>(commands.size()));
    write_u32_be<true>(headers, headers_position, header.sent_time);
    write_u32_be<true>(headers, headers_position, header.challenge);

    size_t crc_pos = 0;
    if (header.type == EnetUdpHeaderType::PlainWithCrc) {
        crc_pos = headers_position;
        write_u32_be<true>(headers, headers_position, 0);
    }

    fragments[fragment_position++] = {headers.data(), headers_position};

    // Write commands
    for (const auto& c : commands) {
        // Write command header
        unsigned cmd_header_start = headers_position;
        write_command_header_unlocked(headers, headers_position, c);
        fragments[fragment_position++] = {headers.data() + cmd_header_start, headers_position - cmd_header_start};

        // Write command payload
        const auto& payload = c.get_payload();
        if (!payload.empty())
            fragments[fragment_position++] = payload;
    }

    // Calculate and write CRC
    if (crc_pos) {
        auto active_fragments = fragments.subspan(0, fragment_position);
        uint32_t crc = calculate_crc(active_fragments);

        uint32_t be = crc;
        if constexpr (std::endian::native == std::endian::little)
            be = std::byteswap(be);
        std::memcpy(headers.data() + crc_pos, &be, 4);
    }

    // Ensure bounds weren't violated
    if (fragment_position > fragments.size())
        debug_and_abort("EnetPeer::create_packet: fragments did overflow!");
    if (headers_position > headers.size())
        debug_and_abort("EnetPeer::create_packet: headers did overflow!");
}

} // namespace enet
} // namespace luxon
