/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_lightprobe_volume_info.hh"

VERTEX_SHADER_CREATE_INFO(eevee_display_lightprobe_volume)

#include "draw_view_lib.glsl"
#include "eevee_lightprobe_lib.glsl"

void main()
{
  /* Constant array moved inside function scope.
   * Minimizes local register allocation in MSL. */
  const vec2 pos[6] = float2_array(vec2(-1.0f, -1.0f),
                                   vec2(1.0f, -1.0f),
                                   vec2(-1.0f, 1.0f),

                                   vec2(1.0f, -1.0f),
                                   vec2(1.0f, 1.0f),
                                   vec2(-1.0f, 1.0f));

  lP = pos[gl_VertexID % 6];
  int cell_index = gl_VertexID / 6;

  ivec3 grid_res = grid_resolution;

  cell = ivec3(cell_index / (grid_res.z * grid_res.y),
               (cell_index / grid_res.z) % grid_res.y,
               cell_index % grid_res.z);

  vec3 ws_cell_pos = lightprobe_volume_grid_sample_position(grid_to_world, grid_res, cell);

  float sphere_radius_final = sphere_radius;
  if (display_validity) {
    float validity = texelFetch(validity_tx, cell, 0).r;
    sphere_radius_final *= mix(1.0f, 0.1f, validity);
  }

  vec3 vs_offset = vec3(lP, 0.0f) * sphere_radius_final;
  vec3 vP = drw_point_world_to_view(ws_cell_pos) + vs_offset;

  gl_Position = drw_point_view_to_homogenous(vP);
  /* Small bias to let the icon draw without Z-fighting. */
  gl_Position.z += 0.0001f;
}
