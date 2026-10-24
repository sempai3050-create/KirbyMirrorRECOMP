// Diagnostic replay acceptance without a ROM: a synthetic deterministic
// machine is recorded through DiagnosticReplay (segment rollovers, a mid-run
// restore, a clean exit) and played back from the first and from a later
// segment; a perturbed machine must stop at the first divergent frame, and a
// session without its end record must report the interrupted frame.
// The bounded writer is exercised directly too (wrap-around, sticky errors,
// recoverable back-pressure).

#include "diagnostic_replay.h"
#include "replay_writer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using gbarecomp::DiagnosticFrameInput;
using gbarecomp::DiagnosticRegion;
using gbarecomp::DiagnosticReplay;
using gbarecomp::DiagnosticReplayConfig;
using gbarecomp::DiagnosticReplayHost;
using gbarecomp::debug::ReplayWriter;

namespace {

int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

// A tiny deterministic "console": 4 KiB of RAM, a CPU word, save memory.
struct Machine {
    std::vector<std::uint8_t> ram = std::vector<std::uint8_t>(4096, 0);
    std::uint32_t cpu = 1;
    std::vector<std::uint8_t> save = std::vector<std::uint8_t>(256, 0xFF);
    std::uint64_t save_seq = 0;
    std::uint64_t frame = 0;
    std::uint64_t epoch = 0;
    std::uint16_t keys = 0x03FF;
    std::uint64_t unwinds = 0;

    void run_frame() {
        for (std::size_t i = 0; i < ram.size(); ++i)
            ram[i] = static_cast<std::uint8_t>(ram[i] * 31u + (keys >> (i % 10)) + cpu + i);
        cpu = cpu * 1103515245u + 12345u + keys;
        if ((frame % 97) == 0) {
            save[frame % save.size()] ^= static_cast<std::uint8_t>(cpu);
            ++save_seq;
        }
        ++frame;
    }
    std::vector<std::uint8_t> serialize() const {
        std::vector<std::uint8_t> out(ram);
        out.insert(out.end(), save.begin(), save.end());
        for (unsigned i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(frame >> (8 * i)));
        for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(cpu >> (8 * i)));
        return out;
    }
    bool restore(const std::vector<std::uint8_t>& in) {
        if (in.size() != ram.size() + save.size() + 12) return false;
        std::memcpy(ram.data(), in.data(), ram.size());
        std::memcpy(save.data(), in.data() + ram.size(), save.size());
        const std::uint8_t* p = in.data() + ram.size() + save.size();
        frame = 0;
        for (unsigned i = 0; i < 8; ++i) frame |= static_cast<std::uint64_t>(p[i]) << (8 * i);
        cpu = 0;
        for (unsigned i = 0; i < 4; ++i) cpu |= static_cast<std::uint32_t>(p[8 + i]) << (8 * i);
        ++epoch;
        return true;
    }
    DiagnosticReplayHost host() {
        DiagnosticReplayHost h;
        h.frame = [this] { return frame; };
        h.state_epoch = [this] { return epoch; };
        h.save_state = [this](std::vector<std::uint8_t>& b, std::string&) {
            b = serialize();
            return true;
        };
        h.load_state = [this](const std::vector<std::uint8_t>& b, std::string& e) {
            if (restore(b)) return true;
            e = "size";
            return false;
        };
        h.timing_capture = [] { return std::string("0 0 0 0 0"); };
        h.timing_restore = [](const std::string&) { return true; };
        h.regions = [this](std::vector<DiagnosticRegion>& out) {
            out.clear();
            out.push_back({"RAM-lo", ram.data(), 2048});
            out.push_back({"RAM-hi", ram.data() + 2048, 2048});
            out.push_back({"CPU", &cpu, sizeof(cpu)});
        };
        h.save_memory = [this](std::vector<std::uint8_t>& out) { out = save; };
        h.save_write_seq = [this] { return save_seq; };
        h.capture_input = [this](DiagnosticFrameInput& in) { in.keyinput = keys; };
        h.apply_input = [this](const DiagnosticFrameInput& in) { keys = in.keyinput; };
        h.request_unwind = [this] { ++unwinds; };
        return h;
    }
};

