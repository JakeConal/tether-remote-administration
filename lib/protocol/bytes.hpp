// lib/protocol/bytes.hpp - the transport seam for protocol v2 (Phase 3
// direction: NO sockets yet). The codec and the session state machine consume
// and produce bytes exclusively through these two interfaces; Phase 4's TCP
// transport will be the next IByteSource/IByteSink pair behind the same seam.
// All Phase 3 tests drive them through in-memory pipes (no blocking reads).
#pragma once

#include <cstddef>
#include <deque>
#include <stdexcept>
#include <vector>

#include "envelope.hpp"

namespace tether::protocol {

// One read attempt over the wire-bearing direction. Both fields act together:
// - count == 0, end_of_stream == false: no bytes available right now
//   (non-blocking pipe or an idle TCP socket).
// - count > 0,  end_of_stream == false: bytes are available.
// - end_of_stream == true: write side is closed and fully drained (EOF); any
//   `count` accompanies the tail bytes.
struct ReadResult {
    std::size_t count = 0;
    bool end_of_stream = false;
};

class IByteSource {
public:
    virtual ~IByteSource() = default;

    // Never blocks: returns at most `max` bytes.
    virtual ReadResult read(unsigned char* dst, std::size_t max) = 0;
};

class IByteSink {
public:
    virtual ~IByteSink() = default;

    // Writes a run of bytes atomically from the sender's perspective.
    virtual void write(const unsigned char* bytes, std::size_t count) = 0;
};

// Single-reader / single-writer in-memory pipe implementing both interfaces:
// one peer writes frames in (sink side), the other drains them out (source
// side). Not thread-safe by design - Phase 3 runs everything on the test
// thread against an injectable clock; no real waits anywhere.
class InMemoryPipe : public IByteSource, public IByteSink {
public:
    ReadResult read(unsigned char* dst, std::size_t max) override {
        if (dst == nullptr && max != 0) {
            throw ProtocolError(ProtocolError::Code::BadArgument, "pipe read into null buffer");
        }
        const std::size_t take = max < buffer_.size() ? max : buffer_.size();
        for (std::size_t i = 0; i < take; ++i) {
            dst[i] = buffer_.front();
            buffer_.pop_front();
        }
        return ReadResult{take, closed_ && buffer_.empty()};
    }

    void write(const unsigned char* bytes, std::size_t count) override {
        if (closed_) {
            throw ProtocolError(ProtocolError::Code::BadArgument, "write into a closed pipe");
        }
        buffer_.insert(buffer_.end(), bytes, bytes + count);
    }

    // Close the write side: readers drain what is buffered, then see EOF.
    void close_writer() { closed_ = true; }

private:
    std::deque<unsigned char> buffer_;
    bool closed_ = false;
};

// A connected encoder/decoder test wire:
//   console_session reads from to_console, writes into to_agent
//   agent_session   reads from to_agent,   writes into to_console
struct InMemoryConnection {
    InMemoryPipe to_console;
    InMemoryPipe to_agent;
};

} // namespace tether::protocol
