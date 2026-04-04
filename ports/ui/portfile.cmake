vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL             https://github.com/carbon-os/ui
    HEAD_REF        main
    # Once you tag releases, replace HEAD_REF with:
    # REF             <full-commit-sha>
    # HEAD_REF        main
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

# Remove duplicate headers from the debug tree
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

# vcpkg requires a copyright/license file
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")