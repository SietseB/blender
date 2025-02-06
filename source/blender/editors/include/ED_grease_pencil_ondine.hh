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

class OndinePrepareRender {
 public:
  /* Members. */
  blender::float4x4 camera_perspective_matrix;

  /* Data for easy access. */
  Depsgraph *depsgraph;
  Scene *scene;

  float render_width;
  float render_height;
  float2 render_size;
  float3 camera_z_axis;
  float3 camera_location;
  float3 camera_normal_vec;
  float camera_rot_sin;
  float camera_rot_cos;

  /* Methods. */
  OndinePrepareRender();

  void init(bContext *C);
  bool prepare_camera_params();
  void set_unique_stroke_seeds(bContext *C, const bool current_frame_only);
  float2 get_point_in_2d(const float3 &pos) const;
  float get_stroke_point_radius(const float3 &point, const float4x4 &transform_matrix) const;
  void get_vertex_color(const MaterialGPencilStyle *mat_style,
                        const ColorGeometry4f &vertex_color,
                        const bool use_texture,
                        float *r_color);
  void set_fill_color(const ColorGeometry4f &fill_color,
                      const MaterialGPencilStyle *mat_style,
                      const bke::greasepencil::Layer &layer,
                      OndineRenderStroke &r_render_stroke);
  void set_zdepth(Object *object, const float4x4 &object_instance_transform) const;
  void set_render_data(Object *object, const float4x4 &object_instance_transform);
};

void ondine_set_unique_stroke_seeds(bContext *C, const bool current_frame_only);
void ondine_set_render_data(Object *ob, const float4x4 &object_instance_transform);
void ondine_set_zdepth(Object *ob, const float4x4 &object_instance_transform);
bool ondine_render_init(bContext *C);

}  // namespace blender::ondine
