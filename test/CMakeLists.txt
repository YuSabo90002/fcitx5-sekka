find_package(GTest REQUIRED)

add_executable(sekka_test
    main.cpp
    basic_test.cpp
    focus_policy_test.cpp
    dict_error_test.cpp
    save_test.cpp
    candidate_list_test.cpp
    ${CMAKE_SOURCE_DIR}/src/sekkacandidatelist.cpp
)

target_link_libraries(sekka_test
    PRIVATE
        GTest::GTest
        GTest::Main
        PkgConfig::SEKKA
        Fcitx5::Core
        Fcitx5::Module::Notifications
)

target_include_directories(sekka_test
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

include(GoogleTest)
gtest_discover_tests(sekka_test)
