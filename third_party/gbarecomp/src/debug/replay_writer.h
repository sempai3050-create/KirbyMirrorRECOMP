// replay_writer.h — bounded background writer for diagnostic replays.
//
// One simulation producer, one disk worker. Every producer call only copies
// into a fixed single-producer/single-consumer byte ring; it never waits on
// disk, hashing or the worker. The worker owns all files, copies sidecars,
// hashes immutable copies of completed frames and flushes every 100 ms, so a
// hung or crashed guest still leaves its last accepted input on disk.
//
// Back-pressure is explicit: when the ring cannot take a record the call
// returns false and status() reports Full until the worker drains; the caller
// abandons the current segment and opens a new one (beginning with a machine
// checkpoint) once space returns. Invalid arguments and I/O failures are
// sticky: the writer stops accepting work and error() explains why.
//
// Hosts without threads (single-threaded wasm) get no writer: create()
// returns null and diagnostic capture is simply unavailable there.
//
// Ported from snesrecomp runner/src/diagnostics/replay_writer.c (the bounded
// writer MegaManXRecomp's diagnostic replays use), extended with per-region
// 64-bit hashes and recoverable back-pressure.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gbarecomp::debug {

// Streaming 64-bit hash shared by the recorder's worker and playback, so both
// sides fingerprint identical bytes identically. Word-at-a-time multiply/rotate
// mixing (xxHash64 round), fast enough to fingerprint ~0.5 MB per frame.
struct ReplayHash {
    std::uint64_t state = 0x9E3779B185EBCA87ull;
    std::uint64_t length = 0;
    std::uint8_t tail[8] = {};
    unsigned tail_len = 0;
    void update(const void* data, std::size_t size);
    std::uint64_t finish() const;
};
std::uint64_t replay_hash(const void* data, std::size_t size);

class ReplayWriter {
public:
    static constexpr std::size_t kQueueBytes = 32u * 1024u * 1024u;
    static constexpr unsigned kMaxParts = 12;

    enum class Status { Ok, Full, Invalid, Io };

    // A FRAME part: one fingerprinted region. A part with reuse=true carries
    // no bytes and asks the worker to repeat that part's previous hash (an
    // unchanged save memory, for example).
    struct Part {
        const void* data = nullptr;
        std::size_t size = 0;
        bool reuse = false;
    };
    struct Sidecar {
        std::string source;  // copied if it exists; absence is not an error
        std::string dest;
    };

    // Null when the host cannot run a worker thread.
    static std::unique_ptr<ReplayWriter> create();
    ~ReplayWriter();
    ReplayWriter(const ReplayWriter&) = delete;
    ReplayWriter& operator=(const ReplayWriter&) = delete;

    // Opens <path> (text records) and <path>.state (checkpoint blobs), writes
    // `header`, copies sidecars and writes each inline text file.
    bool begin(const std::string& path, const std::string& header,
               const std::vector<Sidecar>& sidecars,
               const std::vector<Sidecar>& inline_texts);
    // Appends a checkpoint blob and its "S <offset> <size> <hash> <kind>" line.
    bool snapshot(const void* data, std::size_t size, char kind);
    bool line(const std::string& text);
    // Emits "H <frame> <h0> ... <hN-1>" once the worker hashed the parts.
    bool frame(std::uint64_t frame, const Part* parts, unsigned count);
    bool remove(const std::string& path);
    bool end();

    Status status() const;
    const char* error() const;
    // Bytes the ring can accept right now (producer view).
    std::size_t free_bytes() const;
    // Drain every accepted record, then stop and join the worker. Idempotent.
    // Returns the sticky error text, or null.
    const char* shutdown();

private:
    ReplayWriter();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gbarecomp::debug
