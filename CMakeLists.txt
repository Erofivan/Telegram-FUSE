cmake_minimum_required(VERSION 3.16)
project(tgfs CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(FUSE REQUIRED fuse3)

find_package(CURL REQUIRED)

add_executable(tgfs
    src/main.cpp
    src/tgfs.cpp
    src/telegram_client.cpp
)

target_include_directories(tgfs PRIVATE
    src/
    ${FUSE_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

target_compile_options(tgfs PRIVATE
    ${FUSE_CFLAGS_OTHER}
    -Wall -Wextra -O2
)

target_link_libraries(tgfs PRIVATE
    ${FUSE_LIBRARIES}
    ${CURL_LIBRARIES}
)

target_compile_definitions(tgfs PRIVATE
    FUSE_USE_VERSION=35
    _FILE_OFFSET_BITS=64
)
