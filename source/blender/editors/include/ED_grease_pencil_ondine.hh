/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2022 Blender Foundation. All rights reserved. */
#pragma once

#include "DNA_material_types.h"

#include "BKE_grease_pencil.hh"

#include "BLI_math_matrix_types.hh"

namespace blender::ondine {

/* Image padding, enabling operations at the edges of the render image (like smoothing etc.). */
constexpr int IMAGE_PADDING = 8;
constexpr float GPENCIL_ALPHA_OPACITY_THRESHOLD = 0.001f;

class GpencilOndine {
 public:
  /* Methods */
  GpencilOndine();

  void init(bContext *C);
  bool prepare_camera_params();
  void set_unique_stroke_seeds(bContext *C, const bool current_frame_only);
  float2 gpencil_3d_point_to_2d(const float3 co);
  float stroke_point_radius_get(const float3 point, const float thickness);
  void get_vertex_color(const MaterialGPencilStyle *mat_style,
                        const ColorGeometry4f &vertex_color,
                        const bool use_texture,
                        float *r_color);
  void set_fill_color(const ColorGeometry4f &fill_color,
                      const MaterialGPencilStyle *mat_style,
                      const bke::greasepencil::Layer &layer,
                      OndineRenderStroke &r_render_stroke);
  void set_zdepth(Object *object);
  void set_render_data(Object *object, const blender::float4x4 obmat);

 protected:
  blender::float4x4 diff_mat_;

  /* Data for easy access. */
  struct Main *bmain_;
  struct Depsgraph *depsgraph_;
  struct Scene *scene_;
  struct RegionView3D *rv3d_;
  struct View3D *v3d_;
  struct ARegion *region_;

  float render_x_, render_y_;
  blender::float3 camera_z_axis_;
  blender::float3 camera_loc_;
  blender::float3 camera_normal_vec_;
  float camera_rot_sin_;
  float camera_rot_cos_;
  float defaultpixsize_;

  blender::float2 offset_;

  int cfra_;

 private:
  float persmat_[4][4];
};

void gpencil_ondine_set_unique_stroke_seeds(bContext *C, const bool current_frame_only);
void gpencil_ondine_set_render_data(Object *ob, const float mat[4][4]);
void gpencil_ondine_set_zdepth(Object *ob);
bool gpencil_ondine_render_init(bContext *C);

}  // namespace blender::ondine
