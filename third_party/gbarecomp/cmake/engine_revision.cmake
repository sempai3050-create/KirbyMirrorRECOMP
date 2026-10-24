# Writes ${OUT} with the engine's current git revision, only when it changed,
# so diagnostic replays always name the engine that was actually compiled.
# -DOVERRIDE=<rev> wins (release builds from a copied tree without .git).
if(OVERRIDE)
    set(rev "${OVERRIDE}")
else()
    execute_process(
        COMMAND git -C "${SRC}" rev-parse --short=12 HEAD
        OUTPUT_VARIABLE rev OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE rev_result ERROR_QUIET)
    if(NOT rev_result EQUAL 0 OR rev STREQUAL "")
        set(rev "unknown")
    else()
        execute_process(
            COMMAND git -C "${SRC}" status --porcelain --untracked-files=no
                -- src CMakeLists.txt cmake external
            OUTPUT_VARIABLE dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(NOT dirty STREQUAL "")
            set(rev "${rev}-dirty")
        endif()
    endif()
endif()
set(text "#pragma once\n#define GBARECOMP_ENGINE_REVISION \"${rev}\"\n")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
    if(old STREQUAL text)
        return()
    endif()
endif()
file(WRITE "${OUT}" "${text}")
