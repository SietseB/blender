/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2008 Blender Foundation. */

/** \file
 * \ingroup edgpencil
 * Operators for Ondine watercolor Grease Pencil.
 */

#include "DNA_scene_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_material_types.h"
#include "DNA_space_types.h"

#include "BKE_context.h"
#include "BKE_camera.h"
#include "BKE_screen.h"
#include "BKE_gpencil.h"
#include "BKE_gpencil_geom.h"
#include "BKE_material.h"

#include "BLI_listbase.h"

#include "WM_api.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_prototypes.h"

#include "ED_view3d.h"
#include "ED_gpencil.h"
#include "gpencil_intern.h"

#include "gpencil_ondine.hh"
#include "gpencil_ondine_render.h"

namespace blender {

/* Object instance of Ondine runtime render data */
GpencilOndine *ondine_render = new GpencilOndine();

ARegion *get_invoke_region(bContext *C)
{
  bScreen *screen = CTX_wm_screen(C);
  if (screen == NULL) {
    return NULL;
  }
  ScrArea *area = BKE_screen_find_big_area(screen, SPACE_VIEW3D, 0);
  if (area == NULL) {
    return NULL;
  }

  ARegion *region = BKE_area_find_region_type(area, RGN_TYPE_WINDOW);

  return region;
}

View3D *get_invoke_view3d(bContext *C)
{
  bScreen *screen = CTX_wm_screen(C);
  if (screen == NULL) {
    return NULL;
  }
  ScrArea *area = BKE_screen_find_big_area(screen, SPACE_VIEW3D, 0);
  if (area == NULL) {
    return NULL;
  }
  if (area) {
    return (View3D *)area->spacedata.first;
  }

  return NULL;
}

/* Runtime render properties */
GpencilOndine::GpencilOndine()
{
}

void GpencilOndine::init(bContext *C)
{
  /* Easy access data. */
  bmain_ = CTX_data_main(C);
  depsgraph_ = CTX_data_depsgraph_pointer(C);
  scene_ = CTX_data_scene(C);
  region_ = get_invoke_region(C);
  v3d_ = get_invoke_view3d(C);
  rv3d_ = (RegionView3D *)region_->regiondata;
}

bool GpencilOndine::prepare_camera_params(bContext *C)
{
  Object *cam_ob = scene_->camera;
  float vec_z[3] = {0.0f, 0.0f, -1.0f};

  /* Calculate camera matrix. */
  if (cam_ob != nullptr) {
    /* Set up parameters. */
    CameraParams params;
    BKE_camera_params_init(&params);
    BKE_camera_params_from_object(&params, cam_ob);

    /* Compute matrix, view-plane, etc. */
    RenderData *rd = &scene_->r;
    BKE_camera_params_compute_viewplane(&params, rd->xsch, rd->ysch, rd->xasp, rd->yasp);
    BKE_camera_params_compute_matrix(&params);
    
    float viewmat[4][4];
    invert_m4_m4(viewmat, cam_ob->object_to_world);

    mul_m4_m4m4(persmat_, params.winmat, viewmat);

    /* Store camera position and normal vector */
    camera_loc_ = cam_ob->loc;
    mul_v3_m4v3(camera_normal_vec_, cam_ob->object_to_world, vec_z);
    normalize_v3(camera_normal_vec_);

    /* Store camera rotation */
    camera_rot_sin_ = (float)abs(sin(cam_ob->rot[0]));
    camera_rot_cos_ = (float)abs(cos(cam_ob->rot[0]));
  }
  else {
    unit_m4(persmat_);
    camera_rot_sin_ = 1.0f;
    camera_rot_cos_ = 0.0f;
  }
  
  winx_ = region_->winx;
  winy_ = region_->winy;

  /* Camera position. */
  copy_v3_v3(camera_z_axis_, rv3d_->viewinv[2]);

  /* Camera rectangle. */
  if (rv3d_->persp == RV3D_CAMOB) {
    render_x_ = (scene_->r.xsch * scene_->r.size) / 100;
    render_y_ = (scene_->r.ysch * scene_->r.size) / 100;

    ED_view3d_calc_camera_border(
        CTX_data_scene(C), depsgraph_, region_, v3d_, rv3d_, &camera_rect_, true);
    is_camera_ = true;
    camera_ratio_ = render_x_ / (camera_rect_.xmax - camera_rect_.xmin);
    offset_.x = camera_rect_.xmin;
    offset_.y = camera_rect_.ymin;
    return true;
  }
  else {
    return false;
  }
}

float2 GpencilOndine::gpencil_3D_point_to_2D(const float3 co)
{
  float3 parent_co = diff_mat_ * co;

  float2 r_co;
  mul_v2_project_m4_v3(&r_co.x, persmat_, &parent_co.x);
  r_co.x = (r_co.x + 1.0f) / 2.0f * (float)render_x_;
  r_co.y = (float)render_y_ - (r_co.y + 1.0f) / 2.0f * (float)render_y_;

  return r_co;
}

float GpencilOndine::stroke_point_radius_get(bGPdata *gpd, bGPDlayer *gpl, bGPDstroke *gps,
                                             const int p_index, const float thickness)
{
  float defaultpixsize = 1000.0f / gpd->pixfactor;
  float stroke_radius = (thickness / defaultpixsize) / 2.0f;

  bGPDspoint *pt1 = &gps->points[p_index];
  float3 p1, p2;
  p1.x = pt1->x;
  p1.y = pt1->y;
  p1.z = pt1->z;
  p2.x = pt1->x;
  p2.y = pt1->y + stroke_radius * camera_rot_cos_;
  p2.z = pt1->z + stroke_radius * camera_rot_sin_;
  
  const float2 screen_co1 = gpencil_3D_point_to_2D(p1);
  const float2 screen_co2 = gpencil_3D_point_to_2D(p2);
  const float2 v1 = screen_co1 - screen_co2;
  float radius = math::length(v1);
  
  return MAX2(radius, 1.0f);
}

void GpencilOndine::set_stroke_colors(Object *ob, bGPDlayer *gpl, bGPDstroke *gps, MaterialGPencilStyle *gp_style)
{
  float col[3];

  /* Get stroke color. */
  copy_v4_v4(stroke_color_, gp_style->stroke_rgba);
  interp_v3_v3v3(stroke_color_, stroke_color_, gps->points[0].vert_color, gps->points[0].vert_color[3]);
  interp_v3_v3v3(col, stroke_color_, gpl->tintcolor, gpl->tintcolor[3]);
  copy_v3_v3(gps->render_stroke_color, col);
  
  /* Get fill color */
  copy_v4_v4(fill_color_, gp_style->fill_rgba);
  interp_v3_v3v3(fill_color_, fill_color_, gps->vert_color_fill, gps->vert_color_fill[3]);
  interp_v3_v3v3(col, fill_color_, gpl->tintcolor, gpl->tintcolor[3]);
  copy_v3_v3(gps->render_fill_color, col);
  gps->render_fill_opacity = fill_color_[3] * gpl->opacity;
}

void GpencilOndine::set_zdepth(Object *object)
{
  /* Grease pencil object? */
  if (object->type != OB_GPENCIL) {
    return;
  }

  /* Ondine watercolor object? */
  bGPdata *gpd = (bGPdata *)object->data;
  if ((gpd->ondine_flag & GP_ONDINE_WATERCOLOR) == 0) {
    return;
  }

  /* Save z-depth from view to sort from back to front. */
  gpd->render_zdepth = dot_v3v3(camera_z_axis_, object->object_to_world[3]);
}

void GpencilOndine::set_render_data(Object *object)
{
  float cam_plane[4];

  /* Grease pencil object? */
  if (object->type != OB_GPENCIL) {
    return;
  }

  /* Ondine watercolor object? */
  bGPdata *gpd = (bGPdata *)object->data;
  if ((gpd->ondine_flag & GP_ONDINE_WATERCOLOR) == 0) {
    return;
  }

  /* Calculate camera plane */
  plane_from_point_normal_v3(cam_plane, camera_loc_, camera_normal_vec_);

  /* Iterate all layers of GP watercolor object */
  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    /* Layer is hidden? */
    if (gpl->flag & GP_LAYER_HIDE) {
      continue;
    }

    /* Prepare layer matrix */
    BKE_gpencil_layer_transform_matrix_get(depsgraph_, object, gpl, diff_mat_.values);
    diff_mat_ = diff_mat_ * float4x4(gpl->layer_invmat);

    /* Active keyframe? */
    bGPDframe *gpf = gpl->actframe;
    if ((gpf == nullptr) || (gpf->strokes.first == nullptr)) {
      continue;
    }

    /* Iterate all strokes of layer */
    LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
      if (!ED_gpencil_stroke_material_visible(object, gps)) {
        continue;
      }

      /* Set fill and stroke flags */
      MaterialGPencilStyle *gp_style = BKE_gpencil_material_settings(object, gps->mat_nr + 1);

      const bool has_stroke = ((gp_style->flag & GP_MATERIAL_STROKE_SHOW) &&
                              (gp_style->stroke_rgba[3] > GPENCIL_ALPHA_OPACITY_THRESH));
      const bool has_fill = ((gp_style->flag & GP_MATERIAL_FILL_SHOW) &&
                            (gp_style->fill_rgba[3] > GPENCIL_ALPHA_OPACITY_THRESH));

      gps->render_flag = 0;
      if (has_stroke) {
        gps->render_flag |= GP_ONDINE_STROKE_HAS_STROKE;
      }
      if (has_fill) {
        gps->render_flag |= GP_ONDINE_STROKE_HAS_FILL;
      }

      /* Set stroke and fill color, in linear sRGB */
      this->set_stroke_colors(object, gpl, gps, gp_style);

      /* Calculate distance to camera */
      gps->render_dist_to_camera = dist_signed_to_plane_v3(gps->boundbox_min, cam_plane);

      /* Init min/max calculations */
      float strength = (int)(gps->points[0].strength * 1000 + 0.5);
      strength = (float)strength / 1000;
      bool strength_is_constant = true;
      float min_y = FLT_MAX;
      float max_x = -FLT_MAX;
      int min_i1 = 0;
      float bbox_minx = FLT_MAX, bbox_miny = FLT_MAX;
      float bbox_maxx = -FLT_MAX, bbox_maxy = -FLT_MAX;
      float dist_to_cam = 0.0f;
      float min_dist_to_cam = -FLT_MAX, max_dist_to_cam = FLT_MAX;
      int min_dist_point_index = 0, max_dist_point_index = 0;

      /* Convert 3d stroke points to 2d */
      for (const int i : IndexRange(gps->totpoints)) {
        bGPDspoint &pt = gps->points[i];
        const float2 screen_co = this->gpencil_3D_point_to_2D(&pt.x);
        pt.flat_x = screen_co.x;
        pt.flat_y = screen_co.y;
        dist_to_cam = dist_signed_squared_to_plane_v3(&pt.x, cam_plane);
        pt.dist_to_cam = dist_to_cam;

        /* Keep track of closest/furthest point to camera */
        if (dist_to_cam < max_dist_to_cam) {
          max_dist_to_cam = dist_to_cam;
          max_dist_point_index = i;
        }
        if ((dist_to_cam > min_dist_to_cam) && (dist_to_cam <= 0)) {
          min_dist_to_cam = dist_to_cam;
          min_dist_point_index = i;
        }

        /* Constant alpha strength? */
        if (strength_is_constant) {
          float p_strength = (int)(pt.strength * 1000 + 0.5);
          p_strength = (float)p_strength / 1000;
          if ((strength_is_constant) && (p_strength != strength)) {
            strength_is_constant = false;
          }
        }

        /* Keep track of minimum y point */
        if (pt.flat_y <= min_y) {
          if ((pt.flat_y < min_y) || (pt.flat_x > max_x)) {
            min_i1 = i;
            min_y = pt.flat_y;
            max_x = pt.flat_x;
          }
        }

        /* Get bounding box */
        if (bbox_minx > pt.flat_x) {
          bbox_minx = pt.flat_x;
        }
        if (bbox_miny > pt.flat_y) {
          bbox_miny = pt.flat_y;
        }
        if (bbox_maxx < pt.flat_x) {
          bbox_maxx = pt.flat_x;
        }
        if (bbox_maxy < pt.flat_y) {
          bbox_maxy = pt.flat_y;
        }
      }

      /* Calculate stroke width */
      bool pressure_is_set = false;
      gps->render_stroke_width = 0.0f;
      if (has_stroke) {
        /* Get stroke thickness, taking object scale and layer line change into account */
        float thickness = gps->thickness + gpl->line_change;
        thickness *= mat4_to_scale(object->object_to_world);
        CLAMP_MIN(thickness, 1.0f);
        float max_stroke_width = this->stroke_point_radius_get(gpd, gpl, gps, min_dist_point_index, thickness) * 2.0f;
        float min_stroke_width = this->stroke_point_radius_get(gpd, gpl, gps, max_dist_point_index, thickness) * 2.0f;
        gps->render_stroke_width = max_stroke_width;

        /* Adjust point pressure based on distance to camera.
         * That way a stroke will get thinner when it is further away from the camera. */
        float stroke_width_factor = (max_stroke_width - min_stroke_width) / max_stroke_width;
        float delta_dist = min_dist_to_cam - max_dist_to_cam;
        if (delta_dist != 0.0f) {
          pressure_is_set = true;
          for (const int i : IndexRange(gps->totpoints)) {
            bGPDspoint &pt = gps->points[i];
            /* Adjust pressure based on camera distance */
            pt.pressure_3d = pt.pressure *
              (1.0f - ((min_dist_to_cam - pt.dist_to_cam) / delta_dist) * stroke_width_factor);
          }
        }
      }
      if (!pressure_is_set) {
        for (const int i : IndexRange(gps->totpoints)) {
          bGPDspoint &pt = gps->points[i];
          pt.pressure_3d = pt.pressure;
        }
      }

      /* Set constant strength flag */
      if (strength_is_constant) {
        gps->render_flag |= GP_ONDINE_STROKE_STRENGTH_IS_CONSTANT;
      }

      /* Determine wether a fill is clockwise or counterclockwise */
      /* See: https://en.wikipedia.org/wiki/Curve_orientation */
      gps->render_flag &= ~GP_ONDINE_STROKE_FILL_IS_CLOCKWISE;
      if (has_fill) {
        int lenp = gps->totpoints - 1;
        int min_i0 = (min_i1 == 0) ? lenp : min_i1 - 1;
        int min_i2 = (min_i1 == lenp) ? 0 : min_i1 + 1;
        float det =
          (gps->points[min_i1].flat_x - gps->points[min_i0].flat_x) *
          (gps->points[min_i2].flat_y - gps->points[min_i0].flat_y) -
          (gps->points[min_i2].flat_x - gps->points[min_i0].flat_x) *
          (gps->points[min_i1].flat_y - gps->points[min_i0].flat_y);
        if (det > 0) {
          gps->render_flag |= GP_ONDINE_STROKE_FILL_IS_CLOCKWISE;
        }
      }

      /* Set bounding box */
      gps->render_bbox[0] = bbox_minx;
      gps->render_bbox[1] = bbox_miny;
      gps->render_bbox[2] = bbox_maxx;
      gps->render_bbox[3] = bbox_maxy;
    }
  }
}

}  // namespace blender

void gpencil_ondine_set_render_data(Object *ob)
{
  blender::ondine_render->set_render_data(ob);
}

void gpencil_ondine_set_zdepth(Object *ob)
{
  blender::ondine_render->set_zdepth(ob);
}

bool gpencil_ondine_render_init(bContext *C)
{
  blender::ondine_render->init(C);
  return blender::ondine_render->prepare_camera_params(C);
}
