add_executable(ipc_example main.cpp)

target_link_libraries(ipc_example PRIVATE ui::ui)

# copy index.html next to the binary so __FILE__-relative lookup works
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/index.html
    ${CMAKE_CURRENT_BINARY_DIR}/index.html
    COPYONLY
)