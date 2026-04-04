add_executable(ipc_example main.cpp)

target_link_libraries(ipc_example PRIVATE ui::ui)

if(WIN32)
    target_link_libraries(ipc_example PRIVATE shell32)
endif()