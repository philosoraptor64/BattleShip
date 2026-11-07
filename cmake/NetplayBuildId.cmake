# Netplay build fingerprint.
#
# Two players can only stay in sync if they run the same simulation, so the
# rendezvous server refuses to pair builds whose fingerprint differs (see
# port/net/netplay_rendezvous.cpp). The fingerprint covers everything that
# affects gameplay or the extracted game data: the port code, generated-header
# sources, the asset recipes, and the decomp / libultraship / torch checkouts,
# including uncommitted edits (so identical edits on two machines still match).
# Docs, scripts and CI are left out on purpose.
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

    set(parts "")
    # Committed state of the gameplay-relevant parts of this repo.
    foreach(path port include yamls config.yml CMakeLists.txt)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${src}" rev-parse "HEAD:${path}"
                        OUTPUT_VARIABLE id OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        string(APPEND parts "${path}=${id}\n")
    endforeach()
    # Their uncommitted edits. port/assets is left out: the build regenerates
    # its scroll-arrow PNGs in place, and different Pillow versions write
    # different bytes for the same image (menu art, not gameplay).
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${src}" diff HEAD -- port include yamls config.yml CMakeLists.txt
                            ":(exclude)port/assets"
                    OUTPUT_VARIABLE diff ERROR_QUIET)
    string(APPEND parts "${diff}")
    # The submodules as checked out (not just as recorded), plus their edits.
    foreach(sm decomp libultraship torch)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${src}/${sm}" rev-parse HEAD
                        OUTPUT_VARIABLE id OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${src}/${sm}" diff HEAD
                        OUTPUT_VARIABLE diff ERROR_QUIET)
        string(APPEND parts "${sm}=${id}\n${diff}")
    endforeach()

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
