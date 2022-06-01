/**
 * Copyright 2022 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "plugin/device/gpu/kernel/cuda_impl/cuda_ops/non_zero_impl.cuh"
#include <cub/cub.cuh>
#include <algorithm>

template <typename DataType>
struct IsZero {
  __host__ __device__ __forceinline__ size_t operator()(const DataType &x) const { return x == DataType(0) ? 0 : 1; }
};

template <typename IndexType>
__global__ void NonZeroKernel(const size_t *index_ptr, const size_t *shape_ptr, IndexType *output_ptr,
                              size_t input_size, size_t rank) {
  for (size_t tid = blockIdx.x * blockDim.x + threadIdx.x; tid < input_size; tid += blockDim.x * gridDim.x) {
    bool is_write = (tid != 0 && index_ptr[tid] != index_ptr[tid - 1]) || (tid == 0 && index_ptr[tid]);
    if (is_write) {
      size_t fill_index = index_ptr[tid] * rank - 1;
      size_t fill_value = tid;
      for (size_t i = 0; i < rank; i++) {
        size_t base = shape_ptr[rank - 1 - i];
        output_ptr[fill_index] = fill_value % base;
        fill_index--;
        fill_value /= base;
      }
    }
  }
}

template <typename DataType, typename IndexType>
CUDA_LIB_EXPORT void NonZero(const DataType *input_ptr, size_t *index_ptr, size_t *shape_ptr, IndexType *output_ptr,
                             size_t input_size, size_t rank, cudaStream_t cuda_stream) {
  cub::TransformInputIterator<size_t, IsZero<DataType>, const DataType *> iter(input_ptr, IsZero<DataType>());
  void *d_temp_storage = NULL;
  size_t temp_storage_bytes = 0;
  (void)cub::DeviceScan::InclusiveSum(nullptr, temp_storage_bytes, iter, index_ptr, input_size, cuda_stream);
  (void)cudaMalloc(&d_temp_storage, temp_storage_bytes);
  (void)cub::DeviceScan::InclusiveSum(d_temp_storage, temp_storage_bytes, iter, index_ptr, input_size, cuda_stream);

  // Extract the first index to appear and transform into output index,
  // e.g., [0, 0, 1, 2, 2, 2] -> [(1, 2), (2, 3)] -> [(0, 0, 2), (0, 1, 0)] when shape is (2, 1, 3)
  NonZeroKernel<<<GET_BLOCKS(input_size), GET_THREADS, 0, cuda_stream>>>(index_ptr, shape_ptr, output_ptr, input_size,
                                                                         rank);
  // Since cudaGetLastError can return the last error from a runtime call,
  // we catch the error in Launch function.
  (void)cudaFree(d_temp_storage);
}

template CUDA_LIB_EXPORT void NonZero<bool, int64_t>(const bool *input_ptr, size_t *index_ptr, size_t *shape_ptr,
                                                     int64_t *output_ptr, size_t input_size, size_t rank,
                                                     cudaStream_t cuda_stream);
template CUDA_LIB_EXPORT void NonZero<uint8_t, int64_t>(const uint8_t *input_ptr, size_t *index_ptr, size_t *shape_ptr,
                                                        int64_t *output_ptr, size_t input_size, size_t rank,
                                                        cudaStream_t cuda_stream);
template CUDA_LIB_EXPORT void NonZero<uint16_t, int64_t>(const uint16_t *input_ptr, size_t *index_ptr,
                                                         size_t *shape_ptr, int64_t *output_ptr, size_t input_size,
                                                         size_t rank, cudaStream_t cuda_stream);
template CUDA_LIB_EXPORT void NonZero<uint32_t, int64_t>(const uint32_t *input_ptr, size_t *index_ptr,
                                                         size_t *shape_ptr, int64_t *output_ptr, size_t input_size,
                                                         size_t rank, cudaStream_t cuda_stream);
template CUDA_LIB_EXPORT void NonZero<uint64_t, int64_t>(const uint64_t *input_ptr, size_t *index_ptr,
                                                         size_t *shape_ptr, int64_t *output_ptr, size_t input_size,
                                                         size_t rank, cudaStream_t cuda_stream);
template CUDA_LIB_EXPORT void NonZero<int8_t, int64_t>(const int8_t *input_ptr, size_t *index_ptr, size_t *shape_ptr,
                                                       int64_t *output_ptr, size_t input_size, size_t rank,
                                                       cudaStream_t cuda_stream);
template CUDA_LIB_EXPORT void NonZero<int16_t, int64_t>(const int16_t *input_ptr, size_t *index_ptr, size_t *shape_ptr,
                                                        int64_t *output_ptr, size_t input_size, size_t rank,
                                                        cudaStream_t cuda_stream);
template CUDA_LIB_EXPORT void NonZero<int32_t, int64_t>(const int32_t *input_ptr, size_t *index_ptr, size_t *shape_ptr,
                                                        int64_t *output_ptr, size_t input_size, size_t rank,
                                                        cudaStream_t cuda_stream);
template CUDA_LIB_EXPORT void NonZero<int64_t, int64_t>(const int64_t *input_ptr, size_t *index_ptr, size_t *shape_ptr,
                                                        int64_t *output_ptr, size_t input_size, size_t rank,
                                                        cudaStream_t cuda_stream);
template CUDA_LIB_EXPORT void NonZero<half, int64_t>(const half *input_ptr, size_t *index_ptr, size_t *shape_ptr,
                                                     int64_t *output_ptr, size_t input_size, size_t rank,
                                                     cudaStream_t cuda_stream);
template CUDA_LIB_EXPORT void NonZero<float, int64_t>(const float *input_ptr, size_t *index_ptr, size_t *shape_ptr,
                                                      int64_t *output_ptr, size_t input_size, size_t rank,
                                                      cudaStream_t cuda_stream);
template CUDA_LIB_EXPORT void NonZero<double, int64_t>(const double *input_ptr, size_t *index_ptr, size_t *shape_ptr,
                                                       int64_t *output_ptr, size_t input_size, size_t rank,
                                                       cudaStream_t cuda_stream);
