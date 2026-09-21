include_guard(GLOBAL)

option(WR64_ENABLE_IPO "Enable checked IPO/LTO for selected game targets" OFF)
option(WR64_IPO_RECOMPILED "Include recompiled game code when IPO is enabled" ON)
option(WR64_IPO_RUNTIME "Include librecomp and ultramodern when IPO is enabled" ON)

function(wr64_configure_performance_build)
    if(NOT WR64_ENABLE_IPO)
        return()
    endif()

    # Function scope prevents probe flags/policies from changing the build.
    cmake_policy(SET CMP0069 NEW)
    if(POLICY CMP0138)
        cmake_policy(SET CMP0138 NEW)
    elseif(WR64_SWITCH)
        message(FATAL_ERROR "Switch IPO requires CMake 3.24+ so the capability probe preserves toolchain flags.")
    endif()
    if(WR64_SWITCH)
        # Directory compile options are not inherited by try_compile projects.
        string(APPEND CMAKE_C_FLAGS " -fPIC -mtp=soft -ftls-model=local-exec")
        string(APPEND CMAKE_CXX_FLAGS " -fPIC -mtp=soft -ftls-model=local-exec")
        string(APPEND CMAKE_EXE_LINKER_FLAGS " -fPIC -mtp=soft -ftls-model=local-exec")
    endif()
    include(CheckIPOSupported)
    check_ipo_supported(RESULT wr64_ipo_supported OUTPUT wr64_ipo_output LANGUAGES C CXX)
    if(NOT wr64_ipo_supported)
        message(FATAL_ERROR
            "WR64_ENABLE_IPO was requested but the C/C++ toolchain failed its IPO check. "
            "Use a compatible compiler/linker or configure with -DWR64_ENABLE_IPO=OFF.\n${wr64_ipo_output}")
    endif()

    # Deliberate allowlist: never set CMAKE_INTERPROCEDURAL_OPTIMIZATION,
    # recurse through dependencies, or modify imported/prebuilt SDK targets.
    set(wr64_ipo_targets WaveRace64Recompiled)
    if(WR64_IPO_RECOMPILED)
        list(APPEND wr64_ipo_targets WaveRace64RecompiledCode)
    endif()
    if(WR64_IPO_RUNTIME)
        list(APPEND wr64_ipo_targets librecomp ultramodern)
    endif()
    foreach(wr64_target IN LISTS wr64_ipo_targets)
        if(NOT TARGET ${wr64_target})
            message(FATAL_ERROR "IPO target does not exist: ${wr64_target}")
        endif()
        get_target_property(wr64_imported ${wr64_target} IMPORTED)
        if(wr64_imported)
            message(FATAL_ERROR "Refusing IPO on imported target: ${wr64_target}")
        endif()
        foreach(wr64_config RELEASE RELWITHDEBINFO MINSIZEREL)
            set_property(TARGET ${wr64_target} PROPERTY
                INTERPROCEDURAL_OPTIMIZATION_${wr64_config} TRUE)
        endforeach()
    endforeach()
    if(WR64_SWITCH)
        # LTO performs code generation at the final link as well as compile.
        target_link_options(WaveRace64Recompiled PRIVATE
            "$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-fPIC>"
            "$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-mtp=soft>"
            "$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-ftls-model=local-exec>")
    endif()
    message(STATUS "WaveRace IPO (Release/RelWithDebInfo/MinSizeRel): ${wr64_ipo_targets}")
endfunction()
