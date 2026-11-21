add_executable(FileIndexTest
  FileIndexTest.cpp
  ${REPO_ROOT}/lib/Serialization/AtomicFile.cpp
  ${REPO_ROOT}/lib/FileIndex/FileIndex.cpp
  ${REPO_ROOT}/lib/FsHelpers/NaturalSort.cpp
)
target_include_directories(FileIndexTest PRIVATE
  ${REPO_ROOT}/test/library_builder/stubs
  ${REPO_ROOT}/lib/FileIndex
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/FsHelpers
)
target_link_libraries(FileIndexTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(FileIndexTest)
