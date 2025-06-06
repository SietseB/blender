/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_sculpt_curves_info.hh"

VERTEX_SHADER_CREATE_INFO(overlay_sculpt_curves_selection)

#include "draw_curves_lib.glsl"
#include "draw_model_lib.glsl"
#include "draw_view_clipping_lib.glsl"
#include "draw_view_lib.glsl"

float retrieve_selection()
{
  if (is_point_domain) {
    return texelFetch(selection_tx, hair_get_base_id()).r;
  }
  return texelFetch(selection_tx, hair_get_strand_id()).r;
}

void main()
{
  bool is_persp = (drw_view().winmat[3][3] == 0.0f);
  float time, thick_time, thickness;
  float3 world_pos, tangent, binor;
  hair_get_pos_tan_binor_time(is_persp,
                              drw_modelinv(),
                              drw_view().viewinv[3].xyz,
                              drw_view().viewinv[2].xyz,
                              world_pos,
                              tangent,
                              binor,
                              time,
                              thickness,
                              thick_time);

  gl_Position = drw_point_world_to_homogenous(world_pos);

  mask_weight = 1.0f - (selection_opacity - retrieve_selection() * selection_opacity);

  view_clipping_distances(world_pos);
}
