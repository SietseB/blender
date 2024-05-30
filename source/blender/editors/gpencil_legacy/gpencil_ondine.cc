/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2008 Blender Foundation. */

/** \file
 * \ingroup edgpencil
 * Operators for Ondine watercolor Grease Pencil.
 */

#include "DNA_gpencil_legacy_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "BKE_camera.h"
#include "BKE_context.hh"
#include "BKE_gpencil_geom_legacy.h"
#include "BKE_gpencil_legacy.h"
#include "BKE_main.hh"
#include "BKE_material.h"
#include "BKE_scene.hh"
#include "BKE_screen.hh"

#include "BLI_ghash.h"
#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"

#include "WM_api.hH"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_gpencil_legacy.hh"
#include "ED_view3d.hh"

#include "gpencil_ondine.hh"

namespace blender {

/* Object instance of Ondine runtime render data. */
GpencilOndine *ondine_render = new GpencilOndine();

ARegion *get_invoke_region(bContext *C)
{
  bScreen *screen = CTX_wm_screen(C);
  if (screen == nullptr) {
    return nullptr;
  }
  ScrArea *area = BKE_screen_find_big_area(screen, SPACE_VIEW3D, 0);
  if (area == nullptr) {
    return nullptr;
  }

  ARegion *region = BKE_area_find_region_type(area, RGN_TYPE_WINDOW);

  return region;
}

View3D *get_invoke_view3d(bContext *C)
{
  bScreen *screen = CTX_wm_screen(C);
  if (screen == nullptr) {
    return nullptr;
  }
  ScrArea *area = BKE_screen_find_big_area(screen, SPACE_VIEW3D, 0);
  if (area == nullptr) {
    return nullptr;
  }
  if (area) {
    return (View3D *)area->spacedata.first;
  }

  return nullptr;
}

/* Runtime render properties. */
GpencilOndine::GpencilOndine() {}

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
  /* Get camera. */
  Scene *scene = DEG_get_evaluated_scene(depsgraph_);
  BKE_scene_camera_switch_update(scene);
  Object *cam_ob = scene->camera;
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
    invert_m4_m4(viewmat, cam_ob->object_to_world().ptr());

    mul_m4_m4m4(persmat_, params.winmat, viewmat);

    /* Store camera position and normal vector. */
    float cam_mat[3][3];
    camera_loc_ = cam_ob->loc;
    transpose_m3_m4(cam_mat, cam_ob->world_to_object().ptr());
    copy_v3_v3(camera_normal_vec_, cam_mat[2]);

    /* Store camera rotation. */
    camera_rot_sin_ = fabsf(sin(cam_ob->rot[0]));
    camera_rot_cos_ = fabsf(cos(cam_ob->rot[0]));
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

void GpencilOndine::set_unique_stroke_seeds(bContext *C, const bool current_frame_only)
{
  Main *bmain = CTX_data_main(C);
  LISTBASE_FOREACH (Object *, ob, &bmain->objects) {
    if (ob->type != OB_GPENCIL_LEGACY) {
      continue;
    }
    bGPdata *gpd = (bGPdata *)ob->data;
    if ((gpd->ondine_flag & GP_ONDINE_WATERCOLOR) == 0) {
      continue;
    }

    LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
      if (current_frame_only) {
        bGPDframe *gpf = gpl->actframe;
        if (gpf == nullptr) {
          continue;
        }
        GSet *seeds = BLI_gset_int_new("gps_seeds");
        LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
          while (BLI_gset_haskey(seeds, POINTER_FROM_INT(gps->seed))) {
            gps->seed = rand() * 4096 + rand();
          }
          BLI_gset_add(seeds, POINTER_FROM_INT(gps->seed));
        }
        BLI_gset_free(seeds, nullptr);
      }
      else {
        LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
          GSet *seeds = BLI_gset_int_new("gps_seeds");
          LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
            while (BLI_gset_haskey(seeds, POINTER_FROM_INT(gps->seed))) {
              gps->seed = rand() * 4096 + rand();
            }
            BLI_gset_add(seeds, POINTER_FROM_INT(gps->seed));
          }
          BLI_gset_free(seeds, nullptr);
        }
      }
    }
  }
}

