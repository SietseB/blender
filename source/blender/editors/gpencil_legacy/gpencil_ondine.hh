/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2022 Blender Foundation. All rights reserved. */
#pragma once

#include "DNA_gpencil_legacy_types.h"
#include "DNA_material_types.h"

#include "BLI_math_matrix_types.hh"

namespace blender {

constexpr int IMAGE_PADDING = 8;

class GpencilOndine {
 public:
  /* Methods */
  GpencilOndine();

  void init(bContext *C);
  bool prepare_camera_params(bContext *C);
  void set_zdepth(Object *object);
  void set_render_data(Object *object, const blender::float4x4 obmat);
  void set_stroke_color(const bGPDlayer *gpl,
                        bGPDstroke *gps,
                        const MaterialGPencilStyle *gp_style);
  void get_vertex_color(const bGPDlayer *gpl,
                        const MaterialGPencilStyle *gp_style,
                        const bGPDspoint &point,
                        const bool use_texture,
                        float *r_color);
  float stroke_point_radius_get(bGPDstroke *gps, const int p_index, const float thickness);
  float2 gpencil_3D_point_to_2D(const float3 co);
  void set_unique_stroke_seeds(bContext *C, const bool current_frame_only);

 protected:
  bool invert_axis_[2];
  blender::float4x4 diff_mat_;

  /* Data for easy access. */
  struct Main *bmain_;
  struct Depsgraph *depsgraph_;
  struct Scene *scene_;
  struct RegionView3D *rv3d_;
  struct View3D *v3d_;
  struct ARegion *region_;

  int16_t winx_, winy_;
  int16_t render_x_, render_y_;
  float camera_ratio_;
  rctf camera_rect_;
  blender::float3 camera_z_axis_;
  blender::float3 camera_loc_;
  blender::float3 camera_normal_vec_;
  float camera_rot_sin_;
  float camera_rot_cos_;
  float defaultpixsize_;

  blender::float2 offset_;

  int cfra_;

  float stroke_color_[4], fill_color_[4];

 private:
  float avg_opacity_;
  bool is_camera_;
  float persmat_[4][4];
};

}  // namespace blender

void gpencil_ondine_set_render_data(Object *ob, const float mat[4][4]);
void gpencil_ondine_set_zdepth(Object *ob);
bool gpencil_ondine_render_init(bContext *C);
