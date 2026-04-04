cmake_minimum_required(VERSION 3.22)

if (APPLE)
    project(ui VERSION 0.1.0 LANGUAGES CXX OBJCXX)
else()
    project(ui VERSION 0.1.0 LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Sources ───────────────────────────────────────────────────────────────────

set(UI_SOURCES)

if (WIN32)
    list(APPEND UI_SOURCES
        src/win/webview.cpp
        src/win/scheme.cpp
        src/win/navigation.cpp
        src/win/ipc.cpp
        src/win/lifecycle.cpp
        src/win/window.cpp
    )
elseif (APPLE)
    list(APPEND UI_SOURCES
        src/mac/webview.mm
        src/mac/delegates.mm
        src/mac/navigation.mm
        src/mac/ipc.mm
        src/mac/lifecycle.mm
        src/mac/window.mm
    )
elseif (UNIX)
    list(APPEND UI_SOURCES
        src/linux/webview.cpp
        src/linux/scheme.cpp
        src/linux/navigation.cpp
        src/linux/ipc.cpp
        src/linux/lifecycle.cpp
        src/linux/window.cpp
    )
else()
    message(FATAL_ERROR "ui: unsupported platform")
endif()

# ── Library target ────────────────────────────────────────────────────────────

add_library(ui STATIC ${UI_SOURCES})
add_library(ui::ui ALIAS ui)

# include/ is public: consumers get <ui/webview.h> and <logger/logger.h>
target_include_directories(ui PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# src/ is private: platform files get <shared/helpers.h> and <shared/shim.h>
target_include_directories(ui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
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

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT ui-targets
    FILE ui-config.cmake
    NAMESPACE ui::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ui
)