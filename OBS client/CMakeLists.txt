cmake_minimum_required(VERSION 3.16)
project(OBSClients VERSION 0.2 LANGUAGES CXX)
if(NOT WIN32)
    message(FATAL_ERROR "OBS clients use Windows WGC/WinRT/WASAPI. Build Linux servers from OBS server/code instead.")
endif()
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

# The helper provides the installed compiler's locale prefix when older CMake
# versions decode localized /showIncludes output incorrectly on Windows.
if(MSVC AND DEFINED OBS_MSVC_INCLUDE_PREFIX)
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${OBS_MSVC_INCLUDE_PREFIX}")
endif()

add_library(OBSNetwork STATIC HttpClient.cpp HttpClient.h SignalClient.cpp SignalClient.h)
target_include_directories(OBSNetwork PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(OBSNetwork PUBLIC Qt6::Network)

add_library(OBSClientCore STATIC
    mainwindow.cpp mainwindow.h mainwindow.ui
    PreviewWindow.cpp PreviewWindow.h
    VideoRenderer.cpp VideoRenderer.h
    LoginModel.cpp LoginModel.h
    LoginController.cpp LoginController.h
    ClientRole.h SessionModel.cpp SessionModel.h SessionController.cpp SessionController.h
    CaptureSource.h VideoCapture.cpp VideoCapture.h WasapiCapture.cpp WasapiCapture.h
)
target_include_directories(OBSClientCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(OBSClientCore PUBLIC Qt6::Widgets OBSNetwork d3d11 dxgi d3dcompiler windowsapp ole32 uuid)
qt_add_executable(OBS_Publisher main.cpp)
qt_add_executable(OBS_Player main.cpp)
target_compile_definitions(OBS_Player PRIVATE OBS_PLAYER)
foreach(client OBS_Publisher OBS_Player)
    target_link_libraries(${client} PRIVATE OBSClientCore)
    set_target_properties(${client} PROPERTIES WIN32_EXECUTABLE TRUE)
endforeach()

include(CTest)
option(OBS_LIVE_TESTS "Test against running local Docker services (8080/9000)" OFF)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(CSNLoginTests ../tests/LoginTests.cpp LocalAuthServer.cpp LocalAuthServer.h)
    target_link_libraries(CSNLoginTests PRIVATE OBSClientCore Qt6::Test)
    add_test(NAME login.http_mvc COMMAND CSNLoginTests)
    set_tests_properties(login.http_mvc PROPERTIES TIMEOUT 20)
    add_executable(OBSClientTests ../tests/ClientTests.cpp)
    target_link_libraries(OBSClientTests PRIVATE OBSClientCore Qt6::Test)
    add_test(NAME client.protocol_mvc COMMAND OBSClientTests)
    set_tests_properties(client.protocol_mvc PROPERTIES TIMEOUT 20)
    add_executable(OBSCaptureTests ../tests/CaptureTests.cpp)
    target_link_libraries(OBSCaptureTests PRIVATE OBSClientCore Qt6::Test winmm)
    add_test(NAME client.capture COMMAND OBSCaptureTests)
    # Native capture/COM tests use the Windows platform plugin; no window is shown by unit tests.
    set_tests_properties(client.capture PROPERTIES TIMEOUT 40 ENVIRONMENT "QT_QPA_PLATFORM=windows")
    if(OBS_LIVE_TESTS)
        add_test(NAME client.live_services COMMAND OBSClientTests --integration)
        set_tests_properties(client.live_services PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
    endif()
endif()
if(MSVC)
    target_compile_options(OBSNetwork PRIVATE /utf-8 /W4)
    target_compile_options(OBSClientCore PRIVATE /utf-8 /W4)
    target_compile_options(OBS_Publisher PRIVATE /utf-8 /W4)
    target_compile_options(OBS_Player PRIVATE /utf-8 /W4)
    if(BUILD_TESTING)
        target_compile_options(CSNLoginTests PRIVATE /utf-8 /W4)
        target_compile_options(OBSClientTests PRIVATE /utf-8 /W4)
        target_compile_options(OBSCaptureTests PRIVATE /utf-8 /W4)
    endif()
endif()
include(GNUInstallDirs)
install(TARGETS OBS_Publisher OBS_Player RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
