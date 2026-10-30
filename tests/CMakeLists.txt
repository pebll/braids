# Tests
#
# Unit tests for utility functions with self-contained logic, built as small
# executables linking libncutil and run by ctest (via ./make.sh tests).

# Test macro add_unit_test
macro(add_unit_test testname)
  add_executable(${testname} ${testname}.cpp)

  # Headers
  target_include_directories(${testname} PRIVATE "../lib/common/src")
  target_include_directories(${testname} PRIVATE "../lib/ncutil/src")

  # Compiler flags
  set_target_properties(${testname} PROPERTIES COMPILE_FLAGS
                        "-Wall -Wextra -Wpedantic -Wshadow -Wpointer-arith \
                         -Wcast-qual -Wno-missing-braces -Wswitch-default \
                         -Wunreachable-code -Wundef -Wuninitialized \
                         -Wcast-align")

  # Linking
  target_link_libraries(${testname} PUBLIC ncutil)

  # Common
  target_common_config(${testname})

  # Test registration. The timeout ensures a hang in a tested function fails
  # the test run, rather than blocking it indefinitely.
  add_test(NAME ${testname} COMMAND ${testname})
  set_tests_properties(${testname} PROPERTIES TIMEOUT 60)
endmacro()

# Unit tests
add_unit_test(test001)
add_unit_test(test002)

# Go unit tests for the Matrix protocol
if(HAS_MATRIX)
  add_test(NAME mxchat_go
           COMMAND go test -tags goolm -modcacherw -buildvcs=false ./...
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../lib/mxchat/go)
  set_tests_properties(mxchat_go PROPERTIES TIMEOUT 600)
endif()

# braidsd core tests (stdio api, Dummy protocol)
find_package(Python3 COMPONENTS Interpreter)
if(HAS_DUMMY AND Python3_FOUND)
  add_test(NAME braidsd_dummy
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/braidsd/test_dummy.py
                   --braidsd $<TARGET_FILE:braidsd>)
  set_tests_properties(braidsd_dummy PROPERTIES TIMEOUT 120)
endif()

# braidsd Matrix integration test, needs lib/mxchat/test/homeserver.sh running
# (skipped otherwise)
if(HAS_MATRIX AND Python3_FOUND)
  add_test(NAME braidsd_matrix
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/braidsd/test_matrix.py
                   --braidsd $<TARGET_FILE:braidsd>
                   --peer-src ${CMAKE_CURRENT_SOURCE_DIR}/../lib/mxchat/test/peer)
  set_tests_properties(braidsd_matrix PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77)
endif()
