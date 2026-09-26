cmake_minimum_required(VERSION 3.13)
project(fcitx5-sekka VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# fcitx5 5.1.12 or later is required: src/sekka.cpp registers the addon with
# FCITX_ADDON_FACTORY_V2, and that macro was added to fcitx/addoninstance.h in 5.1.12.
# Declaring 5.0.0 here only moved the failure from configure time to a confusing
# "expected constructor, destructor, or type conversion" compile error (measured on
# Ubuntu 24.04, which ships 5.1.2).
find_package(Fcitx5Core 5.1.12 REQUIRED)
# Optional dependency on the notifications addon (D-105/FR-009). This package defines an
# INTERFACE library called Fcitx5::Module::Notifications that provides only the include
# directory of the notifications public headers (there is no library to link against,
# because calls between addons are resolved at runtime through the AddonManager).
find_package(Fcitx5ModuleNotifications REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Gettext REQUIRED)

# The libsekka version CI builds against. bin/sekka_version.sh reads this line, so keep
# the format as it is (the same practice as fcitx5-cskk).
# GITHUB_ACTION_BUILD_SEKKA_VERSION=0.1.0
pkg_check_modules(SEKKA REQUIRED IMPORTED_TARGET sekka>=0.1)

include("${FCITX_INSTALL_CMAKECONFIG_DIR}/Fcitx5Utils/Fcitx5CompilerSettings.cmake" OPTIONAL)

add_compile_options(-Wall -Wextra -Werror)

add_definitions(-DFCITX_GETTEXT_DOMAIN=\"fcitx5-sekka\")
fcitx5_add_i18n_definition()

add_subdirectory(po)
add_subdirectory(src)

option(ENABLE_TEST "Enable the tests" OFF)
if(ENABLE_TEST)
    enable_testing()
    add_subdirectory(test)
endif()

# Build-time generation of the master dictionary (D-62 / D-63 / D-76, FR-015).
# Off by default: generating 175,789 keys / 62 MB and downloading SKK-JISYO.L (4.5 MB) on
# every build would break the development loop, so this only runs when
# -DSEKKA_BUILD_DICT=ON is given explicitly.
# In the default configuration nothing inside this block runs at all (neither
# file(DOWNLOAD) nor the include).
#
# Note for Phase 3: the full dictionary is a build output, so verify it only after building
# with an explicit `-DSEKKA_BUILD_DICT=ON` (including when verifying SC1 from a clean work
# tree).
option(SEKKA_BUILD_DICT "Build the master dictionary from SKK-JISYO.L (D-76: off by default)" OFF)
if(SEKKA_BUILD_DICT)
    # Note for Phase 3: the full dictionary (175,789 keys) is a build output and is not
    # under git. When verifying from a clean work tree (SC1), build with an explicit
    # -DSEKKA_BUILD_DICT=ON and verify against the generated master-dict.db.
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
    include(FetchSkkJisyo)

    # Resolve the location of sekka-dict-tool (on the Rust side,
    # libsekka/bin/sekka-dict-tool.rs) (D-62). CMake does not build libsekka, so when it is
    # not found the message explains how to build it with cargo.
    set(SEKKA_DICT_TOOL "" CACHE FILEPATH
        "Path of the sekka-dict-tool binary (auto-detected when unset)")
    if(NOT SEKKA_DICT_TOOL)
        find_program(_sekka_dict_tool_found sekka-dict-tool
            HINTS
                "${CMAKE_CURRENT_SOURCE_DIR}/../libsekka/target/release"
                "${CMAKE_CURRENT_SOURCE_DIR}/../libsekka/target/debug"
                "${CMAKE_INSTALL_PREFIX}/bin")
        if(_sekka_dict_tool_found)
            set(SEKKA_DICT_TOOL "${_sekka_dict_tool_found}" CACHE FILEPATH
                "Path of the sekka-dict-tool binary" FORCE)
        endif()
    endif()
    if(NOT SEKKA_DICT_TOOL)
        message(FATAL_ERROR
            "sekka-dict-tool not found. CMake does not build libsekka (the Rust side), so "
            "either run `cd libsekka && cargo build --release --bin sekka-dict-tool` first, "
            "or point -DSEKKA_DICT_TOOL=<path> at the binary and reconfigure.")
    endif()

    # The master dictionary generation target (D-62). ALL is attached only inside this
    # if(SEKKA_BUILD_DICT) block, so with the option off (the default) it never joins the
    # build at all (D-76).
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/master-dict.db"
        COMMAND "${SEKKA_DICT_TOOL}" convert "${_skkjisyo_raw}"
                --output "${CMAKE_CURRENT_BINARY_DIR}/master-dict.db"
        DEPENDS "${_skkjisyo_raw}" "${SEKKA_DICT_TOOL}"
        COMMENT "Generating the master dictionary from SKK-JISYO.L (this takes a few minutes)"
        VERBATIM)
    add_custom_target(sekka-dict ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/master-dict.db")

    # Installing the generated master dictionary (D-62).
    # It follows the convention of placing it under ${FCITX_INSTALL_PKGDATADIR}/sekka.
    # This install rule also lives inside if(SEKKA_BUILD_DICT), so the default build's
    # cmake --install never breaks by trying to install a file that was not generated.
    # PERMISSIONS is explicit because the install default (0644) would place the dictionary
    # at a path **writable by the installing user themselves**. openDictionaries on the
    # fcitx5 side refuses to load a writable dictionary path (02.1-REVIEW CR-01, guarding
    # against SIGBUS from a rewrite while mapped), so a dictionary installed as 0644 into a
    # non-root prefix (such as the development .sekka-prefix) would not be loaded at all.
    # This keeps the same invariant on the install side as the 0444 output of
    # sekka-dict-tool.
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/master-dict.db"
        DESTINATION "${FCITX_INSTALL_PKGDATADIR}/sekka"
        PERMISSIONS OWNER_READ GROUP_READ WORLD_READ)
endif()

include(GNUInstallDirs)

fcitx5_translate_desktop_file(org.fcitx.Fcitx5.Addon.Sekka.metainfo.xml.in
                              org.fcitx.Fcitx5.Addon.Sekka.metainfo.xml XML)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/org.fcitx.Fcitx5.Addon.Sekka.metainfo.xml"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
