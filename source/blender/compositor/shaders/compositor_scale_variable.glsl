/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_bicubic_sampler_lib.glsl"
#include "gpu_shader_compositor_texture_utilities.glsl"

void main()
{
  ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
  ivec2 input_size = texture_size(input_tx);

  vec2 coordinates = (vec2(texel) + vec2(0.5f)) / vec2(input_size);
  vec2 center = vec2(0.5f);

  vec2 scale = vec2(texture_load(x_scale_tx, texel).x, texture_load(y_scale_tx, texel).x);
  vec2 scaled_coordinates = center + (coordinates - center) / max(scale, 0.0001f);

  imageStore(output_img, texel, SAMPLER_FUNCTION(input_tx, scaled_coordinates));
}
