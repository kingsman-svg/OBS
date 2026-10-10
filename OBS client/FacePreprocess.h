#pragma once
#include <cuda_runtime_api.h>

// 唯一 CUDA kernel 入口；纹理为 BGRA8，输出固定 Float32 RGB NCHW [1,3,640,640]。
cudaError_t launchFacePreprocess(cudaTextureObject_t texture, int width, int height,
    int resizedWidth, int resizedHeight, float *output, cudaStream_t stream);
