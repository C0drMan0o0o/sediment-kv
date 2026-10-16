add_executable(sediment_tests placeholder_test.cpp status_test.cpp coding_test.cpp arena_test.cpp skiplist_test.cpp internal_key_test.cpp)
target_link_libraries(sediment_tests PRIVATE sediment sediment_warnings GTest::gtest_main)
gtest_discover_tests(sediment_tests)
