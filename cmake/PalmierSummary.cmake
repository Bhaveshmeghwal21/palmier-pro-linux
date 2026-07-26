# SPDX-License-Identifier: GPL-3.0-or-later
#
# Prints a concise configuration summary at the end of CMake configuration.

include_guard(GLOBAL)

# ---------------------------------------------------------------------------
# palmier_vendor_path_status(<out-var> <enable-var> <available-var> <found-var>)
#
# Resolves a vendor hardware-codec path to exactly one of three states:
#
#   enabled (SDK found)        option ON  and the vendor SDK was located
#   disabled (SDK not found)   option ON  but the vendor SDK is absent
#   disabled (option OFF)      option OFF (SDK presence is irrelevant)
#
# The availability variable is published by cmake/PalmierDependencies.cmake
# (PALMIER_VAAPI_AVAILABLE / PALMIER_QSV_AVAILABLE / PALMIER_NVENC_AVAILABLE).
# It is read defensively: if it is not defined we fall back to the pkg-config
# <prefix>_FOUND variable, and if that is undefined too the path is reported as
# "disabled (SDK not found)" rather than breaking configuration.
# ---------------------------------------------------------------------------
function(palmier_vendor_path_status out_var enable_var available_var found_var)
    if(NOT ${enable_var})
        set(${out_var} "disabled (option OFF)" PARENT_SCOPE)
        return()
    endif()

    if(DEFINED ${available_var})
        set(_sdk "${${available_var}}")
    elseif(DEFINED ${found_var})
        set(_sdk "${${found_var}}")
    else()
        set(_sdk "")
    endif()

    if(_sdk)
        set(${out_var} "enabled (SDK found)" PARENT_SCOPE)
    else()
        set(${out_var} "disabled (SDK not found)" PARENT_SCOPE)
    endif()
endfunction()

palmier_vendor_path_status(_palmier_vaapi_status
    PALMIER_ENABLE_VAAPI PALMIER_VAAPI_AVAILABLE LIBVA_FOUND)
palmier_vendor_path_status(_palmier_nvenc_status
    PALMIER_ENABLE_NVENC PALMIER_NVENC_AVAILABLE FFNVCODEC_FOUND)
palmier_vendor_path_status(_palmier_qsv_status
    PALMIER_ENABLE_QSV PALMIER_QSV_AVAILABLE LIBVPL_FOUND)

message(STATUS "")
message(STATUS "==================== Palmier Pro Linux ====================")
message(STATUS "  Version           : ${PROJECT_VERSION}")
message(STATUS "  Build type        : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard      : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler          : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  ---------------------------------------------------------")
message(STATUS "  Build UI (Qt 6)   : ${PALMIER_BUILD_UI}")
message(STATUS "  Build tests       : ${PALMIER_BUILD_TESTS}")
message(STATUS "  Warnings as errors: ${PALMIER_WERROR}")
message(STATUS "  ---------------------------------------------------------")
message(STATUS "  HW codec: VAAPI   : ${_palmier_vaapi_status}")
message(STATUS "  HW codec: NVENC   : ${_palmier_nvenc_status}")
message(STATUS "  HW codec: QSV     : ${_palmier_qsv_status}")
message(STATUS "===========================================================")
message(STATUS "")