float2 GpencilOndine::gpencil_3D_point_to_2D(const float3 co)
{
  float3 parent_co = math::transform_point(diff_mat_, co);

  float2 r_co;
  mul_v2_project_m4_v3(&r_co.x, persmat_, &parent_co.x);
  r_co.x = (r_co.x + 1.0f) / 2.0f * (float)render_x_;
  r_co.y = (float)render_y_ - (r_co.y + 1.0f) / 2.0f * (float)render_y_;

  return r_co;
}

float GpencilOndine::stroke_point_radius_get(bGPDstroke *gps,
                                             const int p_index,
                                             const float thickness)
{
  float stroke_radius = (thickness / defaultpixsize_) / 2.0f;

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
  float radius = len_v2(v1);

  return math::max(radius, 1.0f);
}

void GpencilOndine::get_vertex_color(const bGPDlayer *gpl,
                                     const MaterialGPencilStyle *gp_style,
                                     const bGPDspoint &point,
                                     const bool use_texture,
                                     float *r_color)
{
  const float vertex_factor = use_texture ? gp_style->mix_stroke_factor : point.vert_color[3];
  copy_v4_v4(stroke_color_, gp_style->stroke_rgba);
  interp_v3_v3v3(stroke_color_, stroke_color_, point.vert_color, vertex_factor);
  interp_v3_v3v3(r_color, stroke_color_, gpl->tintcolor, gpl->tintcolor[3]);
}

void GpencilOndine::set_stroke_color(const bGPDlayer *gpl,
                                     bGPDstroke *gps,
                                     const MaterialGPencilStyle *gp_style)
{
  float color[3];

  /* Get stroke color. */
  copy_v4_v4(stroke_color_, gp_style->stroke_rgba);
  interp_v3_v3v3(
      stroke_color_, stroke_color_, gps->points[0].vert_color, gps->points[0].vert_color[3]);
  interp_v3_v3v3(color, stroke_color_, gpl->tintcolor, gpl->tintcolor[3]);
  copy_v3_v3(gps->runtime.render_stroke_color, color);

  /* Get fill color. */
  copy_v4_v4(fill_color_, gp_style->fill_rgba);
  const float vertex_factor = (gp_style->fill_style == GP_MATERIAL_FILL_STYLE_GRADIENT) ?
                                  gp_style->mix_factor :
                                  gps->vert_color_fill[3];
  interp_v3_v3v3(fill_color_, fill_color_, gps->vert_color_fill, vertex_factor);
  interp_v3_v3v3(color, fill_color_, gpl->tintcolor, gpl->tintcolor[3]);
  copy_v3_v3(gps->runtime.render_fill_color, color);
  gps->runtime.render_fill_opacity = fill_color_[3] * gpl->opacity;
}

void GpencilOndine::set_zdepth(Object *object)
{
  /* Grease pencil object? */
  if (object->type != OB_GPENCIL_LEGACY) {
    return;
  }

  /* Ondine watercolor object? */
  bGPdata *gpd = (bGPdata *)object->data;
  if ((gpd->ondine_flag & GP_ONDINE_WATERCOLOR) == 0) {
    return;
  }

  /* Save z-depth from view to sort from back to front. */
  gpd->runtime.render_zdepth = dot_v3v3(camera_z_axis_, object->object_to_world()[3]);
}

