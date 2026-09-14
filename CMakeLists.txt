cmake_minimum_required(VERSION 3.24)

project(
    ExchangeLab
    VERSION 0.1.0
    DESCRIPTION "A local C++ market-data systems laboratory"
    LANGUAGES CXX
)

option(EXCHANGELAB_ENABLE_THREAD_SANITIZER
       "Build with ThreadSanitizer instrumentation" OFF)

if(EXCHANGELAB_ENABLE_THREAD_SANITIZER)
    if(MSVC)
        message(FATAL_ERROR "ThreadSanitizer is not supported by this CMake option on MSVC")
    endif()
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

function(exchangelab_enable_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4)
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(exchangelab_core
    src/concurrent_market_state.cpp
    src/market_state.cpp
    src/query_worker_pool.cpp
    src/simulator.cpp
)

target_include_directories(exchangelab_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(exchangelab_core PUBLIC cxx_std_20)
exchangelab_enable_warnings(exchangelab_core)

include(FetchContent)

FetchContent_Declare(
    asio
    URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-38-2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(asio)

find_package(Threads REQUIRED)
add_library(exchangelab_asio INTERFACE)
target_include_directories(exchangelab_asio
    INTERFACE
        ${asio_SOURCE_DIR}/include
)
target_compile_definitions(exchangelab_asio INTERFACE ASIO_STANDALONE)
target_link_libraries(exchangelab_asio INTERFACE Threads::Threads)

add_library(exchangelab_network
    src/exchange_publisher.cpp
    src/market_server.cpp
    src/query_client.cpp
    src/query_protocol.cpp
    src/update_codec.cpp
    src/update_stream_decoder.cpp
)
target_link_libraries(exchangelab_network
    PUBLIC
        exchangelab_core
        exchangelab_asio
)
target_compile_features(exchangelab_network PUBLIC cxx_std_20)
exchangelab_enable_warnings(exchangelab_network)

add_executable(exchangelab-server src/server_main.cpp)
target_link_libraries(exchangelab-server PRIVATE exchangelab_network)
exchangelab_enable_warnings(exchangelab-server)

add_executable(exchange-simulator src/exchange_simulator_main.cpp)
target_link_libraries(exchange-simulator PRIVATE exchangelab_network)
exchangelab_enable_warnings(exchange-simulator)

add_executable(exchangelab-client src/client_main.cpp)
target_link_libraries(exchangelab-client PRIVATE exchangelab_network)
exchangelab_enable_warnings(exchangelab-client)

include(CTest)

if(BUILD_TESTING)
    find_package(GTest CONFIG QUIET)

    if(NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    add_executable(exchangelab_tests
        tests/concurrent_market_state_test.cpp
        tests/market_state_test.cpp
        tests/query_protocol_test.cpp
        tests/query_worker_pool_test.cpp
        tests/simulator_test.cpp
        tests/tcp_system_test.cpp
        tests/update_codec_test.cpp
        tests/update_stream_decoder_test.cpp
    )
    target_link_libraries(exchangelab_tests
        PRIVATE
            exchangelab_network
            GTest::gtest_main
    )
    exchangelab_enable_warnings(exchangelab_tests)

    include(GoogleTest)
    # PRE_TEST avoids executing a sanitizer-instrumented binary while CMake is
    # still building it. Discovery instead happens immediately before ctest.
    gtest_discover_tests(exchangelab_tests DISCOVERY_MODE PRE_TEST)
endif()
