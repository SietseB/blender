/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2008 Blender Foundation. */

/** \file
 * \ingroup edgpencil
 * Operators for Ondine watercolor Grease Pencil.
 */

#include "DNA_material_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "BKE_camera.h"
#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_grease_pencil.hh"
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

#include "ED_grease_pencil.hh"
#include "ED_grease_pencil_ondine.hh"
#include "ED_view3d.hh"

namespace blender::ondine {

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

bool GpencilOndine::prepare_camera_params()
{
  /* Get camera. */
  Scene *scene = DEG_get_evaluated_scene(depsgraph_);
  BKE_scene_camera_switch_update(scene);
  Object *cam_ob = scene->camera;

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

  /* Camera position. */
  /* TODO: Can we get this in another way? Then we won't need `rv3d_` any more! */
  copy_v3_v3(camera_z_axis_, rv3d_->viewinv[2]);

  render_x_ = float(scene_->r.xsch * scene_->r.size) / 100.0f;
  render_y_ = float(scene_->r.ysch * scene_->r.size) / 100.0f;

  return true;
}

void GpencilOndine::set_unique_stroke_seeds(bContext *C, const bool current_frame_only)
{
  const Main *bmain = CTX_data_main(C);
  const Scene &scene = *CTX_data_scene(C);

  LISTBASE_FOREACH (Object *, ob, &bmain->objects) {
    if (ob->type != OB_GREASE_PENCIL) {
      continue;
    }

    GreasePencil &grease_pencil = *static_cast<GreasePencil *>(ob->data);
    if ((grease_pencil.ondine_flag & GP_ONDINE_WATERCOLOR) == 0) {
      continue;
    }

    auto set_seed = [](bke::greasepencil::Drawing *drawing) {
      MutableSpan<int> seeds = drawing->seeds_for_write();
      Set<int> used_seeds;
      for (const int seed_i : seeds.index_range()) {
        while (seeds[seed_i] == 0 || used_seeds.contains(seeds[seed_i])) {
          seeds[seed_i] = rand() * 4096 + rand();
        }
        used_seeds.add(seeds[seed_i]);
      }
    };

    if (current_frame_only) {
      for (const bke::greasepencil::Layer *layer : grease_pencil.layers()) {
        bke::greasepencil::Drawing *drawing = grease_pencil.get_drawing_at(*layer, scene.r.cfra);
        if (drawing == nullptr) {
          continue;
        }
        set_seed(drawing);
      }
    }
    else {
      for (GreasePencilDrawingBase *drawing_base : grease_pencil.drawings()) {
        if (drawing_base->type != GP_DRAWING) {
          continue;
        }
        bke::greasepencil::Drawing *drawing = reinterpret_cast<bke::greasepencil::Drawing *>(
            drawing_base);
        set_seed(drawing);
      }
    }
  }
}

float2 GpencilOndine::gpencil_3d_point_to_2d(const float3 co)
{
  float3 parent_co = math::transform_point(diff_mat_, co);

  float2 r_co;
  mul_v2_project_m4_v3(&r_co.x, persmat_, &parent_co.x);
  r_co.x = (r_co.x + 1.0f) / 2.0f * render_x_;
  r_co.y = render_y_ - (r_co.y + 1.0f) / 2.0f * render_y_;

  return r_co;
}

float GpencilOndine::stroke_point_radius_get(const float3 point, const float thickness)
{
  const float stroke_radius = thickness * 0.5f;
  float3 p1, p2;
  p1.x = point.x;
  p1.y = point.y;
  p1.z = point.z;
  p2.x = point.x;
  p2.y = point.y + stroke_radius * camera_rot_cos_;
  p2.z = point.z + stroke_radius * camera_rot_sin_;

  const float2 screen_co1 = gpencil_3d_point_to_2d(p1);
  const float2 screen_co2 = gpencil_3d_point_to_2d(p2);
  const float2 v1 = screen_co1 - screen_co2;
  float radius = len_v2(v1);

  return math::max(radius, 1.0f);
}

void GpencilOndine::get_vertex_color(const MaterialGPencilStyle *mat_style,
                                     const ColorGeometry4f &vertex_color,
                                     const bool use_texture,
                                     float *r_color)
{
  const float vertex_factor = use_texture ? mat_style->mix_stroke_factor : vertex_color.a;
  interp_v3_v3v3(r_color, mat_style->stroke_rgba, vertex_color, vertex_factor);
}

void GpencilOndine::set_fill_color(const ColorGeometry4f &fill_color,
                                   const MaterialGPencilStyle *mat_style,
                                   const bke::greasepencil::Layer &layer,
                                   OndineRenderStroke &r_render_stroke)
{
  float color[3];
  const float vertex_factor = (mat_style->fill_style == GP_MATERIAL_FILL_STYLE_GRADIENT) ?
                                  mat_style->mix_factor :
                                  fill_color[3];
  interp_v3_v3v3(color, mat_style->fill_rgba, fill_color, vertex_factor);
  copy_v3_v3(r_render_stroke.render_fill_color, color);
  r_render_stroke.render_fill_opacity = mat_style->fill_rgba[3] * layer.opacity;
}

void GpencilOndine::set_zdepth(Object *object)
{
  /* Grease pencil object? */
  if (object->type != OB_GREASE_PENCIL) {
    return;
  }

  /* Ondine watercolor object? */
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);
  if ((grease_pencil.ondine_flag & GP_ONDINE_WATERCOLOR) == 0) {
    return;
  }

