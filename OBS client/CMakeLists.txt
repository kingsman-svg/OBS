cmake_minimum_required(VERSION 3.24)
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

# 实时客户端只链接 TensorRT runtime；优化工程的 OpenCV/ORT/parser 不进入 OBS。
set(OBS_TENSORRT_ROOT "$ENV{TENSORRT_ROOT}" CACHE PATH "TensorRT 11 SDK root")
if(NOT EXISTS "${OBS_TENSORRT_ROOT}/include/NvInfer.h")
    message(FATAL_ERROR "Set OBS_TENSORRT_ROOT (or TENSORRT_ROOT) to the TensorRT 11 SDK.")
endif()
if(NOT CMAKE_CUDA_COMPILER AND EXISTS "$ENV{CUDA_PATH}/bin/nvcc.exe")
    set(CMAKE_CUDA_COMPILER "$ENV{CUDA_PATH}/bin/nvcc.exe" CACHE FILEPATH "CUDA compiler")
endif()
set(CMAKE_CUDA_ARCHITECTURES 120 CACHE STRING "RTX 5060 CUDA architecture")
enable_language(CUDA)
find_package(CUDAToolkit 13 REQUIRED)
find_library(OBS_TENSORRT_LIBRARY NAMES nvinfer_11 PATHS "${OBS_TENSORRT_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
find_file(OBS_CUDART_DLL NAMES cudart64_13.dll PATHS "${CUDAToolkit_BIN_DIR}" "$ENV{CUDA_PATH}/bin/x64" "$ENV{CUDA_PATH}/bin" NO_DEFAULT_PATH REQUIRED)
add_library(OBSFaceGpu STATIC FacePreprocess.cu FacePreprocess.h)
set_target_properties(OBSFaceGpu PROPERTIES CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON AUTOMOC OFF AUTOUIC OFF)
target_include_directories(OBSFaceGpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(OBSFaceGpu SYSTEM PUBLIC "${OBS_TENSORRT_ROOT}/include")
target_link_libraries(OBSFaceGpu PUBLIC CUDA::cudart "${OBS_TENSORRT_LIBRARY}")
target_compile_options(OBSFaceGpu PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/utf-8>)

# 与 CourseStudio 使用同一 FFmpeg 7.x SDK；不把本机 DLL/导入库提交到 Git。
set(OBS_FFMPEG_ROOT "$ENV{FFMPEG_7_HOME}" CACHE PATH "FFmpeg 7.x shared SDK root")
if(NOT OBS_FFMPEG_ROOT)
    set(OBS_FFMPEG_ROOT "C:/dev/ffmpeg-7.1-full_build-shared" CACHE PATH "FFmpeg 7.x shared SDK root" FORCE)
endif()
foreach(dependency include/libswresample/swresample.h include/libavutil/channel_layout.h
        lib/swresample.lib lib/avutil.lib bin/swresample-5.dll bin/avutil-59.dll)
    if(NOT EXISTS "${OBS_FFMPEG_ROOT}/${dependency}")
        message(FATAL_ERROR "Missing ${OBS_FFMPEG_ROOT}/${dependency}. Set OBS_FFMPEG_ROOT to a FFmpeg 7.x shared SDK.")
    endif()
endforeach()
add_library(OBSFFmpeg INTERFACE)
target_include_directories(OBSFFmpeg INTERFACE "${OBS_FFMPEG_ROOT}/include")
target_link_libraries(OBSFFmpeg INTERFACE "${OBS_FFMPEG_ROOT}/lib/swresample.lib" "${OBS_FFMPEG_ROOT}/lib/avutil.lib")
# 所有当前目标共用一个输出目录；配置时复制两个实际链接的运行库。
configure_file("${OBS_FFMPEG_ROOT}/bin/swresample-5.dll" "${CMAKE_CURRENT_BINARY_DIR}/swresample-5.dll" COPYONLY)
configure_file("${OBS_FFMPEG_ROOT}/bin/avutil-59.dll" "${CMAKE_CURRENT_BINARY_DIR}/avutil-59.dll" COPYONLY)

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
    AudioPacket.h AudioResampler.cpp AudioResampler.h AudioMixer.cpp AudioMixer.h
    FaceDetection.h FaceDetection.cpp GpuFaceDetector.h GpuFaceDetector.cpp
)
target_include_directories(OBSClientCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(OBSClientCore PUBLIC Qt6::Widgets OBSNetwork OBSFFmpeg OBSFaceGpu d3d11 dxgi d3dcompiler windowsapp ole32 uuid)
qt_add_executable(OBS_Publisher main.cpp)
qt_add_executable(OBS_Player main.cpp)
target_compile_definitions(OBS_Player PRIVATE OBS_PLAYER)
foreach(client OBS_Publisher OBS_Player)
    target_link_libraries(${client} PRIVATE OBSClientCore)
    set_target_properties(${client} PROPERTIES WIN32_EXECUTABLE TRUE)
endforeach()

# 可重复构建时同步匹配的运行库和本机示例引擎；不提交二进制模型到 Git。
set(OBS_FACE_ENGINE "${CMAKE_CURRENT_SOURCE_DIR}/../cpp/out/scrfd_final_smoke.engine" CACHE FILEPATH "Locally validated SCRFD engine")
function(obs_deploy_face_runtime target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OBS_TENSORRT_ROOT}/bin/nvinfer_11.dll" "${OBS_CUDART_DLL}" "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
    if(EXISTS "${OBS_FACE_ENGINE}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/models"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OBS_FACE_ENGINE}" "$<TARGET_FILE_DIR:${target}>/models/scrfd_10g.engine"
            VERBATIM)
    endif()
endfunction()
obs_deploy_face_runtime(OBS_Publisher)
obs_deploy_face_runtime(OBS_Player)

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
    # Native capture/COM and SwapChain tests require an interactive Windows platform session.
    set_tests_properties(client.capture PROPERTIES TIMEOUT 40 ENVIRONMENT "QT_QPA_PLATFORM=windows")
    add_executable(OBSAudioTests ../tests/AudioTests.cpp)
    target_link_libraries(OBSAudioTests PRIVATE OBSClientCore Qt6::Test)
    add_test(NAME client.audio COMMAND OBSAudioTests)
    set_tests_properties(client.audio PROPERTIES TIMEOUT 20)
    add_executable(OBSFaceTests ../tests/FaceTests.cpp)
    target_link_libraries(OBSFaceTests PRIVATE OBSClientCore Qt6::Test)
    add_test(NAME client.face COMMAND OBSFaceTests)
    set_tests_properties(client.face PROPERTIES TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=windows")
    obs_deploy_face_runtime(OBSFaceTests)
    option(OBS_FACE_REFERENCE_TESTS "Compare GPU input with OpenCV CPU preprocessing in hardware tests" OFF)
    if(OBS_FACE_REFERENCE_TESTS)
        set(OBS_OPENCV_ROOT "C:/dev/opencv/opencv/build" CACHE PATH "OpenCV test SDK")
        find_package(OpenCV REQUIRED COMPONENTS core imgproc dnn PATHS "${OBS_OPENCV_ROOT}" NO_DEFAULT_PATH)
        target_sources(OBSFaceTests PRIVATE ../cpp/EngineSession.cpp)
        target_include_directories(OBSFaceTests PRIVATE ../cpp ${OpenCV_INCLUDE_DIRS})
        target_link_libraries(OBSFaceTests PRIVATE ${OpenCV_LIBS})
        target_compile_definitions(OBSFaceTests PRIVATE OBS_FACE_REFERENCE_TESTS)
        foreach(library IN LISTS OpenCV_LIBS)
            add_custom_command(TARGET OBSFaceTests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${library}>" "$<TARGET_FILE_DIR:OBSFaceTests>"
                VERBATIM)
        endforeach()
    endif()
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
        target_compile_options(OBSAudioTests PRIVATE /utf-8 /W4)
        target_compile_options(OBSFaceTests PRIVATE /utf-8 /W4)
    endif()
endif()
include(GNUInstallDirs)
install(TARGETS OBS_Publisher OBS_Player RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
