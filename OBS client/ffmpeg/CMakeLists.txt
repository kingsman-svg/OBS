# 编码/封装及解封装/解码独立为一级目录与库；捕获、GPU检测、WASAPI输出仍在OBSMediaCore。
add_library(OBSFFmpegCore STATIC
    GpuVideoOutput.cpp GpuVideoOutput.h
    GpuVideoConverter.cpp GpuVideoConverter.h
    MediaEncoder.cpp MediaEncoder.h
    MediaPublisher.cpp MediaPublisher.h
    MediaDecoder.cpp MediaDecoder.h MediaPlayer.cpp MediaPlayer.h)
target_include_directories(OBSFFmpegCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(OBSFFmpegCore PUBLIC OBSMediaCore
    "${OBS_FFMPEG_ROOT}/lib/avcodec.lib" "${OBS_FFMPEG_ROOT}/lib/avformat.lib" "${OBS_FFMPEG_ROOT}/lib/swscale.lib")
if(MSVC)
    target_compile_options(OBSFFmpegCore PRIVATE /utf-8 /W4)
endif()
