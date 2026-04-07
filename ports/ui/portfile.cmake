vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL             https://github.com/carbon-os/ui
    REF             a01692d056713402a5400151600544d6e9093b52
    HEAD_REF        main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DUI_BUILD_EXAMPLES=OFF
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(
    CONFIG_PATH lib/cmake/ui
)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")