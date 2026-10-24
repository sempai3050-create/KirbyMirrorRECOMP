// replay_writer.cpp — see replay_writer.h.

#include "replay_writer.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <thread>

#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
#define GBARECOMP_REPLAY_WRITER_THREADS 0
#else
#define GBARECOMP_REPLAY_WRITER_THREADS 1
#endif

namespace gbarecomp::debug {

namespace {

constexpr std::uint64_t kP1 = 11400714785074694791ull;
constexpr std::uint64_t kP2 = 14029467366897019727ull;
constexpr std::uint64_t kP3 = 1609587929392839161ull;

inline std::uint64_t rotl(std::uint64_t v, unsigned r) {
    return (v << r) | (v >> (64u - r));
}
inline std::uint64_t mix(std::uint64_t acc, std::uint64_t lane) {
    acc += lane * kP2;
    acc = rotl(acc, 31);
    return acc * kP1;
}
inline std::uint64_t load64(const std::uint8_t* p) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return v;  // GBA hosts are little-endian; the hash is host-local anyway.
}

enum Op : std::uint32_t { BEGIN = 1, SNAPSHOT, LINE, FRAME, REMOVE, END };
enum StatusCode : int { OK = 0, FULL = 1, INVALID = 2, IO = 3 };

struct Command {
    std::uint32_t op = 0;
    std::uint32_t count = 0;   // FRAME parts
    std::uint64_t size = 0;    // payload bytes following this header
    std::uint64_t frame = 0;
    std::uint32_t kind = 0;    // SNAPSHOT kind character
    std::uint64_t sizes[ReplayWriter::kMaxParts] = {};
    std::uint32_t reuse_mask = 0;
};

constexpr std::size_t kFileBuffer = 64u * 1024u;

}  // namespace

void ReplayHash::update(const void* data, std::size_t size) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    length += size;
    if (tail_len) {
        while (tail_len < 8 && size) { tail[tail_len++] = *p++; --size; }
        if (tail_len < 8) return;
        state = mix(state, load64(tail));
        tail_len = 0;
    }
    while (size >= 8) {
        state = mix(state, load64(p));
        p += 8;
        size -= 8;
    }
    while (size) { tail[tail_len++] = *p++; --size; }
}

std::uint64_t ReplayHash::finish() const {
    std::uint64_t h = state ^ (length * kP1);
    for (unsigned i = 0; i < tail_len; ++i)
        h = rotl(h ^ (tail[i] * kP3), 11) * kP1;
    h ^= h >> 33; h *= kP2;
    h ^= h >> 29; h *= kP3;
    h ^= h >> 32;
    return h;
}

std::uint64_t replay_hash(const void* data, std::size_t size) {
    ReplayHash h;
    h.update(data, size);
    return h.finish();
}

struct ReplayWriter::Impl {
    std::vector<std::uint8_t> ring = std::vector<std::uint8_t>(kQueueBytes);
    std::atomic<std::size_t> read_pos{0};
    std::atomic<std::size_t> write_pos{0};
    std::atomic<int> error{OK};
    std::atomic<bool> full{false};
    std::atomic<bool> stop{false};
    std::size_t head = 0;  // producer only
#if GBARECOMP_REPLAY_WRITER_THREADS
    std::thread thread;
#endif
    bool joined = false;

    // ── producer ──────────────────────────────────────────────────────
    std::size_t free_space() const {
        const std::size_t tail = read_pos.load(std::memory_order_acquire);
        return (tail + kQueueBytes - head - 1) % kQueueBytes;
    }
    void put(std::size_t at, const void* data, std::size_t n) {
        if (!n) return;
        const std::size_t first = std::min(n, kQueueBytes - at);
        std::memcpy(ring.data() + at, data, first);
        if (n > first)
            std::memcpy(ring.data(),
                        static_cast<const std::uint8_t*>(data) + first, n - first);
    }
    void fail(int code) {
        int expected = OK;
        error.compare_exchange_strong(expected, code);
    }
    bool enqueue(const Command& c, const void* const* data,
                 const std::size_t* sizes, unsigned parts) {
        if (stop.load(std::memory_order_relaxed) ||
            error.load(std::memory_order_relaxed) != OK)
            return false;
        const std::size_t need = sizeof(Command) + c.size;
        if (need >= kQueueBytes) { fail(INVALID); return false; }
        if (need > free_space()) {
            full.store(true, std::memory_order_relaxed);
            return false;
        }
        std::size_t at = head;
        put(at, &c, sizeof(c));
        at = (at + sizeof(c)) % kQueueBytes;
        for (unsigned i = 0; i < parts; ++i) {
            if (sizes[i] && !data[i]) { fail(INVALID); return false; }
            put(at, data[i], sizes[i]);
            at = (at + sizes[i]) % kQueueBytes;
        }
        head = at;
        full.store(false, std::memory_order_relaxed);
        // Release publishes every payload byte before the worker sees head.
        write_pos.store(at, std::memory_order_release);
        return true;
    }