void GpencilOndine::set_render_data(Object *object, const blender::float4x4 matrix_world)
{
  /* Grease pencil object? */
  if (object->type != OB_GPENCIL_LEGACY) {
    return;
  }

  /* Ondine watercolor object? */
  bGPdata *gpd = (bGPdata *)object->data;
  if ((gpd->ondine_flag & GP_ONDINE_WATERCOLOR) == 0) {
    return;
  }

  /* Iterate all layers of GP watercolor object. */
  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    /* Layer is hidden? */
    if (gpl->flag & GP_LAYER_HIDE) {
      continue;
    }

    /* Active keyframe? */
    bGPDframe *gpf = gpl->actframe;
    if ((gpf == nullptr) || (gpf->strokes.first == nullptr)) {
      continue;
    }

    /* Prepare layer matrix and pixel size. */
    BKE_gpencil_layer_transform_matrix_get(depsgraph_, object, gpl, diff_mat_.ptr());
    diff_mat_ = diff_mat_ * float4x4(gpl->layer_invmat);
    defaultpixsize_ = 1000.0f / gpd->pixfactor;

    /* Iterate all strokes of layer. */
    LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
      if (!ED_gpencil_stroke_material_visible(object, gps)) {
        continue;
      }

      /* Set fill and stroke flags. */
      MaterialGPencilStyle *gp_style = BKE_gpencil_material_settings(object, gps->mat_nr + 1);

      const bool has_stroke = ((gp_style->flag & GP_MATERIAL_STROKE_SHOW) &&
                               (gp_style->stroke_rgba[3] > GPENCIL_ALPHA_OPACITY_THRESH));
      const bool has_fill = ((gp_style->flag & GP_MATERIAL_FILL_SHOW) &&
                             (gp_style->fill_rgba[3] > GPENCIL_ALPHA_OPACITY_THRESH));
      const bool use_texture = (gp_style->stroke_style == GP_MATERIAL_STROKE_STYLE_TEXTURE &&
                                gp_style->sima != nullptr && !has_fill);

      gps->runtime.render_flag = 0;
      if (has_stroke) {
        gps->runtime.render_flag |= GP_ONDINE_STROKE_HAS_STROKE;
      }
      if (has_fill) {
        gps->runtime.render_flag |= GP_ONDINE_STROKE_HAS_FILL;
      }

      /* Set stroke and fill color, in linear sRGB. */
      set_stroke_color(gpl, gps, gp_style);

      /* Determine size of 2D point data. */
      const bool make_cyclic = (has_fill || (gps->flag & GP_STROKE_CYCLIC) != 0);
      gps->totpoints_2d = gps->totpoints;
      if (make_cyclic) {
        gps->totpoints_2d++;
      }

      /* Create array for 2D point data. */
      MEM_SAFE_FREE(gps->points_2d);
      gps->points_2d = (bGPDspoint2D *)MEM_malloc_arrayN(
          gps->totpoints_2d, sizeof(bGPDspoint2D), __func__);

      /* Init min/max calculations. */
      float strength = (int)(gps->points[0].strength * 1000 + 0.5);
      strength = (float)strength / 1000;
      float min_y = FLT_MAX;
      float max_x = -FLT_MAX;
      int min_i1 = 0;
      float bbox_minx = FLT_MAX, bbox_miny = FLT_MAX;
      float bbox_maxx = -FLT_MAX, bbox_maxy = -FLT_MAX;
      float dist_to_cam = 0.0f;
      float min_dist_to_cam = -FLT_MAX, max_dist_to_cam = FLT_MAX;
      int min_dist_point_index = 0;

      /* Convert 3D stroke points to 2D. */
      for (const int i : IndexRange(gps->totpoints)) {
        /* Apply object world matrix (given by object instances). */
        float3 co;
        bGPDspoint &pt = gps->points[i];
        copy_v3_v3(co, &pt.x);
        co = math::transform_point(matrix_world, co);

        /* Convert to 2D space. */
        bGPDspoint2D &pt_2d = gps->points_2d[i];
        const float2 screen_co = gpencil_3D_point_to_2D(co);
        pt_2d.data[ONDINE_X] = screen_co.x;
        pt_2d.data[ONDINE_Y] = screen_co.y;
        pt_2d.data[ONDINE_STRENGTH] = pt.strength;

        /* Set vertex color. */
        get_vertex_color(gpl, gp_style, gps->points[i], use_texture, &pt_2d.data[ONDINE_COLOR]);

        /* Get distance to camera.
         * Somehow we have to apply the object world matrix here again, I don't know why... */
        mul_m4_v3(object->object_to_world().ptr(), co);
        dist_to_cam = math::min(0.0f, math::dot(co - camera_loc_, camera_normal_vec_));
        pt_2d.data[ONDINE_DIST_TO_CAM] = dist_to_cam;

        /* Keep track of closest/furthest point to camera. */
        if (dist_to_cam < max_dist_to_cam) {
          max_dist_to_cam = dist_to_cam;
        }
        if (dist_to_cam > min_dist_to_cam) {
          min_dist_to_cam = dist_to_cam;
          min_dist_point_index = i;
        }

        /* Keep track of minimum y point. */
        if (screen_co.y <= min_y) {
          if ((pt_2d.data[ONDINE_Y] < min_y) || (screen_co.x > max_x)) {
            min_i1 = i;
            min_y = screen_co.y;
            max_x = screen_co.x;
          }
        }

        /* Get bounding box. */
        if (bbox_minx > screen_co.x) {
          bbox_minx = screen_co.x;
        }
        if (bbox_miny > screen_co.y) {
          bbox_miny = screen_co.y;
        }
        if (bbox_maxx < screen_co.x) {
          bbox_maxx = screen_co.x;
        }
        if (bbox_maxy < screen_co.y) {
          bbox_maxy = screen_co.y;
        }
      }

      /* Calculate stroke width. */
      bool pressure_is_set = false;
      bool out_of_view = true;
      float max_pressure = 0.001f;
      gps->runtime.render_stroke_radius = 0.0f;
      if (has_stroke) {
        /* Get stroke thickness, taking object scale and layer line change into account. */
        float thickness = gps->thickness + gpl->line_change;
        thickness *= mat4_to_scale(object->object_to_world().ptr());
        CLAMP_MIN(thickness, 1.0f);
        const float max_stroke_radius = stroke_point_radius_get(
            gps, min_dist_point_index, thickness);
        gps->runtime.render_stroke_radius = max_stroke_radius;

        /* Adjust point pressure based on distance to camera.
         * That way a stroke will get thinner when it is further away from the camera. */
        if ((min_dist_to_cam - max_dist_to_cam) > FLT_EPSILON) {
          pressure_is_set = true;

          for (const int i : IndexRange(gps->totpoints)) {
            bGPDspoint &pt = gps->points[i];
            bGPDspoint2D &pt_2d = gps->points_2d[i];

            /* Adjust pressure based on camera distance.
             * Bit slow, but the most accurate way. */
            float radius = stroke_point_radius_get(gps, i, thickness);
            pt_2d.data[ONDINE_PRESSURE3D] = math::max(
                0.001f, pt.pressure * math::min(1.0f, radius / max_stroke_radius));
            if (pt_2d.data[ONDINE_PRESSURE3D] > max_pressure) {
              max_pressure = pt_2d.data[ONDINE_PRESSURE3D];
            }

            /* Point in view of camera? */
            radius = max_stroke_radius * pt_2d.data[ONDINE_PRESSURE3D];
            if ((pt_2d.data[ONDINE_X] + radius) >= 0.0f &&
                (pt_2d.data[ONDINE_X] - radius) <= render_x_ &&
                (pt_2d.data[ONDINE_Y] + radius) >= 0.0f &&
                (pt_2d.data[ONDINE_Y] - radius) <= render_y_)
            {
              out_of_view = false;
            }
          }
        }
      }
      if (!pressure_is_set) {
        for (const int i : IndexRange(gps->totpoints)) {
          bGPDspoint &pt = gps->points[i];
          bGPDspoint2D &pt_2d = gps->points_2d[i];
          pt_2d.data[ONDINE_PRESSURE3D] = math::max(0.001f, pt.pressure);
          if (pt_2d.data[ONDINE_PRESSURE3D] > max_pressure) {
            max_pressure = pt_2d.data[ONDINE_PRESSURE3D];
          }

          /* Point in view of camera? */
          if (pt_2d.data[ONDINE_X] >= 0.0f && pt_2d.data[ONDINE_X] <= render_x_ &&
              pt_2d.data[ONDINE_Y] >= 0.0f && pt_2d.data[ONDINE_Y] <= render_y_)
          {
            out_of_view = false;
          }
        }
      }
      /* Normalize pressure. */
      if (max_pressure > 1.0f) {
        for (const int i : IndexRange(gps->totpoints)) {
          gps->points_2d[i].data[ONDINE_PRESSURE3D] /= max_pressure;
        }
        max_pressure = 1.0f;
      }
      gps->runtime.render_max_pressure = max_pressure;

      if (out_of_view) {
        gps->runtime.render_flag |= GP_ONDINE_STROKE_IS_OUT_OF_VIEW;
      }
      else {
        gps->runtime.render_flag &= ~GP_ONDINE_STROKE_IS_OUT_OF_VIEW;
      }

      /* Determine wether a fill is clockwise or counterclockwise. */
      /* See: https://en.wikipedia.org/wiki/Curve_orientation */
      gps->runtime.render_flag &= ~GP_ONDINE_STROKE_FILL_IS_CLOCKWISE;
      if (has_fill) {
        int lenp = gps->totpoints - 1;
        int min_i0 = (min_i1 == 0) ? lenp : min_i1 - 1;
        int min_i2 = (min_i1 == lenp) ? 0 : min_i1 + 1;
        float det =
            (gps->points_2d[min_i1].data[ONDINE_X] - gps->points_2d[min_i0].data[ONDINE_X]) *
                (gps->points_2d[min_i2].data[ONDINE_Y] - gps->points_2d[min_i0].data[ONDINE_Y]) -
            (gps->points_2d[min_i2].data[ONDINE_X] - gps->points_2d[min_i0].data[ONDINE_X]) *
                (gps->points_2d[min_i1].data[ONDINE_Y] - gps->points_2d[min_i0].data[ONDINE_Y]);
        if (det > 0) {
          gps->runtime.render_flag |= GP_ONDINE_STROKE_FILL_IS_CLOCKWISE;
        }
      }

      /* When the stroke is cyclic, repeat the first point at the end. */
      if (make_cyclic) {
        memcpy(&gps->points_2d[gps->totpoints_2d - 1], &gps->points_2d[0], sizeof(bGPDspoint2D));
      }

      /* Add padding to 2D points. */
      for (const int i : IndexRange(gps->totpoints_2d)) {
        gps->points_2d[i].data[ONDINE_X] += IMAGE_PADDING;
        gps->points_2d[i].data[ONDINE_Y] += IMAGE_PADDING;
      }

      /* Set bounding box. */
      gps->runtime.render_bbox[0] = bbox_minx + IMAGE_PADDING;
      gps->runtime.render_bbox[1] = bbox_miny + IMAGE_PADDING;
      gps->runtime.render_bbox[2] = bbox_maxx + IMAGE_PADDING;
      gps->runtime.render_bbox[3] = bbox_maxy + IMAGE_PADDING;
      gps->runtime.render_dist_to_camera = max_dist_to_cam;
    }
  }
}

}  // namespace blender

