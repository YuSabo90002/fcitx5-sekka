set(SEKKA_SOURCES
    sekka.cpp
    sekkacandidatelist.cpp
)

add_library(sekka-module MODULE ${SEKKA_SOURCES})

target_link_libraries(sekka-module
    PRIVATE
        Fcitx5::Core
        PkgConfig::SEKKA
        Fcitx5::Module::Notifications
)

target_include_directories(sekka-module
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

set_target_properties(sekka-module PROPERTIES
    PREFIX ""
    OUTPUT_NAME "sekka"
)

install(TARGETS sekka-module
    DESTINATION "${FCITX_INSTALL_ADDONDIR}")

# Addon metadata
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/sekka-addon.conf.in.in"
    "${CMAKE_CURRENT_BINARY_DIR}/sekka-addon.conf"
    @ONLY)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/sekka-addon.conf"
    DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon"
    RENAME sekka.conf)

# Input method metadata
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/sekka.conf.in"
    "${CMAKE_CURRENT_BINARY_DIR}/sekka-im.conf"
    COPYONLY)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/sekka-im.conf"
    DESTINATION "${FCITX_INSTALL_PKGDATADIR}/inputmethod"
    RENAME sekka.conf)
