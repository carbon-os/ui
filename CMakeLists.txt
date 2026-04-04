cmake_minimum_required(VERSION 3.22)

if (APPLE)
    project(ui VERSION 0.1.0 LANGUAGES CXX OBJCXX)
else()
    project(ui VERSION 0.1.0 LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Logger (header-only, globally available to all targets) ───────────────────
#
#  logger/logger.h lives at the root of the repo. Adding the root as a
#  global include dir means any target anywhere in the tree can do:
#      #include <logger/logger.h>
#  with no extra target_link_libraries needed.

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# ── Sources ───────────────────────────────────────────────────────────────────

set(UI_SOURCES)

if (WIN32)
    list(APPEND UI_SOURCES src/win/webview.cpp)
elseif (APPLE)
    list(APPEND UI_SOURCES src/mac/webview.mm)
elseif (UNIX)
    list(APPEND UI_SOURCES src/linux/webview.cpp)
else()
    message(FATAL_ERROR "ui: unsupported platform")
endif()

# ── Library target ────────────────────────────────────────────────────────────

add_library(ui STATIC ${UI_SOURCES})
add_library(ui::ui ALIAS ui)

target_include_directories(ui PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# ── Platform dependencies ─────────────────────────────────────────────────────

if (WIN32)
    find_package(unofficial-webview2 CONFIG REQUIRED)
    target_link_libraries(ui PRIVATE
        unofficial::webview2::webview2
        user32
        ole32
        shell32
    )

elseif (APPLE)
    target_link_libraries(ui PRIVATE
        "-framework WebKit"
        "-framework Cocoa"
    )

elseif (UNIX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WEBKIT REQUIRED webkit2gtk-4.1)

    target_include_directories(ui PRIVATE ${WEBKIT_INCLUDE_DIRS})
    target_link_libraries(ui PRIVATE ${WEBKIT_LIBRARIES})
    target_compile_options(ui PRIVATE ${WEBKIT_CFLAGS_OTHER})
endif()

# ── Compiler warnings ─────────────────────────────────────────────────────────

if (MSVC)
    target_compile_options(ui PRIVATE /W4 /WX)
else()
    target_compile_options(ui PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# ── Optional: build examples ──────────────────────────────────────────────────

option(UI_BUILD_EXAMPLES "Build examples" OFF)

if (UI_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ── Install ───────────────────────────────────────────────────────────────────

include(GNUInstallDirs)

install(TARGETS ui
    EXPORT ui-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/ui
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY logger
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT ui-targets
    FILE ui-config.cmake
    NAMESPACE ui::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ui
)