void gpencil_ondine_set_render_data(Object *ob, const float mat[4][4])
{
  blender::ondine_render->set_render_data(ob, blender::float4x4(mat));
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

static int gpencil_ondine_set_unique_stroke_seeds(bContext *C, wmOperator *op)
{
  const bool current_frame_only = RNA_boolean_get(op->ptr, "current_frame");
  blender::ondine_render->set_unique_stroke_seeds(C, current_frame_only);
  return OPERATOR_FINISHED;
}

/* Operator definition: ondine_set_unique_stroke_seeds. */
void GPENCIL_OT_ondine_set_unique_stroke_seeds(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Set Unique Stroke Seeds";
  ot->idname = "GPENCIL_OT_ondine_set_unique_stroke_seeds";
  ot->description = "Set unique stroke seeds in each frame for Ondine watercolor rendering";

  /* api callbacks */
  ot->exec = gpencil_ondine_set_unique_stroke_seeds;

  ot->prop = RNA_def_boolean(ot->srna, "current_frame", true, "Current Frame Only", "");
}

/* Init Ondine watercolor rendering for current frame. */
static int gpencil_ondine_render_init_exec(bContext *C, wmOperator * /*op*/)
{
  bool success = gpencil_ondine_render_init(C);
  if (!success) {
    return OPERATOR_CANCELLED;
  }

  return OPERATOR_FINISHED;
}

/* Operator definition: ondine_render_init. */
void GPENCIL_OT_ondine_render_init(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Init Ondine rendering";
  ot->idname = "GPENCIL_OT_ondine_render_init";
  ot->description = "Initialize Ondine watercolor rendering for current frame";

  /* api callbacks */
  ot->exec = gpencil_ondine_render_init_exec;
}