    // ── worker ────────────────────────────────────────────────────────
    void get(std::size_t at, void* data, std::size_t n) const {
        if (!n) return;
        const std::size_t first = std::min(n, kQueueBytes - at);
        std::memcpy(data, ring.data() + at, first);
        if (n > first)
            std::memcpy(static_cast<std::uint8_t*>(data) + first, ring.data(),
                        n - first);
    }
    void hash_ring(ReplayHash& h, std::size_t at, std::size_t n) const {
        const std::size_t first = std::min(n, kQueueBytes - at);
        h.update(ring.data() + at, first);
        if (n > first) h.update(ring.data(), n - first);
    }
    bool write_ring(std::FILE* f, std::size_t at, std::size_t n) const {
        if (!f) return false;
        const std::size_t first = std::min(n, kQueueBytes - at);
        if (first && std::fwrite(ring.data() + at, 1, first, f) != first)
            return false;
        return n == first ||
               std::fwrite(ring.data(), 1, n - first, f) == n - first;
    }
    static std::string take_string(const std::vector<std::uint8_t>& blob,
                                   std::size_t& at, bool& ok) {
        std::uint32_t n = 0;
        if (at + 4 > blob.size()) { ok = false; return {}; }
        std::memcpy(&n, blob.data() + at, 4);
        at += 4;
        if (at + n > blob.size()) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(blob.data() + at), n);
        at += n;
        return s;
    }
    static bool copy_optional(const std::string& source, const std::string& dest) {
        std::FILE* in = std::fopen(source.c_str(), "rb");
        if (!in) return errno == ENOENT;
        std::FILE* out = std::fopen(dest.c_str(), "wb");
        bool ok = out != nullptr;
        char bytes[8192];
        std::size_t n;
        while (ok && (n = std::fread(bytes, 1, sizeof(bytes), in)) != 0)
            ok = std::fwrite(bytes, 1, n, out) == n;
        if (std::ferror(in)) ok = false;
        if (std::fclose(in)) ok = false;
        if (out && std::fclose(out)) ok = false;
        return ok;
    }
    static bool close_files(std::FILE*& text, std::FILE*& states) {
        bool ok = true;
        if (states && std::fclose(states)) ok = false;
        if (text && std::fclose(text)) ok = false;
        text = states = nullptr;
        return ok;
    }

    void run() {
        std::FILE* text = nullptr;
        std::FILE* states = nullptr;
        std::vector<char> text_buffer(kFileBuffer), state_buffer(kFileBuffer);
        std::size_t tail = 0;
        std::uint64_t offset = 0;
        std::uint64_t last_hash[ReplayWriter::kMaxParts] = {};
        auto last_flush = std::chrono::steady_clock::now();
        bool dirty = false;
        // Test-only slow storage (acceptance of back-pressure recovery):
        // GBARECOMP_DIAGNOSTICS_TEST_WRITER_DELAY_US delays every frame record.
        long frame_delay_us = 0;
        if (const char* e = std::getenv("GBARECOMP_DIAGNOSTICS_TEST_WRITER_DELAY_US"))
            frame_delay_us = std::strtol(e, nullptr, 10);
        for (;;) {
            if (write_pos.load(std::memory_order_acquire) != tail) {
                Command c;
                get(tail, &c, sizeof(c));
                std::size_t at = (tail + sizeof(c)) % kQueueBytes;
                bool ok = true;
                switch (c.op) {
                case BEGIN: {
                    std::vector<std::uint8_t> blob(static_cast<std::size_t>(c.size));
                    get(at, blob.data(), blob.size());
                    std::size_t cursor = 0;
                    const std::string path = take_string(blob, cursor, ok);
                    const std::string header = take_string(blob, cursor, ok);
                    ok = close_files(text, states) && ok;
                    offset = 0;
                    std::memset(last_hash, 0, sizeof(last_hash));
                    if (!ok) break;
                    text = std::fopen(path.c_str(), "wb");
                    states = std::fopen((path + ".state").c_str(), "wb");
                    ok = text && states &&
                         !std::setvbuf(text, text_buffer.data(), _IOFBF, kFileBuffer) &&
                         !std::setvbuf(states, state_buffer.data(), _IOFBF, kFileBuffer) &&
                         std::fputs(header.c_str(), text) != EOF;
                    std::uint32_t copies = 0, inlines = 0;
                    if (ok && cursor + 8 <= blob.size()) {
                        std::memcpy(&copies, blob.data() + cursor, 4);
                        std::memcpy(&inlines, blob.data() + cursor + 4, 4);
                        cursor += 8;
                    } else {
                        ok = false;
                    }
                    for (std::uint32_t i = 0; ok && i < copies; ++i) {
                        const std::string src = take_string(blob, cursor, ok);
                        const std::string dst = take_string(blob, cursor, ok);
                        ok = ok && copy_optional(src, dst);
                    }
                    for (std::uint32_t i = 0; ok && i < inlines; ++i) {
                        const std::string body = take_string(blob, cursor, ok);
                        const std::string dst = take_string(blob, cursor, ok);
                        std::FILE* f = ok ? std::fopen(dst.c_str(), "wb") : nullptr;
                        ok = f && std::fwrite(body.data(), 1, body.size(), f) == body.size();
                        if (f && std::fclose(f)) ok = false;
                    }
                    break;
                }
                case SNAPSHOT: {
                    ok = write_ring(states, at, static_cast<std::size_t>(c.size));
                    ReplayHash h;
                    hash_ring(h, at, static_cast<std::size_t>(c.size));
                    ok = ok && text &&
                         std::fprintf(text, "S %llu %llu %016llx %c\n",
                                      static_cast<unsigned long long>(offset),
                                      static_cast<unsigned long long>(c.size),
                                      static_cast<unsigned long long>(h.finish()),
                                      static_cast<char>(c.kind)) >= 0;
                    offset += c.size;
                    break;
                }
                case LINE:
                    ok = write_ring(text, at, static_cast<std::size_t>(c.size));
                    break;
                case FRAME: {
                    if (frame_delay_us > 0)
                        std::this_thread::sleep_for(std::chrono::microseconds(frame_delay_us));
                    ok = text && std::fprintf(text, "H %llu",
                        static_cast<unsigned long long>(c.frame)) >= 0;
                    for (std::uint32_t i = 0; ok && i < c.count; ++i) {
                        if (!(c.reuse_mask & (1u << i))) {
                            ReplayHash h;
                            hash_ring(h, at, static_cast<std::size_t>(c.sizes[i]));
                            at = (at + c.sizes[i]) % kQueueBytes;
                            last_hash[i] = h.finish();
                        }
                        ok = std::fprintf(text, " %016llx",
                            static_cast<unsigned long long>(last_hash[i])) >= 0;
                    }
                    ok = ok && std::fputc('\n', text) != EOF;
                    break;
                }
                case REMOVE: {
                    std::string path(static_cast<std::size_t>(c.size), '\0');
                    get(at, path.data(), path.size());
                    ok = std::remove(path.c_str()) == 0 || errno == ENOENT;
                    break;
                }
                case END:
                    ok = close_files(text, states);
                    break;
                default:
                    ok = false;
                    break;
                }
                if (!ok) { fail(IO); break; }
                // The payload stays owned until disk I/O and hashing finish;
                // the producer refuses work at capacity rather than waiting.
                tail = (tail + sizeof(c) + static_cast<std::size_t>(c.size)) % kQueueBytes;
                read_pos.store(tail, std::memory_order_release);
                dirty = text != nullptr;
            } else {
                if (stop.load(std::memory_order_acquire)) {
                    if (write_pos.load(std::memory_order_acquire) == tail) break;
                    continue;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            const auto now = std::chrono::steady_clock::now();
            if (dirty && now - last_flush >= std::chrono::milliseconds(100)) {
                // Checkpoints reach the OS before the records that cite them.
                if ((states && std::fflush(states)) || (text && std::fflush(text))) {
                    fail(IO);
                    break;
                }
                last_flush = now;
                dirty = false;
            }
        }
        if (!close_files(text, states)) fail(IO);
    }
};

ReplayWriter::ReplayWriter() : impl_(new Impl) {}

std::unique_ptr<ReplayWriter> ReplayWriter::create() {
#if GBARECOMP_REPLAY_WRITER_THREADS
    std::unique_ptr<ReplayWriter> w(new ReplayWriter);
    try {
        Impl* impl = w->impl_.get();
        impl->thread = std::thread([impl] { impl->run(); });
    } catch (const std::system_error&) {
        return nullptr;
    }
    return w;
#else
    return nullptr;
#endif
}

ReplayWriter::~ReplayWriter() { shutdown(); }

namespace {
void append_string(std::vector<std::uint8_t>& blob, const std::string& s) {
    const auto n = static_cast<std::uint32_t>(s.size());
    const auto at = blob.size();
    blob.resize(at + 4 + s.size());
    std::memcpy(blob.data() + at, &n, 4);
    if (!s.empty()) std::memcpy(blob.data() + at + 4, s.data(), s.size());
}
}  // namespace

bool ReplayWriter::begin(const std::string& path, const std::string& header,
                         const std::vector<Sidecar>& sidecars,
                         const std::vector<Sidecar>& inline_texts) {
    std::vector<std::uint8_t> blob;
    append_string(blob, path);
    append_string(blob, header);
    const auto copies = static_cast<std::uint32_t>(sidecars.size());
    const auto inlines = static_cast<std::uint32_t>(inline_texts.size());
    const auto at = blob.size();
    blob.resize(at + 8);
    std::memcpy(blob.data() + at, &copies, 4);
    std::memcpy(blob.data() + at + 4, &inlines, 4);
    for (const auto& s : sidecars) { append_string(blob, s.source); append_string(blob, s.dest); }
    // Inline texts reuse Sidecar: source holds the body, dest the file.
    for (const auto& s : inline_texts) { append_string(blob, s.source); append_string(blob, s.dest); }
    Command c;
    c.op = BEGIN;
    c.size = blob.size();
    const void* data[] = {blob.data()};
    const std::size_t sizes[] = {blob.size()};
    return impl_->enqueue(c, data, sizes, 1);
}

bool ReplayWriter::snapshot(const void* data, std::size_t size, char kind) {
    Command c;
    c.op = SNAPSHOT;
    c.size = size;
    c.kind = static_cast<unsigned char>(kind);
    const void* parts[] = {data};
    const std::size_t sizes[] = {size};
    return impl_->enqueue(c, parts, sizes, 1);
}

bool ReplayWriter::line(const std::string& text) {
    Command c;
    c.op = LINE;
    c.size = text.size();
    const void* parts[] = {text.data()};
    const std::size_t sizes[] = {text.size()};
    return impl_->enqueue(c, parts, sizes, 1);
}

bool ReplayWriter::frame(std::uint64_t frame, const Part* parts, unsigned count) {
    if (count > kMaxParts) { impl_->fail(INVALID); return false; }
    Command c;
    c.op = FRAME;
    c.frame = frame;
    c.count = count;
    const void* data[kMaxParts] = {};
    std::size_t sizes[kMaxParts] = {};
    unsigned payload_parts = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (parts[i].reuse) {
            c.reuse_mask |= 1u << i;
            continue;
        }
        c.sizes[i] = parts[i].size;
        c.size += parts[i].size;
        data[payload_parts] = parts[i].data;
        sizes[payload_parts] = parts[i].size;
        ++payload_parts;
    }
    return impl_->enqueue(c, data, sizes, payload_parts);
}

