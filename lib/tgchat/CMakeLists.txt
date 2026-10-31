# Project
cmake_minimum_required(VERSION 3.16 FATAL_ERROR)
project(libtgchat LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

# Ccache
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
endif()

# Compiler specifics
set(ATOMIC_TEST_SOURCE
  "
  #include <atomic>
  #include <cstdint>

  std::atomic<int> x;
  std::atomic<uint64_t> y(0);

  int main()
  {
    uint64_t i = y.load(std::memory_order_relaxed);
    return std::atomic_is_lock_free(&y) && std::atomic_is_lock_free(&x);
  }
  ")
check_cxx_source_compiles("${ATOMIC_TEST_SOURCE}" HAVE_BUILTIN_ATOMIC)
if (NOT HAVE_BUILTIN_ATOMIC)
  message(STATUS "No built-in atomic, using libatomic")
  set (USE_LIBATOMIC TRUE)
else()
  message(STATUS "Using built-in atomic")
endif()

# Library
add_library(tgchat ${LIBRARY_LINKING_TYPE}
  src/tgchat.cpp
  src/tgchat.h
)
install(TARGETS tgchat DESTINATION ${CMAKE_INSTALL_LIBDIR})

# Common
target_common_config(tgchat)

# Telegram API credentials of braids (https://my.telegram.org), given at
# build time so they are not in the repository: -DTG_API_ID=... -DTG_API_HASH=...,
# the environment variables of the same name, or a git-ignored .env file in
# the repository root (TG_API_ID=..., TG_API_HASH=...). Without them nchat's
# credentials are used (development builds only).
set(TG_DOTENV "${CMAKE_SOURCE_DIR}/.env")
if(EXISTS "${TG_DOTENV}")
  # editing .env reconfigures the build
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${TG_DOTENV}")
  file(STRINGS "${TG_DOTENV}" TG_DOTENV_LINES REGEX "^[ \t]*(export[ \t]+)?TG_API_(ID|HASH)[ \t]*=")
  foreach(TG_DOTENV_LINE ${TG_DOTENV_LINES})
    string(REGEX REPLACE "^[ \t]*(export[ \t]+)?(TG_API_[A-Z]+)[ \t]*=[ \t]*[\"']?([^\"' \t#]*).*$" "\\2" TG_KEY "${TG_DOTENV_LINE}")
    string(REGEX REPLACE "^[ \t]*(export[ \t]+)?(TG_API_[A-Z]+)[ \t]*=[ \t]*[\"']?([^\"' \t#]*).*$" "\\3" TG_VALUE "${TG_DOTENV_LINE}")
    if(NOT ${TG_KEY} AND NOT DEFINED ENV{${TG_KEY}})
      set(${TG_KEY} "${TG_VALUE}")
    endif()
  endforeach()
endif()
if(NOT TG_API_ID AND DEFINED ENV{TG_API_ID})
  set(TG_API_ID "$ENV{TG_API_ID}")
endif()
if(NOT TG_API_HASH AND DEFINED ENV{TG_API_HASH})
  set(TG_API_HASH "$ENV{TG_API_HASH}")
endif()
if(TG_API_ID AND TG_API_HASH)
  message(STATUS "Telegram API credentials: braids (from build configuration)")
  target_compile_definitions(tgchat PRIVATE TG_API_ID="${TG_API_ID}" TG_API_HASH="${TG_API_HASH}")
else()
  message(STATUS "Telegram API credentials: nchat fallback (set TG_API_ID / TG_API_HASH for releases)")
endif()

# Headers
target_include_directories(tgchat PRIVATE "../common/src")
target_include_directories(tgchat PRIVATE "../ncutil/src")
target_include_directories(tgchat PRIVATE "ext/td")

# Dependency tdlib
add_subdirectory(ext/td EXCLUDE_FROM_ALL)

if(LIBRARY_LINKING_TYPE STREQUAL "SHARED")
  set(TDLIBNAME "tdclientshared")
  set(TDLIBFILENAME "libtdclientshared${CMAKE_SHARED_LIBRARY_SUFFIX}")
  install(FILES ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/${TDLIBFILENAME} DESTINATION ${CMAKE_INSTALL_LIBDIR})
else()
  set(TDLIBNAME "tdclient")
endif()

# Linking
target_link_libraries(tgchat PUBLIC ncutil ${TDLIBNAME})
if (USE_LIBATOMIC)
  set_target_properties(tgchat PROPERTIES LINK_FLAGS "-latomic")
endif()

# Compiler flags
set_target_properties(tgchat PROPERTIES COMPILE_FLAGS
                      "-Wall -Wextra -Wpedantic -Wshadow -Wpointer-arith \
                       -Wcast-qual -Wno-missing-braces -Wswitch-default \
                       -Wunreachable-code -Wundef -Wuninitialized \
                       -Wcast-align")
