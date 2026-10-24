#include <cstring>
#include <string>
#include <vector>

#include "mobile_platform.h"
#include "runtime.h"

#ifndef GBARECOMP_BUILTIN_NAME
#define GBARECOMP_BUILTIN_NAME "Kirby & The Amazing Mirror (USA)"
#endif
#ifndef GBARECOMP_BUILTIN_SHA1
#define GBARECOMP_BUILTIN_SHA1 "274b102b6d940f46861a92b4e65f89a51815c12c"
#endif
#ifndef GBARECOMP_DEFAULT_GAME_CONFIG
#define GBARECOMP_DEFAULT_GAME_CONFIG "game.toml"
#endif

namespace {

int kirby_main(int argc, char** argv) {
    std::vector<std::string> args(argv, argv + argc);
    gbarecomp::MobileProcessOptions mobile;
    mobile.game_config = GBARECOMP_DEFAULT_GAME_CONFIG;
    mobile.program_name = "./KirbyAmazingMirrorRecomp";
    gbarecomp::mobile_prepare_process(args, mobile);

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 ||
            std::strcmp(argv[i], "-h") == 0) {
            return 0;
        }
    }

    gbarecomp::RunOptions options;
    options.builtin_game_name = GBARECOMP_BUILTIN_NAME;
    options.builtin_rom_sha1 = GBARECOMP_BUILTIN_SHA1;
    options.launcher_region = "USA";
    options.launcher_game_config = GBARECOMP_DEFAULT_GAME_CONFIG;
    options.launcher_save_path = "saves/kirby_amazing_mirror.sav";
    options.ui_touch_friendly = true;
    options.orientation = gbarecomp::RunOptions::Orientation::Landscape;
    std::vector<char*> native_args;
    native_args.reserve(args.size());
    for (std::string& arg : args) {
        native_args.push_back(arg.data());
    }
    return gbarecomp::run_game(
        static_cast<int>(native_args.size()), native_args.data(), options);
}

}  // namespace

#if defined(__ANDROID__)
extern "C" int SDL_main(int argc, char** argv) {
    return gbarecomp::mobile_run_with_stack(kirby_main, argc, argv);
}
#else
int main(int argc, char** argv) {
    return kirby_main(argc, argv);
}
#endif