bool ReplayWriter::remove(const std::string& path) {
    Command c;
    c.op = REMOVE;
    c.size = path.size();
    const void* parts[] = {path.data()};
    const std::size_t sizes[] = {path.size()};
    return impl_->enqueue(c, parts, sizes, 1);
}

bool ReplayWriter::end() {
    Command c;
    c.op = END;
    return impl_->enqueue(c, nullptr, nullptr, 0);
}

ReplayWriter::Status ReplayWriter::status() const {
    switch (impl_->error.load(std::memory_order_relaxed)) {
        case INVALID: return Status::Invalid;
        case IO: return Status::Io;
        default:
            return impl_->full.load(std::memory_order_relaxed) ? Status::Full
                                                                : Status::Ok;
    }
}

const char* ReplayWriter::error() const {
    switch (status()) {
        case Status::Full: return "replay disk writer fell behind; bounded queue full";
        case Status::Invalid: return "invalid or oversized replay write";
        case Status::Io: return "replay file write failed";
        default: return nullptr;
    }
}

std::size_t ReplayWriter::free_bytes() const { return impl_->free_space(); }

const char* ReplayWriter::shutdown() {
    if (!impl_->joined) {
        impl_->stop.store(true, std::memory_order_release);
#if GBARECOMP_REPLAY_WRITER_THREADS
        if (impl_->thread.joinable()) impl_->thread.join();
#endif
        impl_->joined = true;
    }
    const int e = impl_->error.load(std::memory_order_relaxed);
    if (e == INVALID) return "invalid or oversized replay write";
    if (e == IO) return "replay file write failed";
    return nullptr;
}

}  // namespace gbarecomp::debug
