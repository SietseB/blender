/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2022 Blender Foundation. All rights reserved. */
#pragma once

#include "BLI_float4x4.hh"
#include "BLI_math_vec_types.hh"
#include "BLI_vector.hh"

// using blender::Vector;

namespace blender::editor::gpencil {

class GpencilOndine
{
  public:
    /* Methods */
    GpencilOndine();
    void init(bContext *C);
    bool prepare_camera_params(bContext *C);
    void set_zdepths(bContext *C);

  protected:
    bool invert_axis_[2];
    float4x4 diff_mat_;

    /* Data for easy access. */
    struct Main *bmain_;
    struct Depsgraph *depsgraph_;
    struct Scene *scene_;
    struct bGPdata *gpd_;
    struct RegionView3D *rv3d_;
    struct View3D *v3d_;
    struct ARegion *region_;

    int16_t winx_, winy_;
    int16_t render_x_, render_y_;
    float camera_ratio_;
    rctf camera_rect_;

    float2 offset_;

    int cfra_;

    float stroke_color_[4], fill_color_[4];

   private:
    float avg_opacity_;
    bool is_camera_;
    float persmat_[4][4];
};

}  // namespace blender::editor::gpencil
