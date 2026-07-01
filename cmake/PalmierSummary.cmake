# SPDX-License-Identifier: GPL-3.0-or-later
#
# Prints a concise configuration summary at the end of CMake configuration.

include_guard(GLOBAL)

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
message(STATUS "  HW codec: VAAPI   : ${PALMIER_ENABLE_VAAPI}")
message(STATUS "  HW codec: NVENC   : ${PALMIER_ENABLE_NVENC}")
message(STATUS "  HW codec: QSV     : ${PALMIER_ENABLE_QSV}")
message(STATUS "===========================================================")
message(STATUS "")
