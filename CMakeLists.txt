cmake_minimum_required(VERSION 3.24)

project(PeerDesk
    VERSION 0.7.0
    DESCRIPTION "PeerDesk remote desktop"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network Multimedia Test)
qt_standard_project_setup()

add_library(peerdesk_core STATIC
    src/core/protocol.cpp
    src/core/protocol.h
    src/core/util.cpp
    src/core/util.h
)

target_include_directories(peerdesk_core PUBLIC src)
target_link_libraries(peerdesk_core PUBLIC Qt6::Core Qt6::Network)

if(WIN32)
    target_compile_definitions(peerdesk_core PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
    )
    target_link_libraries(peerdesk_core PUBLIC ws2_32 iphlpapi)
endif()

if(MSVC)
    target_compile_options(peerdesk_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(peerdesk_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(peerdesk-tests
    tests/protocol_test.cpp
)
target_link_libraries(peerdesk-tests PRIVATE peerdesk_core Qt6::Test)

enable_testing()
add_test(NAME peerdesk-core COMMAND peerdesk-tests)

install(TARGETS peerdesk-tests RUNTIME DESTINATION bin)
