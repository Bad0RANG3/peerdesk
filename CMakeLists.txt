cmake_minimum_required(VERSION 3.24)

project(PeerDesk
    VERSION 0.7.0
    DESCRIPTION "PeerDesk remote desktop"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network Multimedia Test)
qt_standard_project_setup()

add_library(monocypher STATIC
    third_party/monocypher/monocypher.c
)
target_include_directories(monocypher PUBLIC third_party/monocypher)
set_target_properties(monocypher PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

add_library(peerdesk_core STATIC
    src/core/crypto.cpp
    src/core/crypto.h
    src/core/protocol.cpp
    src/core/protocol.h
    src/core/reliable.cpp
    src/core/reliable.h
    src/core/stun.cpp
    src/core/stun.h
    src/core/util.cpp
    src/core/util.h
    src/core/video.cpp
    src/core/video.h
)

target_include_directories(peerdesk_core PUBLIC src)
target_link_libraries(peerdesk_core PUBLIC Qt6::Core Qt6::Network PRIVATE monocypher)

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

add_executable(peerdesk-crypto-tests
    tests/crypto_test.cpp
)
target_link_libraries(peerdesk-crypto-tests PRIVATE peerdesk_core Qt6::Test)

add_executable(peerdesk-transport-tests
    tests/transport_test.cpp
)
target_link_libraries(peerdesk-transport-tests PRIVATE peerdesk_core Qt6::Test)

add_executable(peerdesk-stun-tests
    tests/stun_test.cpp
)
target_link_libraries(peerdesk-stun-tests PRIVATE peerdesk_core Qt6::Test)

enable_testing()
add_test(NAME peerdesk-core COMMAND peerdesk-tests)
add_test(NAME peerdesk-crypto COMMAND peerdesk-crypto-tests)
add_test(NAME peerdesk-transport COMMAND peerdesk-transport-tests)
add_test(NAME peerdesk-stun COMMAND peerdesk-stun-tests)

install(TARGETS
    peerdesk-tests
    peerdesk-crypto-tests
    peerdesk-transport-tests
    peerdesk-stun-tests
    RUNTIME DESTINATION bin)
