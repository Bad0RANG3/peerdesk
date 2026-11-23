cmake_minimum_required(VERSION 3.24)

project(PeerDesk
    VERSION 0.9.0
    DESCRIPTION "PeerDesk remote desktop"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

if(WIN32)
    enable_language(RC)
endif()

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

add_library(peerdesk_diagnostics STATIC
    src/diagnostics/diagnostic_bundle.cpp
    src/diagnostics/diagnostic_bundle.h
    src/diagnostics/logging.cpp
    src/diagnostics/logging.h
)
target_include_directories(peerdesk_diagnostics PUBLIC src)
target_link_libraries(peerdesk_diagnostics PUBLIC Qt6::Core)

add_library(peerdesk_server STATIC
    src/server/server.cpp
    src/server/server.h
)
target_link_libraries(peerdesk_server
    PUBLIC peerdesk_core Qt6::Core Qt6::Network
)

add_library(peerdesk_transport STATIC
    src/transport/authenticated_packet_pipeline.cpp
    src/transport/authenticated_packet_pipeline.h
    src/transport/link_feedback_pipeline.cpp
    src/transport/link_feedback_pipeline.h
    src/transport/peer_link.cpp
    src/transport/peer_link.h
    src/transport/udp_io_pipeline.cpp
    src/transport/udp_io_pipeline.h
    src/transport/video_receive_pipeline.cpp
    src/transport/video_receive_pipeline.h
    src/transport/video_send_pipeline.cpp
    src/transport/video_send_pipeline.h
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
    src/session/file_transfer_pipeline.cpp
    src/session/file_transfer_pipeline.h
    src/session/video_pipeline.cpp
    src/session/video_pipeline.h
    src/session/host_input_pipeline.cpp
    src/session/host_input_pipeline.h
    src/session/viewer_audio_pipeline.cpp
    src/session/viewer_audio_pipeline.h
    src/session/viewer_video_pipeline.cpp
    src/session/viewer_video_pipeline.h
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

add_library(peerdesk_app STATIC
    src/app/quality_presets.cpp
    src/app/quality_presets.h
    src/app/preferences.cpp
    src/app/preferences.h
    src/app/single_instance.cpp
    src/app/single_instance.h
    src/app/audio_output.cpp
    src/app/audio_output.h
    src/app/diagnostic_service.cpp
    src/app/diagnostic_service.h
    src/app/host_controller.cpp
    src/app/host_controller.h
    src/app/viewer_controller.cpp
    src/app/viewer_controller.h
)
target_link_libraries(peerdesk_app
    PUBLIC peerdesk_client peerdesk_session
        peerdesk_diagnostics
        Qt6::Core Qt6::Gui Qt6::Multimedia
)

add_library(peerdesk_ui STATIC
    src/ui/app_window.cpp
    src/ui/app_window.h
    src/ui/code_input.cpp
    src/ui/code_input.h
    src/ui/connection_page.cpp
    src/ui/connection_page.h
    src/ui/host_panel.cpp
    src/ui/host_panel.h
    src/ui/outgoing_file_queue.cpp
    src/ui/outgoing_file_queue.h
    src/ui/settings_dialog.cpp
    src/ui/settings_dialog.h
    src/ui/ui_theme.cpp
    src/ui/ui_theme.h
    src/ui/ui_widgets.cpp
    src/ui/ui_widgets.h
    src/ui/viewer_frame_pipeline.cpp
    src/ui/viewer_frame_pipeline.h
    src/ui/viewer_page.cpp
    src/ui/viewer_page.h
)
target_link_libraries(peerdesk_ui
    PUBLIC peerdesk_app Qt6::Core Qt6::Gui Qt6::Widgets
)
set_target_properties(peerdesk_ui PROPERTIES AUTOUIC OFF)

if(WIN32)
    add_library(peerdesk_platform STATIC
        src/platform/pcm_resampler.cpp
        src/platform/pcm_resampler.h
        src/platform/startup_registration.h
        src/platform/wasapi_audio.cpp
        src/platform/wasapi_audio.h
        src/platform/windows_clipboard.cpp
        src/platform/windows_clipboard.h
        src/platform/windows_input.cpp
        src/platform/windows_input.h
        src/platform/windows_screen_source.cpp
        src/platform/windows_screen_source.h
        src/platform/windows_startup_registration.cpp
        src/platform/windows_startup_registration.h
    )
    target_link_libraries(peerdesk_platform
        PUBLIC peerdesk_session Qt6::Core Qt6::Gui
        PRIVATE ole32 uuid ksguid
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
    target_compile_options(peerdesk_diagnostics PRIVATE /W4 /permissive-)
    target_compile_options(peerdesk_server PRIVATE /W4 /permissive-)
    target_compile_options(peerdesk_transport PRIVATE /W4 /permissive-)
    target_compile_options(peerdesk_client PRIVATE /W4 /permissive-)
    target_compile_options(peerdesk_session PRIVATE /W4 /permissive-)
    target_compile_options(peerdesk_app PRIVATE /W4 /permissive-)
    target_compile_options(peerdesk_ui PRIVATE /W4 /permissive-)
    target_compile_options(peerdesk_platform PRIVATE /W4 /permissive-)
else()
    target_compile_options(peerdesk_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(peerdesk_diagnostics PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(peerdesk_server PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(peerdesk_transport PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(peerdesk_client PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(peerdesk_session PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(peerdesk_app PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(peerdesk_ui PRIVATE -Wall -Wextra -Wpedantic)
    if(TARGET peerdesk_platform)
        target_compile_options(peerdesk_platform PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(WIN32)
    set(PEERDESK_RC_DESCRIPTION "PeerDesk signaling and relay server")
    set(PEERDESK_RC_INTERNAL_NAME "peerdesk-server")
    set(PEERDESK_RC_ORIGINAL_FILENAME "peerdesk-server.exe")
    configure_file(
        packaging/windows/version.rc.in
        "${CMAKE_CURRENT_BINARY_DIR}/peerdesk-server.rc"
        @ONLY
    )
    set(PEERDESK_SERVER_RC
        "${CMAKE_CURRENT_BINARY_DIR}/peerdesk-server.rc")

    set(PEERDESK_RC_DESCRIPTION "PeerDesk remote desktop client")
    set(PEERDESK_RC_INTERNAL_NAME "peerdesk-client")
    set(PEERDESK_RC_ORIGINAL_FILENAME "peerdesk-client.exe")
    configure_file(
        packaging/windows/version.rc.in
        "${CMAKE_CURRENT_BINARY_DIR}/peerdesk-client.rc"
        @ONLY
    )
    set(PEERDESK_CLIENT_RC
        "${CMAKE_CURRENT_BINARY_DIR}/peerdesk-client.rc")
endif()

add_executable(peerdesk-server
    apps/peerdesk-server/main.cpp
    ${PEERDESK_SERVER_RC}
)
target_compile_definitions(peerdesk-server PRIVATE
    PEERDESK_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(peerdesk-server PRIVATE
    peerdesk_server
    peerdesk_diagnostics
)

if(WIN32)
    add_executable(peerdesk-client WIN32
        apps/peerdesk-client/main.cpp
        ${PEERDESK_CLIENT_RC}
    )
else()
    add_executable(peerdesk-client
        apps/peerdesk-client/main.cpp
    )
endif()
target_compile_definitions(peerdesk-client PRIVATE
    PEERDESK_VERSION="${PROJECT_VERSION}"
)
set_target_properties(peerdesk-client PROPERTIES AUTOUIC OFF)
target_link_libraries(peerdesk-client PRIVATE
    peerdesk_ui
    peerdesk_diagnostics
    Qt6::Widgets
)
if(WIN32)
    target_link_libraries(peerdesk-client PRIVATE peerdesk_platform)
endif()

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

add_executable(peerdesk-app-controller-tests
    tests/app_controller_test.cpp
)
target_link_libraries(peerdesk-app-controller-tests
    PRIVATE peerdesk_app peerdesk_server Qt6::Test
)

add_executable(peerdesk-audio-output-tests
    tests/audio_output_test.cpp
)
target_link_libraries(peerdesk-audio-output-tests
    PRIVATE peerdesk_app Qt6::Test
)

add_executable(peerdesk-single-instance-tests
    tests/single_instance_test.cpp
)
target_link_libraries(peerdesk-single-instance-tests
    PRIVATE peerdesk_app Qt6::Test
)

add_executable(peerdesk-single-instance-host
    tests/single_instance_host.cpp
)
target_link_libraries(peerdesk-single-instance-host
    PRIVATE peerdesk_app
)

add_executable(peerdesk-ui-tests
    tests/ui_test.cpp
)
set_target_properties(peerdesk-ui-tests PROPERTIES AUTOUIC OFF)
target_link_libraries(peerdesk-ui-tests
    PRIVATE peerdesk_ui Qt6::Test
)

add_executable(peerdesk-logging-tests
    tests/logging_test.cpp
)
target_link_libraries(peerdesk-logging-tests
    PRIVATE peerdesk_app peerdesk_diagnostics Qt6::Test
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
add_test(NAME peerdesk-app-controller COMMAND peerdesk-app-controller-tests)
add_test(NAME peerdesk-audio-output COMMAND peerdesk-audio-output-tests)
add_test(NAME peerdesk-single-instance COMMAND peerdesk-single-instance-tests)
add_test(NAME peerdesk-ui COMMAND peerdesk-ui-tests)
add_test(NAME peerdesk-logging COMMAND peerdesk-logging-tests)
set_tests_properties(peerdesk-single-instance PROPERTIES
    ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1")
set_tests_properties(peerdesk-ui PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
if(WIN32)
    add_test(NAME peerdesk-platform COMMAND peerdesk-platform-tests)
endif()

install(TARGETS
    peerdesk-client
    peerdesk-server
    RUNTIME DESTINATION ".")

install(FILES
    LICENSE
    README.md
    packaging/windows/USER-GUIDE.txt
    DESTINATION ".")