std::uint16_t script_keys(std::uint64_t step) {
    return static_cast<std::uint16_t>(0x03FFu & ~((step / 7) % 11 ? (1u << ((step / 13) % 10)) : 0u));
}

DiagnosticReplayConfig config(const fs::path& dir) {
    DiagnosticReplayConfig c;
    c.directory = dir.string();
    c.tag = "synthetic";
    c.rom_sha1 = "0123456789abcdef0123456789abcdef01234567";
    c.engine_revision = "test";
    c.executable_hash = "test";
    c.title = "Synthetic machine";
    c.history_bytes = 64u * 1024u * 1024u;
    return c;
}

// Record `steps` frames; at step `restore_at` reload the state saved at
// `save_at` (a player's savestate). Returns the first segment's path.
std::string record(const fs::path& dir, unsigned steps, unsigned save_at,
                   unsigned restore_at, bool clean_exit) {
    Machine m;
    DiagnosticReplay rec;
    CHECK(rec.start_recording(config(dir), m.host()));
    std::vector<std::uint8_t> slot;
    for (unsigned step = 0; step < steps; ++step) {
        rec.frame_end();
        m.keys = script_keys(step);                       // host input
        if (step == save_at) slot = m.serialize();        // savestate save
        if (step == restore_at) CHECK(m.restore(slot));   // savestate load
        rec.frame_begin('h');
        m.run_frame();
    }
    rec.shutdown();
    std::vector<std::string> segs;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.path().extension() == ".replay") segs.push_back(e.path().string());
    std::sort(segs.begin(), segs.end());
    CHECK(!segs.empty());
    if (!clean_exit && !segs.empty()) {
        // A killed process never writes its end record.
        std::ifstream in(segs.back());
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        const auto e = text.rfind("\nE ");
        CHECK(e != std::string::npos);
        if (e != std::string::npos) {
            std::ofstream out(segs.back(), std::ios::binary | std::ios::trunc);
            out << text.substr(0, e + 1);
        }
    }
    return segs.empty() ? std::string() : segs.front();
}

struct PlaybackResult {
    bool finished = false;
    bool failed = false;
    std::uint64_t matched = 0;
    std::uint64_t unwinds = 0;
    std::uint64_t stop_frame = 0;
};

PlaybackResult play(const std::string& segment, std::uint64_t perturb_at = ~0ull) {
    Machine m;
    DiagnosticReplay rep;
    PlaybackResult r;
    if (!rep.start_playback(segment, config(fs::path(segment).parent_path()), m.host())) {
        r.failed = true;
        return r;
    }
    for (unsigned guard = 0; guard < 100000; ++guard) {
        rep.frame_end();
        rep.frame_begin('h');
        if (rep.finished() || rep.failed()) break;
        if (m.frame == perturb_at) m.ram[100] ^= 1;
        m.run_frame();
    }
    r.finished = rep.finished();
    r.failed = rep.failed();
    r.matched = rep.matched_frames();
    r.unwinds = m.unwinds;
    r.stop_frame = m.frame;
    return r;
}

std::string segment_name(const std::string& first, unsigned n) {
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), "-%06u.replay", n);
    return first.substr(0, first.size() - std::strlen("-000001.replay")) + suffix;
}

