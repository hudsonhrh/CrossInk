add_executable(ClippingDetailLayoutTest
  ClippingDetailLayoutTest.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(ClippingDetailLayoutTest PRIVATE ${REPO_ROOT}/src ${REPO_ROOT}/lib/Utf8)
target_link_libraries(ClippingDetailLayoutTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ClippingDetailLayoutTest)
