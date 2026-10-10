#include "FacePreprocess.h"
#include <cuda_runtime.h>

namespace {
// 点采样再手动双线性插值，采用 OpenCV 的半像素中心和边缘钳制规则。
__device__ float channel(cudaTextureObject_t texture, int x, int y, int c)
{
    const uchar4 pixel = tex2D<uchar4>(texture, float(x) + 0.5f, float(y) + 0.5f);
    return c == 0 ? float(pixel.z) : c == 1 ? float(pixel.y) : float(pixel.x); // BGRA → RGB。
}
__global__ void preprocess(cudaTextureObject_t texture, int width, int height, int rw, int rh, float *output)
{
    const int x = int(blockIdx.x * blockDim.x + threadIdx.x), y = int(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= 640 || y >= 640) return;
    // 1. 右/下补黑；黑色也按训练输入规则归一化。
    for (int c = 0; c < 3; ++c) {
        float value = 0;
        if (x < rw && y < rh) {
            // 2. 等比缩放，实际 rw/rh 与 CPU 对照使用相同的整数尺寸。
            const float sx = fmaxf(0, fminf((float(x) + 0.5f) * width / rw - 0.5f, float(width - 1)));
            const float sy = fmaxf(0, fminf((float(y) + 0.5f) * height / rh - 0.5f, float(height - 1)));
            const int ix = int(floorf(sx)), iy = int(floorf(sy));
            const float ax = sx - ix, ay = sy - iy;
            const float top = channel(texture, ix, iy, c) * (1 - ax) + channel(texture, min(ix + 1, width - 1), iy, c) * ax;
            const float bottom = channel(texture, ix, min(iy + 1, height - 1), c) * (1 - ax) + channel(texture, min(ix + 1, width - 1), min(iy + 1, height - 1), c) * ax;
            value = floorf(top * (1 - ay) + bottom * ay + 0.5f); // 对齐 CPU uint8 resize 的像素量化。
        }
        // 3. 写入连续显存张量；没有视频像素 D2H/H2D 往返。
        output[c * 640 * 640 + y * 640 + x] = (value - 127.5f) / 128.0f;
    }
}
}
cudaError_t launchFacePreprocess(cudaTextureObject_t texture, int width, int height,
    int resizedWidth, int resizedHeight, float *output, cudaStream_t stream)
{
    preprocess<<<dim3(40, 40), dim3(16, 16), 0, stream>>>(texture, width, height, resizedWidth, resizedHeight, output);
    return cudaGetLastError(); // 执行期错误由工作线程同步 stream 后统一报告。
}
