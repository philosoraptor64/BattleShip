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
# Computed at configure time; ios/build-and-install.sh reconfigures on every
# run. Outside a git checkout it falls back to "nogit".

function(ssb64_compute_netplay_build_id out_var)
    find_package(Git QUIET)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}")
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
    # Their uncommitted edits.
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${src}" diff HEAD -- port include yamls config.yml CMakeLists.txt
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
