#pragma OPENCL EXTENSION cl_khr_fp16 : enable
__constant sampler_t smp_zero = CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP | CLK_FILTER_NEAREST;
__kernel void DepthwiseConv2d_IMG_NC4HW4(__read_only image2d_t src_data, __global FLT4 *filter, __global FLT4 *bias,
                                         __write_only image2d_t dst_data, int2 kernel_size, int2 stride, int2 padding,
                                         int2 dilation, int4 src_size, int4 dst_size, float relu_clip_min,
                                         float relu_clip_max) {
  int X = get_global_id(0);
  int Y = get_global_id(1);
  int Z = get_global_id(2);
  if (X >= dst_size.x || Y >= dst_size.y || Z >= dst_size.z) return;
  FLT4 r = (FLT4)(0.0f, 0.0f, 0.0f, 0.0f);
  int x_offset = X * stride.x + padding.x;
  int y_offset = Y * stride.y + padding.y;
  int fx_c = Z * kernel_size.x * kernel_size.y;
  for (int ky = 0; ky < kernel_size.y; ++ky) {
    int y_c = y_offset + ky * dilation.y;
    bool outside_y = y_c < 0 || y_c >= src_size.y;
    for (int kx = 0; kx < kernel_size.x; ++kx) {
      int x_c = x_offset + kx * dilation.x;
      bool outside_x = x_c < 0 || x_c >= src_size.x;
      if (!outside_x && !outside_y) {
        FLT4 flt_p = filter[fx_c];
        FLT4 src_p = READ_IMAGE(src_data, smp_zero, (int2)(x_c, (Z * src_size.y + y_c)));
        r += TO_FLT4(src_p * flt_p);
      }
      fx_c++;
    }
  }
  FLT4 bias_p = bias[Z];
  FLT4 res = TO_FLT4(r) + bias_p;
  res = clamp(res, (FLT)(relu_clip_min), (FLT)(relu_clip_max));
  WRITE_IMAGE(dst_data, (int2)(X, (Z * dst_size.y + Y)), res);
}

