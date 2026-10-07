cmake_minimum_required(VERSION 3.20)
project(OBS_Server VERSION 0.1 LANGUAGES CXX)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "OBS 服务端网络库使用 Linux epoll，请在 OBS 容器内构建。")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)

# 公共网络库与业务无关。后续登录、调度、信令目标都链接 obs_net。
add_library(obs_net STATIC
    net/UniqueFd.h net/Buffer.h net/Buffer.cpp
    net/Channel.h net/Channel.cpp net/Poller.h net/Poller.cpp
    net/EventLoop.h net/EventLoop.cpp net/Acceptor.h net/Acceptor.cpp
    net/TcpConnection.h net/TcpConnection.cpp net/TcpServer.h net/TcpServer.cpp)
target_include_directories(obs_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/net)
target_link_libraries(obs_net PUBLIC Threads::Threads)
target_compile_options(obs_net PRIVATE -Wall -Wextra -Wpedantic -Werror)

option(OBS_ENABLE_SANITIZERS "启用 ASan 和 UBSan" OFF)
if(OBS_ENABLE_SANITIZERS)
    target_compile_options(obs_net PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(obs_net PUBLIC -fsanitize=address,undefined)
endif()
include(CTest)
if(BUILD_TESTING)
    add_executable(obs_net_tests tests/NetworkTests.cpp)
    target_link_libraries(obs_net_tests PRIVATE obs_net)
    target_compile_options(obs_net_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME network.behavior COMMAND obs_net_tests)
    set_tests_properties(network.behavior PROPERTIES TIMEOUT 30)
endif()
