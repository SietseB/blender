/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_compositor_texture_utilities.glsl"

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);

  float4 color = texture_load(input_tx, texel);

  /* An opaque pixel, not part of the inpainting region, write the original color. */
  if (color.a == 1.0f) {
    imageStore(output_img, texel, color);
    return;
  }

  float distance_to_boundary = texture_load(distance_to_boundary_tx, texel).x;

  /* Further than the inpainting distance, not part of the inpainting region, write the original
   * color. */
  if (distance_to_boundary > max_distance) {
    imageStore(output_img, texel, color);
    return;
  }

  /* Mix the inpainted color with the original color using its alpha because semi-transparent areas
   * are considered to be partially inpainted. */
  float4 inpainted_color = texture_load(inpainted_region_tx, texel);
  imageStore(output_img, texel, float4(mix(inpainted_color.rgb, color.rgb, color.a), 1.0f));
}
