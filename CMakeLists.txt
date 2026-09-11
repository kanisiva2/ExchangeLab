cmake_minimum_required(VERSION 3.24)

project(
    ExchangeLab
    VERSION 0.1.0
    DESCRIPTION "A local C++ market-data systems laboratory"
    LANGUAGES CXX
)

function(exchangelab_enable_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4)
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(exchangelab_core
    src/market_state.cpp
    src/simulator.cpp
)

target_include_directories(exchangelab_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(exchangelab_core PUBLIC cxx_std_20)
exchangelab_enable_warnings(exchangelab_core)

add_executable(exchangelab src/main.cpp)
target_link_libraries(exchangelab PRIVATE exchangelab_core)
exchangelab_enable_warnings(exchangelab)

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
        tests/market_state_test.cpp
        tests/simulator_test.cpp
    )
    target_link_libraries(exchangelab_tests
        PRIVATE
            exchangelab_core
            GTest::gtest_main
    )
    exchangelab_enable_warnings(exchangelab_tests)

    include(GoogleTest)
    gtest_discover_tests(exchangelab_tests)
endif()