__kernel void DepthwiseConv2d_IMG_NHWC4(__read_only image2d_t src_data, __global FLT4 *filter, __global FLT4 *bias,
                                        __write_only image2d_t dst_data, int2 kernel_size, int2 stride, int2 padding,
                                        int2 dilation, int4 src_size, int4 dst_size, float relu_clip_min,
                                        float relu_clip_max) {
  int X = get_global_id(0);
  int Y = get_global_id(1);
  int Z = get_global_id(2) * 2;
  if (X >= dst_size.x || Y >= dst_size.y || Z >= dst_size.z) return;
  FLT4 r[2] = {(FLT4)(0.0f, 0.0f, 0.0f, 0.0f), (FLT4)(0.0f, 0.0f, 0.0f, 0.0f)};
  int x_offset = X * stride.x + padding.x;
  int y_offset = Y * stride.y + padding.y;
  int f_len = kernel_size.x * kernel_size.y;
  int fx_c = Z * f_len;
  for (int ky = 0; ky < kernel_size.y; ++ky) {
    int y_c = y_offset + ky * dilation.y;
    bool outside_y = y_c < 0 || y_c >= src_size.y;
    for (int kx = 0; kx < kernel_size.x; ++kx) {
      int x_c = x_offset + kx * dilation.x;
      bool outside_x = x_c < 0 || x_c >= src_size.x;
      if (!outside_x && !outside_y) {
        FLT4 flt_p0 = filter[fx_c];
        FLT4 flt_p1 = filter[fx_c + f_len];
        FLT4 src_p0 = READ_IMAGE(src_data, smp_zero, (int2)(Z + x_c * src_size.z, y_c));
        FLT4 src_p1 = READ_IMAGE(src_data, smp_zero, (int2)(Z + 1 + x_c * src_size.z, y_c));
        r[0] += TO_FLT4(src_p0 * flt_p0);
        r[1] += TO_FLT4(src_p1 * flt_p1);
      }
      fx_c++;
    }
  }
  r[0] += bias[Z];
  r[0] = clamp(r[0], (FLT)(relu_clip_min), (FLT)(relu_clip_max));
  r[1] += bias[Z + 1];
  r[1] = clamp(r[1], (FLT)(relu_clip_min), (FLT)(relu_clip_max));
  WRITE_IMAGE(dst_data, (int2)(X * dst_size.z + Z, Y), r[0]);
  if ((dst_size.z & 0x1) == 0) {
    WRITE_IMAGE(dst_data, (int2)(X * dst_size.z + Z + 1, Y), r[1]);
  }
}
__kernel void DepthwiseConv2d_IMG_NHWC4_1x1(__read_only image2d_t src_data, __global FLT4 *filter, __global FLT4 *bias,
                                            __write_only image2d_t dst_data, int2 kernel_size, int2 stride,
                                            int2 padding, int2 dilation, int4 src_size, int4 dst_size,
                                            float relu_clip_min, float relu_clip_max) {
  int X = get_global_id(0);
  int Y = get_global_id(1);
  int Z = get_global_id(2) * 2;
  if (X >= dst_size.x || Y >= dst_size.y || Z >= dst_size.z) return;
  FLT4 r[2] = {(FLT4)(0.0f, 0.0f, 0.0f, 0.0f), (FLT4)(0.0f, 0.0f, 0.0f, 0.0f)};
  int x_offset = X * stride.x + padding.x;
  int y_offset = Y * stride.y + padding.y;
  int fx_c = Z;
  int y_c = y_offset;
  bool outside_y = y_c < 0 || y_c >= src_size.y;
  int x_c = x_offset;
  bool outside_x = x_c < 0 || x_c >= src_size.x;
  if (!outside_x && !outside_y) {
    FLT4 flt_p0 = filter[fx_c];
    FLT4 flt_p1 = filter[fx_c + 1];
    FLT4 src_p0 = READ_IMAGE(src_data, smp_zero, (int2)(Z + x_c * src_size.z, y_c));
    FLT4 src_p1 = READ_IMAGE(src_data, smp_zero, (int2)(Z + 1 + x_c * src_size.z, y_c));
    r[0] += TO_FLT4(src_p0 * flt_p0);
    r[1] += TO_FLT4(src_p1 * flt_p1);
  }
  r[0] += bias[Z];
  r[0] = clamp(r[0], (FLT)(relu_clip_min), (FLT)(relu_clip_max));
  r[1] += bias[Z + 1];
  r[1] = clamp(r[1], (FLT)(relu_clip_min), (FLT)(relu_clip_max));
  WRITE_IMAGE(dst_data, (int2)(X * dst_size.z + Z, Y), r[0]);
  if ((dst_size.z & 0x1) == 0) {
    WRITE_IMAGE(dst_data, (int2)(X * dst_size.z + Z + 1, Y), r[1]);
  }
}
__kernel void DepthwiseConv2d_BUF_NC4HW4(__global FLT4 *src_data, __global FLT4 *filter, __global FLT4 *bias,
                                         __global FLT4 *dst_data, int2 kernel_size, int2 stride, int2 padding,
                                         int2 dilation, int4 src_size, int4 dst_size, float relu_clip_min,
                                         float relu_clip_max) {
  int X = get_global_id(0);
  int Y = get_global_id(1);
  int Z = get_global_id(2);
  if (X >= dst_size.x || Y >= dst_size.y || Z >= dst_size.z) return;
  FLT4 r = (FLT4)(0.0f, 0.0f, 0.0f, 0.0f);
  int x_offset = X * stride.x + padding.x;
  int y_offset = Y * stride.y + padding.y;
  int fx_c = Z * kernel_size.x * kernel_size.y;
  for (int ky = 0; ky < kernel_size.y; ++ky) {
    int y_c = y_offset + ky * dilation.y;
    bool outside_y = y_c < 0 || y_c >= src_size.y;
    for (int kx = 0; kx < kernel_size.x; ++kx) {
      int x_c = x_offset + kx * dilation.x;
      bool outside_x = x_c < 0 || x_c >= src_size.x;
      if (!outside_x && !outside_y) {
        FLT4 flt_p = filter[fx_c];
        FLT4 src_p = src_data[(((Z)*src_size.y + (y_c)) * src_size.x + (x_c))];
        r += TO_FLT4(src_p * flt_p);
      }
      fx_c++;
    }
  }
  FLT4 bias_p = bias[Z];
  FLT4 res = TO_FLT4(r) + bias_p;
  res = clamp(res, (FLT)(relu_clip_min), (FLT)(relu_clip_max));
  dst_data[(((Z)*dst_size.y + (Y)) * dst_size.x + (X))] = res;
}

