#
# Copyright (c) 2026 [大河马/dahema@me.com]
# SPDX-License-Identifier: MIT
#

# GTEST
find_package(GTest)

if(${GTEST_FOUND})
  include(GoogleTest)
  enable_testing()

  add_executable(lpc_tests test_lpc.cc)
  target_link_libraries(lpc_tests PRIVATE ${MUDOS_NG_LINK} GTest::GTest  GTest::Main)
  target_compile_definitions(lpc_tests PRIVATE -DTESTSUITE_DIR="${CMAKE_SOURCE_DIR}/testsuite")

  add_executable(ofile_tests test_ofile.cc)
  target_link_libraries(ofile_tests PRIVATE ${MUDOS_NG_LINK} GTest::GTest  GTest::Main)
  target_compile_definitions(ofile_tests PRIVATE -DTESTSUITE_DIR="${CMAKE_SOURCE_DIR}/testsuite")

  add_executable(io_thread_tests test_io_thread.cc)
  target_link_libraries(io_thread_tests PRIVATE ${MUDOS_NG_LINK} GTest::GTest  GTest::Main)

  add_executable(heartbeat_thread_tests test_heartbeat_thread.cc)
  target_link_libraries(heartbeat_thread_tests PRIVATE ${MUDOS_NG_LINK} GTest::GTest GTest::Main)

  gtest_discover_tests(lpc_tests ofile_tests io_thread_tests heartbeat_thread_tests DISCOVERY_TIMEOUT 60)
endif()

