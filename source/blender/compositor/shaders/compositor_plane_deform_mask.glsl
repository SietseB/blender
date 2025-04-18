/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

void main()
{
  ivec2 texel = ivec2(gl_GlobalInvocationID.xy);

  vec2 coordinates = (vec2(texel) + vec2(0.5f)) / vec2(imageSize(mask_img));

  vec3 transformed_coordinates = to_float3x3(homography_matrix) * vec3(coordinates, 1.0f);
  /* Point is at infinity and will be zero when sampled, so early exit. */
  if (transformed_coordinates.z == 0.0f) {
    imageStore(mask_img, texel, vec4(0.0f));
    return;
  }
  vec2 projected_coordinates = transformed_coordinates.xy / transformed_coordinates.z;

  bool is_inside_plane = all(greaterThanEqual(projected_coordinates, vec2(0.0f))) &&
                         all(lessThanEqual(projected_coordinates, vec2(1.0f)));
  float mask_value = is_inside_plane ? 1.0f : 0.0f;

  imageStore(mask_img, texel, vec4(mask_value));
}
