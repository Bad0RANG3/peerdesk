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

add_library(peerdesk_server STATIC
    src/server/server.cpp
    src/server/server.h
)
target_link_libraries(peerdesk_server
    PUBLIC peerdesk_core Qt6::Core Qt6::Network
)

add_library(peerdesk_transport STATIC
    src/transport/peer_link.cpp
    src/transport/peer_link.h
)
target_link_libraries(peerdesk_transport
    PUBLIC peerdesk_core Qt6::Core Qt6::Network
)

add_library(peerdesk_client STATIC
    src/client/signaling_client.cpp
    src/client/signaling_client.h
)
target_link_libraries(peerdesk_client
    PUBLIC peerdesk_transport Qt6::Core Qt6::Network
)

add_library(peerdesk_session STATIC
    src/session/session_types.cpp
    src/session/session_types.h
    src/session/media_sources.cpp
    src/session/media_sources.h
    src/session/audio_codec.cpp
    src/session/audio_codec.h
    src/session/file_transfer.cpp
    src/session/file_transfer.h
    src/session/host_session.cpp
    src/session/host_session.h
    src/session/viewer_session.cpp
    src/session/viewer_session.h
)
target_compile_definitions(peerdesk_session PRIVATE
    PEERDESK_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(peerdesk_session
    PUBLIC peerdesk_transport Qt6::Core Qt6::Gui
)

if(WIN32)
    add_library(peerdesk_platform STATIC
        src/platform/pcm_resampler.cpp
        src/platform/pcm_resampler.h
        src/platform/wasapi_audio.cpp
        src/platform/wasapi_audio.h
        src/platform/windows_clipboard.cpp
        src/platform/windows_clipboard.h
        src/platform/windows_input.cpp
        src/platform/windows_input.h
        src/platform/windows_screen_source.cpp
        src/platform/windows_screen_source.h
    )
    target_link_libraries(peerdesk_platform
        PUBLIC peerdesk_session Qt6::Core Qt6::Gui
        PRIVATE ole32 uuid
    )

    target_compile_definitions(peerdesk_core PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
    )
    target_link_libraries(peerdesk_core PUBLIC ws2_32 iphlpapi)
endif()

if(MSVC)
    target_compile_options(peerdesk_core PRIVATE /W4 /permissive-)
    target_compile_options(peerdesk_server PRIVATE /W4 /permissive-)
    target_compile_options(peerdesk_transport PRIVATE /W4 /permissive-)
    target_compile_options(peerdesk_client PRIVATE /W4 /permissive-)
    target_compile_options(peerdesk_session PRIVATE /W4 /permissive-)
    target_compile_options(peerdesk_platform PRIVATE /W4 /permissive-)
else()
    target_compile_options(peerdesk_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(peerdesk_server PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(peerdesk_transport PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(peerdesk_client PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(peerdesk_session PRIVATE -Wall -Wextra -Wpedantic)
    if(TARGET peerdesk_platform)
        target_compile_options(peerdesk_platform PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

add_executable(peerdesk-server
    apps/peerdesk-server/main.cpp
)
target_compile_definitions(peerdesk-server PRIVATE
    PEERDESK_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(peerdesk-server PRIVATE peerdesk_server)

add_executable(peerdesk-link-harness
    tools/link-harness/main.cpp
)
target_link_libraries(peerdesk-link-harness PRIVATE peerdesk_transport)

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

add_executable(peerdesk-server-tests
    tests/server_test.cpp
)
target_link_libraries(peerdesk-server-tests
    PRIVATE peerdesk_server Qt6::Test
)

add_executable(peerdesk-peer-link-tests
    tests/peer_link_test.cpp
)
target_link_libraries(peerdesk-peer-link-tests
    PRIVATE peerdesk_transport peerdesk_server Qt6::Test
)

add_executable(peerdesk-session-tests
    tests/session_test.cpp
)
target_link_libraries(peerdesk-session-tests
    PRIVATE peerdesk_session peerdesk_server Qt6::Test
)

add_executable(peerdesk-signaling-client-tests
    tests/signaling_client_test.cpp
)
target_link_libraries(peerdesk-signaling-client-tests
    PRIVATE peerdesk_client peerdesk_server Qt6::Test
)

if(WIN32)
    add_executable(peerdesk-platform-tests
        tests/platform_test.cpp
    )
    target_link_libraries(peerdesk-platform-tests
        PRIVATE peerdesk_platform Qt6::Test
    )
endif()

enable_testing()
add_test(NAME peerdesk-core COMMAND peerdesk-tests)
add_test(NAME peerdesk-crypto COMMAND peerdesk-crypto-tests)
add_test(NAME peerdesk-transport COMMAND peerdesk-transport-tests)
add_test(NAME peerdesk-stun COMMAND peerdesk-stun-tests)
add_test(NAME peerdesk-server COMMAND peerdesk-server-tests)
add_test(NAME peerdesk-peer-link COMMAND peerdesk-peer-link-tests)
add_test(NAME peerdesk-session COMMAND peerdesk-session-tests)
add_test(NAME peerdesk-signaling-client COMMAND peerdesk-signaling-client-tests)
if(WIN32)
    add_test(NAME peerdesk-platform COMMAND peerdesk-platform-tests)
endif()

install(TARGETS
    peerdesk-server
    peerdesk-tests
    peerdesk-crypto-tests
    peerdesk-transport-tests
    peerdesk-stun-tests
    peerdesk-server-tests
    peerdesk-peer-link-tests
    peerdesk-session-tests
    peerdesk-signaling-client-tests
    RUNTIME DESTINATION bin)

if(WIN32)
    install(TARGETS peerdesk-platform-tests RUNTIME DESTINATION bin)
endif()