void test_writer(const fs::path& dir) {
    auto w = ReplayWriter::create();
    CHECK(w != nullptr);
    if (!w) return;
    const std::string path = (dir / "w.replay").string();
    CHECK(w->begin(path, "HEADER\n", {{(dir / "missing.ini").string(), path + ".cfg"}},
                   {{"inline body", path + ".txt"}}));
    std::vector<std::uint8_t> blob(3 * 1024 * 1024);
    for (std::size_t i = 0; i < blob.size(); ++i) blob[i] = static_cast<std::uint8_t>(i * 7);
    // Enough traffic to wrap the 32 MiB ring several times.
    for (int i = 0; i < 40; ++i) {
        ReplayWriter::Part parts[2] = {{blob.data(), blob.size(), false},
                                       {nullptr, 0, i != 0}};
        while (!w->frame(static_cast<std::uint64_t>(i), parts, 2)) {
            CHECK(w->status() == ReplayWriter::Status::Full);  // recoverable
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    CHECK(w->snapshot(blob.data(), 1000, 'C'));
    CHECK(w->line("L line\n"));
    CHECK(w->end());
    CHECK(w->shutdown() == nullptr);
    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.rfind("HEADER\n", 0) == 0);
    const std::uint64_t h = gbarecomp::debug::replay_hash(blob.data(), blob.size());
    char expect[64];
    std::snprintf(expect, sizeof(expect), "H 39 %016llx", static_cast<unsigned long long>(h));
    CHECK(text.find(expect) != std::string::npos);
    std::snprintf(expect, sizeof(expect), "S 0 1000 %016llx C",
                  static_cast<unsigned long long>(gbarecomp::debug::replay_hash(blob.data(), 1000)));
    CHECK(text.find(expect) != std::string::npos);
    CHECK(text.find("L line\n") != std::string::npos);
    CHECK(fs::exists(path + ".txt"));
    CHECK(!fs::exists(path + ".cfg"));  // absent sources are not errors
    CHECK(fs::file_size(path + ".state") == 1000);

    // Streaming and one-shot hashing agree across arbitrary splits.
    gbarecomp::debug::ReplayHash split;
    split.update(blob.data(), 3);
    split.update(blob.data() + 3, 1021);
    split.update(blob.data() + 1024, blob.size() - 1024);
    CHECK(split.finish() == h);

    // Oversized writes are sticky errors.
    auto bad = ReplayWriter::create();
    std::vector<std::uint8_t> huge(ReplayWriter::kQueueBytes);
    CHECK(!bad->snapshot(huge.data(), huge.size(), 'C'));
    CHECK(bad->status() == ReplayWriter::Status::Invalid);
    CHECK(!bad->line("x\n"));
    CHECK(bad->shutdown() != nullptr);
}

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() /
        ("gbarecomp-diagnostic-replay-test-" + std::to_string(std::rand()));
    fs::remove_all(root);
    fs::create_directories(root);

    test_writer(root);

    // Full session: 2000 frames (4 segments), a savestate saved at 300 and
    // restored at 1500, clean exit.
    const fs::path a = root / "a";
    fs::create_directories(a);
    const std::string first = record(a, 2000, 300, 1500, true);
    if (!first.empty()) {
        const PlaybackResult all = play(first);
        CHECK(all.finished && !all.failed);
        CHECK(all.matched == 1999);           // every executed frame fingerprinted
        CHECK(all.unwinds == 2);              // initial sync + the restore

        // A later segment replays on its own from its checkpoint.
        const PlaybackResult mid = play(segment_name(first, 3));
        CHECK(mid.finished && !mid.failed);
        CHECK(mid.matched > 500);

        // A perturbed machine stops at exactly the perturbed frame.
        const PlaybackResult bad = play(first, 777);
        CHECK(bad.failed && !bad.finished);
        CHECK(bad.matched == 777);
        CHECK(fs::exists("replay-divergence.state"));
        fs::remove("replay-divergence.state");
    }

    // A session without its end record reports the interrupted frame.
    const fs::path b = root / "b";
    fs::create_directories(b);
    const std::string killed = record(b, 700, ~0u, ~0u, false);
    if (!killed.empty()) {
        const PlaybackResult r = play(killed);
        CHECK(r.failed && !r.finished);
        CHECK(r.matched == 699);
    }

    fs::remove_all(root);
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("diagnostic replay tests passed\n");
    return 0;
}
