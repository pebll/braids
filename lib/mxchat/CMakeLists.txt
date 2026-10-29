# Project
cmake_minimum_required(VERSION 3.16 FATAL_ERROR)
project(libmxchat LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 14)

# Ccache
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
endif()

# Library
add_library(mxchat ${LIBRARY_LINKING_TYPE}
  src/mxchat.cpp
  src/mxchat.h
)
install(TARGETS mxchat DESTINATION ${CMAKE_INSTALL_LIBDIR})

# Common
target_common_config(mxchat)

# Dependency libraries
add_subdirectory(go)
set(MXCHAT_GOLIB ref-cgomx)
add_dependencies(mxchat ${MXCHAT_GOLIB})

# Platform specifics
if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
  add_compile_definitions(_XOPEN_SOURCE_EXTENDED)
  FIND_LIBRARY(CARBON_LIBRARY CoreFoundation)
  FIND_LIBRARY(CARBON_LIBRARY Security)
  target_link_libraries(mxchat PUBLIC "-framework CoreFoundation" "-framework Security" resolv)
elseif (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
  # Go's cgo resolver calls res_search, which glibc < 2.34 (the manylinux dist
  # image) keeps in libresolv rather than libc. Go declares the -lresolv it
  # needs in a #cgo directive, but a c-archive does not propagate those to the
  # final link, so spell it out here.
  target_link_libraries(mxchat PUBLIC resolv)
endif()

# Headers
target_include_directories(mxchat PRIVATE "../common/src")
target_include_directories(mxchat PRIVATE "../ncutil/src")

# Compiler flags
set_target_properties(mxchat PROPERTIES COMPILE_FLAGS
                      "-Wall -Wextra -Wpedantic -Wshadow -Wpointer-arith \
                       -Wcast-qual -Wno-missing-braces -Wswitch-default \
                       -Wunreachable-code -Wundef -Wuninitialized \
                       -Wcast-align")

# Linking
message(STATUS "Go libraries: ${GO_LIBRARIES}")
target_link_libraries(mxchat PUBLIC ${MXCHAT_GOLIB} ncutil ${GO_LIBRARIES})
