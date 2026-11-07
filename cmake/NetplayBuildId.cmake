# Netplay build fingerprint.
#
# Two players can only stay in sync if they run the same simulation, so the
# rendezvous server refuses to pair builds whose fingerprint differs (see
# port/net/netplay_rendezvous.cpp). The fingerprint covers only what decides
# a match: the decomp game code, the port code the simulation runs through,
# and the asset extractor + recipes; uncommitted edits there count too (so
# identical edits on two machines still match). Gameplay *settings* (some
# enhancement cvars) are not code and must travel with the match config.
# When adding a port file that the simulation calls into, add it below.
#
# Runs as a build step on every build (a commit changes the fingerprint
# without touching any file, so a configure-time value would go stale):
#   cmake -DSRC=<source dir> -DOUT=<header> -P cmake/NetplayBuildId.cmake
# The header is rewritten only when the value changes, so only the file that
# includes it rebuilds. Outside a git checkout the value is "nogit".

function(ssb64_compute_netplay_build_id src out_var)
    find_package(Git QUIET)
    if (NOT GIT_FOUND OR NOT EXISTS "${src}/.git")
        set(${out_var} "nogit" PARENT_SCOPE)
        return()
    endif()

    # What decides the outcome of a match. Everything else (libultraship's
    # graphics/audio/input, menus art, hi-res packs, widescreen, frame
    # interpolation, platform glue, saves, the room-code client, build files)
    # may differ between players without affecting the simulation. Inputs
    # are exchanged after the controller layer, so input handling is out too.
    set(port_paths
        port/rollback port/resource port/bridge port/stubs port/enhancements port/hooks
        port/gameloop.cpp port/gameloop.h port/fighter_registry.cpp port/fighter_registry.h
        port/port_aobj_fixup.cpp port/port_aobj_fixup.h
        yamls config.yml)
    # repo-relative path list -> "repo:path=<git object id>" lines + uncommitted diff
    set(parts "")
    foreach(path ${port_paths})
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${src}" rev-parse "HEAD:${path}"
                        OUTPUT_VARIABLE id OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        string(APPEND parts "${path}=${id}\n")
    endforeach()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${src}" diff HEAD -- ${port_paths}
                    OUTPUT_VARIABLE diff ERROR_QUIET)
    string(APPEND parts "${diff}")

    # The game itself (decomp source, including the netplay sync code).
    foreach(path src include)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${src}/decomp" rev-parse "HEAD:${path}"
                        OUTPUT_VARIABLE id OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        string(APPEND parts "decomp/${path}=${id}\n")
    endforeach()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${src}/decomp" diff HEAD -- src include
                    OUTPUT_VARIABLE diff ERROR_QUIET)
    string(APPEND parts "${diff}")

    # The asset extractor (decides the game data built from the ROM).
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${src}/torch" rev-parse "HEAD:src"
                    OUTPUT_VARIABLE id OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${src}/torch" diff HEAD -- src
                    OUTPUT_VARIABLE diff ERROR_QUIET)
    string(APPEND parts "torch/src=${id}\n${diff}")

    string(SHA1 hash "${parts}")
    string(SUBSTRING "${hash}" 0 12 short)
    set(${out_var} "${short}" PARENT_SCOPE)
endfunction()

if (CMAKE_SCRIPT_MODE_FILE AND DEFINED SRC AND DEFINED OUT)
    ssb64_compute_netplay_build_id("${SRC}" id)
    set(content "#pragma once\n#define SSB64_NETPLAY_BUILD_ID \"${id}\"\n")
    set(old "")
    if (EXISTS "${OUT}")
        file(READ "${OUT}" old)
    endif()
    if (NOT old STREQUAL content)
        file(WRITE "${OUT}" "${content}")
        message(STATUS "Netplay build ID: ${id}")
    endif()
endif()
