if (WIN32)
    set(IPC_SRC main_win.cpp)
else()
    set(IPC_SRC main.cpp)
endif()

add_executable(ipc_example ${IPC_SRC})

target_link_libraries(ipc_example PRIVATE ui::ui)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/index.html
    ${CMAKE_CURRENT_BINARY_DIR}/index.html
    COPYONLY
)