__kernel void DepthwiseConv2d_BUF_NHWC4(__global FLT4 *src_data, __global FLT4 *filter, __global FLT4 *bias,
                                        __global FLT4 *dst_data, int2 kernel_size, int2 stride, int2 padding,
                                        int2 dilation, int4 src_size, int4 dst_size, float relu_clip_min,
                                        float relu_clip_max) {
  int X = get_global_id(0);
  int Y = get_global_id(1);
  int Z = get_global_id(2);
  if (X >= dst_size.x || Y >= dst_size.y || Z >= dst_size.z) return;
  FLT4 r = (FLT4)(0.0f, 0.0f, 0.0f, 0.0f);
  int x_offset = X * stride.x + padding.x;
  int y_offset = Y * stride.y + padding.y;
  int fx_c = Z * kernel_size.x * kernel_size.y;
  for (int ky = 0; ky < kernel_size.y; ++ky) {
    int y_c = y_offset + ky * dilation.y;
    bool outside_y = y_c < 0 || y_c >= src_size.y;
    for (int kx = 0; kx < kernel_size.x; ++kx) {
      int x_c = x_offset + kx * dilation.x;
      bool outside_x = x_c < 0 || x_c >= src_size.x;
      if (!outside_x && !outside_y) {
        FLT4 flt_p = filter[fx_c];
        FLT4 src_p = src_data[((y_c * src_size.x + x_c) * src_size.z + Z)];
        r += TO_FLT4(src_p * flt_p);
      }
      fx_c++;
    }
  }
  FLT4 bias_p = bias[Z];
  FLT4 res = TO_FLT4(r) + bias_p;
  res = clamp(res, (FLT)(relu_clip_min), (FLT)(relu_clip_max));
  dst_data[((Y * dst_size.x + X) * dst_size.z + Z)] = res;
}

__kernel void DepthwiseConv2d_BUF_NHWC4_1x1(__global FLT4 *src_data, __global FLT4 *filter, __global FLT4 *bias,
                                            __global FLT4 *dst_data, int2 kernel_size, int2 stride, int2 padding,
                                            int2 dilation, int4 src_size, int4 dst_size, float relu_clip_min,
                                            float relu_clip_max) {
  int X = get_global_id(0);
  int Y = get_global_id(1);
  int Z = get_global_id(2);
  if (X >= dst_size.x || Y >= dst_size.y || Z >= dst_size.z) return;
  FLT4 r = (FLT4)(0.0f, 0.0f, 0.0f, 0.0f);
  int x_offset = X * stride.x + padding.x;
  int y_offset = Y * stride.y + padding.y;
  int fx_c = Z;
  {
    int y_c = y_offset;
    bool outside_y = y_c < 0 || y_c >= src_size.y;
    {
      int x_c = x_offset;
      bool outside_x = x_c < 0 || x_c >= src_size.x;
      if (!outside_x && !outside_y) {
        FLT4 flt_p = filter[fx_c];
        FLT4 src_p = src_data[((y_c * src_size.x + x_c) * src_size.z + Z)];
        r += TO_FLT4(src_p * flt_p);
      }
    }
  }
  FLT4 bias_p = bias[Z];
  FLT4 res = TO_FLT4(r) + bias_p;
  res = clamp(res, (FLT)(relu_clip_min), (FLT)(relu_clip_max));
  dst_data[((Y * dst_size.x + X) * dst_size.z + Z)] = res;
}
