cmake_minimum_required(VERSION 3.20)
project(localshare LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(localshare
    src/thread_pool.cpp
    src/http.cpp
    src/server.cpp
    src/main.cpp
)

target_include_directories(localshare PRIVATE include)

find_package(Threads REQUIRED)
target_link_libraries(localshare PRIVATE Threads::Threads)

target_compile_options(localshare PRIVATE -Wall -Wextra -Wpedantic -O2)
