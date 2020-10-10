#ifdef cl_khr_fp16
#pragma OPENCL EXTENSION cl_khr_fp16 : enable
#endif
__kernel void MaxPooling2d_BUF(__global FLT4 *input, __global FLT4 *output, const int4 input_shape,
                               const int4 output_shape, const int2 stride, const int2 kernel_size, const int2 padding) {
  // axis to dst tensor coordinate
  int X = get_global_id(0);
  int Y = get_global_id(1);
  int Z = get_global_id(2);

  // boundary check
  if (X >= output_shape.x || Y >= output_shape.y || Z >= output_shape.w) {
    return;
  }

  FLT4 maximum = (FLT4)(-10000.0f);
  int xs = X * stride.x - padding.x;
  int ys = Y * stride.y - padding.y;

  for (int kx = 0; kx < kernel_size.x; ++kx) {
    int x_c = xs + kx;
    if (x_c < 0 || x_c >= input_shape.x) {
      continue;
    }
    for (int ky = 0; ky < kernel_size.y; ++ky) {
      int y_c = ys + ky;
      if (y_c < 0 || y_c >= input_shape.y) {
        continue;
      }
      FLT4 src = input[(input_shape.y * x_c + y_c) * input_shape.w + Z];
      maximum = max(src, maximum);
    }
  }
  output[(output_shape.y * X + Y) * output_shape.w + Z] = maximum;
}

__constant sampler_t smp_none = CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_NONE | CLK_FILTER_NEAREST;

__kernel void MaxPooling2d_NHWC4_IMG(__read_only image2d_t input, __write_only image2d_t output, const int4 input_shape,
                                     const int4 output_shape, const int2 stride, const int2 kernel_size,
                                     const int2 padding) {
  // axis to dst tensor coordinate
  int X = get_global_id(0);
  int Y = get_global_id(1);
  int Z = get_global_id(2);

  // boundary check
  if (X >= output_shape.x || Y >= output_shape.y || Z >= output_shape.w) {
    return;
  }

  FLT4 maximum = (FLT4)(-10000.0f);
  int xs = X * stride.x - padding.x;
  int ys = Y * stride.y - padding.y;
  for (int ky = 0; ky < kernel_size.y; ++ky) {
    int y_c = ys + ky;
    if (y_c < 0 || y_c >= input_shape.y) continue;
    for (int kx = 0; kx < kernel_size.x; ++kx) {
      int x_c = xs + kx;
      if (x_c < 0 || x_c >= input_shape.x) continue;
      FLT4 src = READ_IMAGE(input, smp_none, (int2)(y_c * input_shape.w + Z, x_c));
      maximum = max(src, maximum);
    }
  }
  WRITE_IMAGE(output, (int2)(Y * output_shape.w + Z, X), maximum);
}

__kernel void MaxPooling2d_ReLU_NHWC4_IMG(__read_only image2d_t input, __write_only image2d_t output,
                                          const int4 input_shape, const int4 output_shape, const int2 stride,
                                          const int2 kernel_size, const int2 padding) {
  // axis to dst tensor coordinate
  int X = get_global_id(0);
  int Y = get_global_id(1);
  int Z = get_global_id(2);

  // boundary check
  if (X >= output_shape.x || Y >= output_shape.y || Z >= output_shape.w) {
    return;
  }

  FLT4 maximum = (FLT4)(-10000.0f);
  int xs = X * stride.x - padding.x;
  int ys = Y * stride.y - padding.y;
  for (int ky = 0; ky < kernel_size.y; ++ky) {
    int y_c = ys + ky;
    if (y_c < 0 || y_c >= input_shape.y) continue;
    for (int kx = 0; kx < kernel_size.x; ++kx) {
      int x_c = xs + kx;
      if (x_c < 0 || x_c >= input_shape.x) continue;
      FLT4 src = READ_IMAGE(input, smp_none, (int2)(y_c * input_shape.w + Z, x_c));
      maximum = max(src, maximum);
    }
  }
  WRITE_IMAGE(output, (int2)(Y * output_shape.w + Z, X), max(maximum, (FLT4)(0.f)));
}

__kernel void MaxPooling2d_NC4HW4_IMG(__read_only image2d_t input, __write_only image2d_t output,
                                      const int4 input_shape, const int4 output_shape, const int2 stride,
                                      const int2 kernel_size, const int2 padding) {
  // axis to dst tensor coordinate
  int X = get_global_id(0);
  int Y = get_global_id(1);
  int Z = get_global_id(2);

  // boundary check
  if (X >= output_shape.x || Y >= output_shape.y || Z >= output_shape.w) {
    return;
  }

  FLT4 maximum = (FLT4)(-10000.0f);
  int xs = X * stride.x - padding.x;
  int ys = Y * stride.y - padding.y;
  for (int ky = 0; ky < kernel_size.y; ++ky) {
    int y_c = ys + ky;
    if (y_c < 0 || y_c >= input_shape.y) continue;
    for (int kx = 0; kx < kernel_size.x; ++kx) {
      int x_c = xs + kx;
      if (x_c < 0 || x_c >= input_shape.x) continue;
      FLT4 src = READ_IMAGE(input, smp_none, (int2)(y_c, Z * input_shape.x + x_c));
      maximum = max(src, maximum);
    }
  }
  WRITE_IMAGE(output, (int2)(Y, Z * output_shape.x + X), maximum);
}
