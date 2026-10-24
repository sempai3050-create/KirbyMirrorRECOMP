#include "config.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

int fail(const char* message) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

bool write_file(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

std::string base_config(const std::string& hooks) {
    return
        "[program]\n"
        "load_address = 0x08000000\n"
        "size = 0x100\n"
        "entry_pc = 0x08000000\n\n"
        "[identity]\n"
        "sha1 = \"0000000000000000000000000000000000000000\"\n\n" + hooks;
}

}  // namespace

int main() {
    const fs::path path = fs::temp_directory_path() /
        ("gbarecomp-config-hook-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".toml");
    std::error_code ec;
    gbarecomp::Config cfg;

    const std::string valid = base_config(
        "[[mod_function_hook]]\n"
        "addr = 0x08000020\n"
        "mode = \"thumb\"\n");
    if (!write_file(path, valid) || !gbarecomp::load_config(path.string(), cfg) ||
        !cfg.program.static_resume_all ||
        cfg.mod_function_hooks.size() != 1 ||
        cfg.mod_function_hooks[0].mode != gbarecomp::CpuMode::Thumb) {
        fs::remove(path, ec);
        return fail("valid mod_function_hook did not parse");
    }

    const std::string unaligned = base_config(
        "[[mod_function_hook]]\n"
        "addr = 0x08000021\n"
        "mode = \"thumb\"\n");
    std::string explicit_no_resumes = valid;
    explicit_no_resumes.insert(explicit_no_resumes.find("[identity]"), "static_resume_all = false\n\n");
    if (!write_file(path, explicit_no_resumes) || !gbarecomp::load_config(path.string(), cfg) ||
        cfg.program.static_resume_all || !write_file(path, valid) ||
        !gbarecomp::load_config(path.string(), cfg) || !cfg.program.static_resume_all) {
        fs::remove(path, ec);
        return fail("explicit static-resume override or reloaded default was lost");
    }
    if (!write_file(path, unaligned) || gbarecomp::load_config(path.string(), cfg)) {
        fs::remove(path, ec);
        return fail("unaligned mod_function_hook was accepted");
    }

    const std::string mismatch = base_config(
        "[[extra_func]]\n"
        "addr = 0x08000020\n"
        "mode = \"arm\"\n\n"
        "[[mod_function_hook]]\n"
        "addr = 0x08000020\n"
        "mode = \"thumb\"\n");
    if (!write_file(path, mismatch) || gbarecomp::load_config(path.string(), cfg)) {
        fs::remove(path, ec);
        return fail("mode-conflicting mod_function_hook was accepted");
    }
    fs::remove(path, ec);
    for (const auto* format : {"u8_rel_thumb", "u16_rel_thumb"}) {
        const unsigned width = std::string(format) == "u8_rel_thumb" ? 1 : 2;
        const std::string table =
            "[[jump_table]]\naddr = 0x08000060\ncount = 3\nstride = " +
            std::to_string(width) + "\nformat = \"" + format +
            "\"\nentries_mode = \"thumb\"\n";
        if (!write_file(path, base_config(table + "base = 0x08000012\n")) ||
            !gbarecomp::load_config(path.string(), cfg) ||
            cfg.jump_tables.size() != 1 || cfg.jump_tables[0].base != 0x08000012u) {
            fs::remove(path, ec);
            return fail("valid relative Thumb table rejected");
        }
        for (const auto& invalid : {table, table + "base = 0x08000013\n",
                                   table + "base = 2\n"}) {
            if (!write_file(path, base_config(invalid)) ||
                gbarecomp::load_config(path.string(), cfg)) {
                fs::remove(path, ec);
                return fail("invalid relative Thumb table accepted");
            }
        }
    }
    fs::remove(path, ec);
    return 0;
}
