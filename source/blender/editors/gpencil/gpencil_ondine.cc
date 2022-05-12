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

  invert_axis_[0] = false;
  invert_axis_[1] = true;
}

bool GpencilOndine::prepare_camera_params(bContext *C)
{
  Object *cam_ob = scene_->camera;

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
    invert_m4_m4(viewmat, cam_ob->obmat);

    mul_m4_m4m4(persmat_, params.winmat, viewmat);
  }
  else {
    unit_m4(persmat_);
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
  r_co.y = (r_co.y + 1.0f) / 2.0f * (float)render_y_;

  /* Invert X axis. */
  if (invert_axis_[0]) {
    r_co.x = (float)render_x_ - r_co.x;
  }
  /* Invert Y axis. */
  if (invert_axis_[1]) {
    r_co.y = (float)render_y_ - r_co.y;
  }

  return r_co;
}

float GpencilOndine::stroke_point_radius_get(bGPDlayer *gpl, bGPDstroke *gps)
{
  bGPDspoint *pt = &gps->points[0];
  const float2 screen_co = gpencil_3D_point_to_2D(&pt->x);

  /* Radius. */
  bGPDstroke *gps_perimeter = BKE_gpencil_stroke_perimeter_from_view(
      rv3d_, gpd_, gpl, gps, 3, diff_mat_.values);

  pt = &gps_perimeter->points[0];
  const float2 screen_ex = gpencil_3D_point_to_2D(&pt->x);

  const float2 v1 = screen_co - screen_ex;
  float radius = math::length(v1);
  BKE_gpencil_free_stroke(gps_perimeter);

  return MAX2(radius, 1.0f);
}

void GpencilOndine::set_stroke_colors(Object *ob, bGPDlayer *gpl, bGPDstroke *gps, MaterialGPencilStyle *gp_style)
{
  /* Stroke color. */
  copy_v4_v4(stroke_color_, gp_style->stroke_rgba);
  avg_opacity_ = 0.0f;

  /* Get average vertex color and apply. */
  float avg_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  for (const bGPDspoint &pt : Span(gps->points, gps->totpoints)) {
    add_v4_v4(avg_color, pt.vert_color);
    avg_opacity_ += pt.strength;
  }
  mul_v4_v4fl(avg_color, avg_color, 1.0f / (float)gps->totpoints);
  interp_v3_v3v3(stroke_color_, stroke_color_, avg_color, avg_color[3]);
  avg_opacity_ /= (float)gps->totpoints;

  float col[3];
  interp_v3_v3v3(col, stroke_color_, gpl->tintcolor, gpl->tintcolor[3]);
  linearrgb_to_srgb_v3_v3(col, col);
  copy_v3_v3(gps->render_stroke_color, col);
  gps->render_stroke_opacity = stroke_color_[3] * avg_opacity_ * gpl->opacity;

  /* Get fill color and apply */
  copy_v4_v4(fill_color_, gp_style->fill_rgba);
  interp_v3_v3v3(fill_color_, fill_color_, gps->vert_color_fill, gps->vert_color_fill[3]);

  interp_v3_v3v3(col, fill_color_, gpl->tintcolor, gpl->tintcolor[3]);
  linearrgb_to_srgb_v3_v3(col, col);
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
  gpd->render_zdepth = dot_v3v3(camera_z_axis_, object->obmat[3]);
}

void GpencilOndine::set_render_data(Object *object)
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
  gpd_ = gpd;

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
      if (gps->totpoints < 2) {
        continue;
      }
      if (!ED_gpencil_stroke_material_visible(object, gps)) {
        continue;
      }

      /* Apply layer thickness and object scale to stroke thickness */
      /*  TODO!!!!
      gps->render_thickness = gps->thickness + gpl->line_change;
      gps->render_thickness *= mat4_to_scale(object->obmat);
      CLAMP_MIN(gps->render_thickness, 1.0f);
      */

      /* Set fill and stroke flags */
      MaterialGPencilStyle *gp_style = BKE_gpencil_material_settings(object, gps->mat_nr + 1);

      const bool is_stroke = ((gp_style->flag & GP_MATERIAL_STROKE_SHOW) &&
                              (gp_style->stroke_rgba[3] > GPENCIL_ALPHA_OPACITY_THRESH));
      const bool is_fill = ((gp_style->flag & GP_MATERIAL_FILL_SHOW) &&
                            (gp_style->fill_rgba[3] > GPENCIL_ALPHA_OPACITY_THRESH));

      gps->render_flag = 0;
      if (is_stroke) {
        gps->render_flag |= GP_ONDINE_STROKE_HAS_STROKE;
      }
      if (is_fill) {
        gps->render_flag |= GP_ONDINE_STROKE_HAS_FILL;
      }

      /* Set stroke and fill color */
      this->set_stroke_colors(object, gpl, gps, gp_style);

      /* Calculate stroke width */
      gps->render_stroke_width = 0.0f;
      if (is_stroke) {
        /* TODO: find simpler way to calculate this!!!!!!!!!!! */

        /* Get the thickness in pixels using a simple 1 point stroke. */
        const float max_pressure = BKE_gpencil_stroke_max_pressure_get(gps);

        bGPDstroke *gps_temp = BKE_gpencil_stroke_duplicate(gps, false, false);
        gps_temp->totpoints = 1;
        gps_temp->points = MEM_new<bGPDspoint>("gp_stroke_points");
        bGPDspoint *pt_src = &gps->points[0];
        bGPDspoint *pt_dst = &gps_temp->points[0];
        copy_v3_v3(&pt_dst->x, &pt_src->x);
        pt_dst->pressure = max_pressure;

        const float radius = this->stroke_point_radius_get(gpl, gps_temp);
        gps->render_stroke_width = radius * 2.0f;

        BKE_gpencil_free_stroke(gps_temp);
      }

      /* Convert 3d stroke points to 2d */
      for (const int i : IndexRange(gps->totpoints)) {
        bGPDspoint &pt = gps->points[i];
        const float2 screen_co = this->gpencil_3D_point_to_2D(&pt.x);
        pt.flat_x = screen_co.x;
        pt.flat_y = screen_co.y;
      }
    }
  }
}

}  // namespace blender

void gpencil_ondine_set_zdepth(Object *ob)
{
  blender::ondine_render->set_zdepth(ob);
}

bool gpencil_ondine_render_init(bContext *C)
{
  blender::ondine_render->init(C);
  return blender::ondine_render->prepare_camera_params(C);
}