  /* Save z-depth from view to sort from back to front. */
  grease_pencil.runtime->render_zdepth = dot_v3v3(camera_z_axis_, object->object_to_world()[3]);
}

void GpencilOndine::set_render_data(Object *object, const blender::float4x4 matrix_world)
{
  /* Grease pencil object? */
  if (object->type != OB_GREASE_PENCIL) {
    return;
  }

  /* Ondine watercolor object? */
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);
  if ((grease_pencil.ondine_flag & GP_ONDINE_WATERCOLOR) == 0) {
    return;
  }

  /* Iterate all layers of GP watercolor object. */
  for (const bke::greasepencil::Layer *layer : grease_pencil.layers()) {
    /* Layer is hidden? */
    if (!layer->is_visible())
      continue;

    /* Active keyframe? */
    bke::greasepencil::Drawing *drawing = grease_pencil.get_drawing_at(*layer, scene_->r.cfra);
    if (drawing == nullptr) {
      continue;
    }
    if (drawing->strokes().curves_num() == 0) {
      continue;
    }

    /* TODO: Prepare layer matrix and pixel size. */
    const float4x4 viewmat = layer->to_world_space(*object);
    const float object_scale = mat4_to_scale(object->object_to_world().ptr());

    const bke::CurvesGeometry &curves = drawing->strokes();
    const OffsetIndices<int> points_by_curve = curves.points_by_curve();
    drawing->runtime->points_2d.reinitialize(curves.points_num());
    Array<GPStrokePoint> &points_2d = drawing->runtime->points_2d;
    drawing->runtime->render_strokes.reinitialize(curves.curves_num());
    Array<OndineRenderStroke> &render_strokes = drawing->runtime->render_strokes;
    memset(render_strokes.data(), 0, render_strokes.as_span().size_in_bytes());

    const Span<float3> positions = curves.positions();
    const VArray<ColorGeometry4f> fill_colors = drawing->fill_colors();
    const VArray<bool> cyclic = curves.cyclic();
    const VArray<int> materials = *curves.attributes().lookup_or_default<int>(
        "material_index", bke::AttrDomain::Curve, 0);
    const VArray<float> opacities = drawing->opacities();
    const VArray<float> radii = drawing->radii();
    const VArray<ColorGeometry4f> vertex_colors = drawing->vertex_colors();

    threading::parallel_for(curves.curves_range(), 64, [&](const IndexRange curve_range) {
      for (const int curve_i : curve_range) {
        const IndexRange points = points_by_curve[curve_i];

        /* Set fill and stroke flags. */
        MaterialGPencilStyle *mat_style = BKE_gpencil_material_settings(object,
                                                                        materials[curve_i] + 1);
        if (mat_style->flag & GP_MATERIAL_HIDE) {
          continue;
        }
        const bool has_stroke = ((mat_style->flag & GP_MATERIAL_STROKE_SHOW) &&
                                 (mat_style->stroke_rgba[3] > GPENCIL_ALPHA_OPACITY_THRESHOLD));
        const bool has_fill = ((mat_style->flag & GP_MATERIAL_FILL_SHOW) &&
                               (mat_style->fill_rgba[3] > GPENCIL_ALPHA_OPACITY_THRESHOLD));
        const bool use_texture = (mat_style->stroke_style == GP_MATERIAL_STROKE_STYLE_TEXTURE &&
                                  mat_style->sima != nullptr && !has_fill);

        if (has_stroke) {
          render_strokes[curve_i].render_flag |= GP_ONDINE_STROKE_HAS_STROKE;
        }
        if (has_fill) {
          render_strokes[curve_i].render_flag |= GP_ONDINE_STROKE_HAS_FILL;

          /* Set fill color, in linear sRGB. */
          set_fill_color(fill_colors[curve_i], mat_style, *layer, render_strokes[curve_i]);
        }
        if (cyclic[curve_i] || has_fill) {
          render_strokes[curve_i].render_flag |= GP_ONDINE_STROKE_IS_CYCLIC;
        }

        /* Init min/max calculations. */
        float min_y = FLT_MAX;
        float max_x = -FLT_MAX;
        int min_i1 = 0;
        float bbox_minx = FLT_MAX, bbox_miny = FLT_MAX;
        float bbox_maxx = -FLT_MAX, bbox_maxy = -FLT_MAX;
        float dist_to_cam = 0.0f;
        float min_dist_to_cam = -FLT_MAX, max_dist_to_cam = FLT_MAX;
        int min_dist_point_index = 0;

        /* Convert 3D stroke points to 2D. */
        for (const int point : points_by_curve[curve_i]) {
          /* Apply layer matrix. */
          float3 co = math::transform_point(viewmat, positions[point]);

          /* Apply object world matrix (given by object instances). */
          co = math::transform_point(matrix_world, co);

          /* Convert to 2D space. */
          const float2 screen_co = gpencil_3d_point_to_2d(co);
          points_2d[point].x = screen_co.x;
          points_2d[point].y = screen_co.y;
          points_2d[point].alpha = opacities[point];

          /* Set vertex color. */
          get_vertex_color(
              mat_style, vertex_colors[point], use_texture, &points_2d[point].color_r);

          /* Get distance to camera.
           * Somehow we have to apply the object world matrix here again, I don't know why... */
          mul_m4_v3(object->object_to_world().ptr(), co);
          dist_to_cam = math::min(0.0f, math::dot(co - camera_loc_, camera_normal_vec_));
          points_2d[point].dist_to_cam = dist_to_cam;

          /* Keep track of closest/furthest point to camera. */
          if (dist_to_cam < max_dist_to_cam) {
            max_dist_to_cam = dist_to_cam;
          }
          if (dist_to_cam > min_dist_to_cam) {
            min_dist_to_cam = dist_to_cam;
            min_dist_point_index = point;
          }

          /* Keep track of minimum y point. */
          if (screen_co.y <= min_y) {
            if ((points_2d[point].y < min_y) || (screen_co.x > max_x)) {
              min_i1 = point;
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
        float max_radius = 0.001f;
        render_strokes[curve_i].render_stroke_radius = 0.0f;
        if (has_stroke) {
          /* Get stroke thickness, taking object scale into account. */
          const float max_stroke_radius = stroke_point_radius_get(positions[min_dist_point_index],
                                                                  object_scale);
          render_strokes[curve_i].render_stroke_radius = max_stroke_radius;

          /* Adjust point pressure based on distance to camera.
           * That way a stroke will get thinner when it is further away from the camera. */
          if ((min_dist_to_cam - max_dist_to_cam) > FLT_EPSILON) {
            pressure_is_set = true;

            for (const int point : points_by_curve[curve_i]) {
              /* Adjust pressure based on camera distance. Bit slow, but the most accurate way. */
              float radius = stroke_point_radius_get(positions[point], object_scale);
              points_2d[point].radius = math::max(
                  0.001f, radii[point] * math::min(1.0f, radius / max_stroke_radius));
              max_radius = math::max(max_radius, points_2d[point].radius);

              /* Point in view of camera? */
              radius = max_stroke_radius * points_2d[point].radius;
              if ((points_2d[point].x + radius) >= 0.0f &&
                  (points_2d[point].x - radius) >= render_x_ &&
                  (points_2d[point].y + radius) >= 0.0f &&
                  (points_2d[point].y - radius) >= render_y_)
              {
                out_of_view = false;
              }
            }
          }
        }
        if (!pressure_is_set) {
          for (const int point : points_by_curve[curve_i]) {
            points_2d[point].radius = math::max(0.001f, radii[point]);
            max_radius = math::max(max_radius, points_2d[point].radius);

            /* Point in view of camera? */
            if (points_2d[point].x >= 0.0f && points_2d[point].x <= render_x_ &&
                points_2d[point].y >= 0.0f && points_2d[point].y <= render_y_)
            {
              out_of_view = false;
            }
          }
        }
        /* Normalize pressure. */
        if (max_radius > 1.0f) {
          for (const int point : points_by_curve[curve_i]) {
            points_2d[point].radius /= max_radius;
          }
          max_radius = 1.0f;
        }
        render_strokes[curve_i].render_max_radius = max_radius;

        if (out_of_view) {
          render_strokes[curve_i].render_flag |= GP_ONDINE_STROKE_IS_OUT_OF_VIEW;
        }
        else {
          render_strokes[curve_i].render_flag &= ~GP_ONDINE_STROKE_IS_OUT_OF_VIEW;
        }

        /* Determine wether a fill is clockwise or counterclockwise.
         * See: https://en.wikipedia.org/wiki/Curve_orientation */
        render_strokes[curve_i].render_flag &= ~GP_ONDINE_STROKE_FILL_IS_CLOCKWISE;
        if (has_fill) {
          const IndexRange curve_range = points_by_curve[curve_i];
          const int min_i0 = (min_i1 == curve_range.first()) ? curve_range.last() : min_i1 - 1;
          const int min_i2 = (min_i1 == curve_range.last()) ? curve_range.first() : min_i1 + 1;
          const float det = (points_2d[min_i1].x - points_2d[min_i0].x) *
                                (points_2d[min_i2].y - points_2d[min_i0].y) -
                            (points_2d[min_i2].x - points_2d[min_i0].x) *
                                (points_2d[min_i1].y - points_2d[min_i0].y);
          if (det > 0.0f) {
            render_strokes[curve_i].render_flag |= GP_ONDINE_STROKE_FILL_IS_CLOCKWISE;
          }
        }

        /* Add padding to 2d points. */
        for (const int point : points_by_curve[curve_i]) {
          points_2d[point].x += IMAGE_PADDING;
          points_2d[point].y += IMAGE_PADDING;
        }

        /* Set bounding box. */
        render_strokes[curve_i].render_bbox[0] = bbox_minx + IMAGE_PADDING;
        render_strokes[curve_i].render_bbox[1] = bbox_miny + IMAGE_PADDING;
        render_strokes[curve_i].render_bbox[2] = bbox_maxx + IMAGE_PADDING;
        render_strokes[curve_i].render_bbox[3] = bbox_maxy + IMAGE_PADDING;
        render_strokes[curve_i].render_dist_to_camera = max_dist_to_cam;
      }
    });
  }
}

void gpencil_ondine_set_unique_stroke_seeds(bContext *C, const bool current_frame_only)
{
  ondine_render->set_unique_stroke_seeds(C, current_frame_only);
}

void gpencil_ondine_set_render_data(Object *ob, const float mat[4][4])
{
  ondine_render->set_render_data(ob, blender::float4x4(mat));
}

void gpencil_ondine_set_zdepth(Object *ob)
{
  ondine_render->set_zdepth(ob);
}

bool gpencil_ondine_render_init(bContext *C)
{
  ondine_render->init(C);
  return ondine_render->prepare_camera_params();
}

}  // namespace blender::ondine
