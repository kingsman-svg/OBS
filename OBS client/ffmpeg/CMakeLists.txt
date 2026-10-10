# 编码/封装独立为一级目录与库；捕获、同步和GPU检测仍在OBSMediaCore。
add_library(OBSFFmpegCore STATIC
    GpuVideoOutput.cpp GpuVideoOutput.h
    MediaEncoder.cpp MediaEncoder.h
    MediaPublisher.cpp MediaPublisher.h)
target_include_directories(OBSFFmpegCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(OBSFFmpegCore PUBLIC OBSMediaCore
    "${OBS_FFMPEG_ROOT}/lib/avcodec.lib" "${OBS_FFMPEG_ROOT}/lib/avformat.lib")
if(MSVC)
    target_compile_options(OBSFFmpegCore PRIVATE /utf-8 /W4)
endif()
