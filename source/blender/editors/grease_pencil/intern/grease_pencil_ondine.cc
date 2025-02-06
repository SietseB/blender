/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2008 Blender Foundation. */

/** \file
 * \ingroup edgpencil
 * Operators for Ondine watercolor Grease Pencil.
 */

#include "BKE_camera.h"
#include "BKE_curves.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_scene.hh"
#include "BKE_screen.hh"

#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"

#include "DEG_depsgraph_query.hh"

#include "ED_grease_pencil.hh"
#include "ED_grease_pencil_ondine.hh"
#include "ED_view3d.hh"

namespace blender::ondine {

/* Object instance of Ondine runtime render data. */
OndinePrepareRender *ondine_prepare_render = new OndinePrepareRender();

/* Runtime render properties. */
OndinePrepareRender::OndinePrepareRender() {}

void OndinePrepareRender::init(bContext *C)
{
  /* Easy access data. */
  this->depsgraph = CTX_data_depsgraph_pointer(C);
  this->scene = CTX_data_scene(C);
}

bool OndinePrepareRender::prepare_camera_params()
{
  /* Get camera. */
  Scene *scene = DEG_get_evaluated_scene(this->depsgraph);
  BKE_scene_camera_switch_update(scene);
  Object *camera = scene->camera;

  if (camera == nullptr) {
    return false;
  }

  /* Set up camera parameters. */
  CameraParams params;
  BKE_camera_params_init(&params);
  BKE_camera_params_from_object(&params, camera);

  /* Compute camera matrix, view-plane, etc. */
  RenderData *rd = &this->scene->r;
  BKE_camera_params_compute_viewplane(&params, rd->xsch, rd->ysch, rd->xasp, rd->yasp);
  BKE_camera_params_compute_matrix(&params);

  const float4x4 viewmat = math::invert(camera->object_to_world());
  this->camera_perspective_matrix = float4x4(params.winmat) * viewmat;

  /* Store camera position and normal vector. */
  this->camera_location = camera->loc;
  float cam_mat[3][3];
  transpose_m3_m4(cam_mat, camera->world_to_object().ptr());
  copy_v3_v3(this->camera_normal_vec, cam_mat[2]);

  /* Store camera rotation. */
  this->camera_rot_sin = math::abs(sin(camera->rot[0]));
  this->camera_rot_cos = math::abs(cos(camera->rot[0]));

  /* Store camera z-axis, for calculating z-depth of objects. */
  const float4x4 camera_to_world = math::normalize(camera->object_to_world());
  copy_v3_v3(this->camera_z_axis, camera_to_world[2]);

  this->render_width = float(this->scene->r.xsch * this->scene->r.size) / 100.0f;
  this->render_height = float(this->scene->r.ysch * this->scene->r.size) / 100.0f;
  this->render_size = {this->render_width, this->render_height};

  return true;
}

void OndinePrepareRender::set_unique_stroke_seeds(bContext *C, const bool current_frame_only)
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

float2 OndinePrepareRender::get_point_in_2d(const float3 &pos) const
{
  float2 co_2d = (float2(math::project_point(this->camera_perspective_matrix, pos)) + 1.0f) *
                 0.5f * this->render_size;
  co_2d.y = this->render_height - co_2d.y;

  return co_2d;
}

float OndinePrepareRender::get_stroke_point_radius(const float3 &point,
                                                   const float4x4 &transform_matrix) const
{
  const float3 world_co1 = math::project_point(transform_matrix, point);
  const float3 world_co2 = math::project_point(
      transform_matrix, point + float3({0.0f, this->camera_rot_cos, this->camera_rot_sin}));
  const float2 screen_co1 = get_point_in_2d(world_co1);
  const float2 screen_co2 = get_point_in_2d(world_co2);
  const float radius = len_v2(screen_co1 - screen_co2);

  return math::max(radius, 1.0f);
}

void OndinePrepareRender::get_vertex_color(const MaterialGPencilStyle *mat_style,
                                           const ColorGeometry4f &vertex_color,
                                           const bool use_texture,
                                           float *r_color)
{
  const float vertex_factor = use_texture ? mat_style->mix_stroke_factor : vertex_color.a;
  interp_v3_v3v3(r_color, mat_style->stroke_rgba, vertex_color, vertex_factor);
}

void OndinePrepareRender::set_fill_color(const ColorGeometry4f &fill_color,
                                         const MaterialGPencilStyle *mat_style,
                                         const bke::greasepencil::Layer &layer,
                                         OndineRenderStroke &r_render_stroke)
{
  const float vertex_factor = (mat_style->fill_style == GP_MATERIAL_FILL_STYLE_GRADIENT) ?
                                  mat_style->mix_factor :
                                  fill_color.a;
  interp_v3_v3v3(
      r_render_stroke.render_fill_color, mat_style->fill_rgba, fill_color, vertex_factor);
  r_render_stroke.render_fill_opacity = mat_style->fill_rgba[3] * layer.opacity;
}

void OndinePrepareRender::set_zdepth(Object *object,
                                     const float4x4 &object_instance_transform) const
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

  /* Save z-depth from camera view to sort from back to front. */
  // TODO: check if this calculation is correct...
  grease_pencil.runtime->render_zdepth = math::dot(
      this->camera_z_axis, (object_instance_transform * object->object_to_world()).location());
}

void OndinePrepareRender::set_render_data(Object *object,
                                          const float4x4 &object_instance_transform)
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
  threading::parallel_for(
      grease_pencil.layers().index_range(), 1, [&](const IndexRange layer_range) {
        for (const int layer_i : layer_range) {
          const bke::greasepencil::Layer &layer = grease_pencil.layer(layer_i);

          /* Layer is hidden? */
          if (!layer.is_visible())
            continue;

          /* Active keyframe? */
          bke::greasepencil::Drawing *drawing = grease_pencil.get_drawing_at(layer,
                                                                             this->scene->r.cfra);
          if (drawing == nullptr) {
            continue;
          }
          if (drawing->strokes().is_empty()) {
            continue;
          }

          const float4x4 layer_to_world = object_instance_transform *
                                          layer.to_world_space(*object);

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

          threading::parallel_for(curves.curves_range(), 128, [&](const IndexRange curve_range) {
            for (const int curve_i : curve_range) {
              const IndexRange points = points_by_curve[curve_i];

              /* Set fill and stroke flags. */
              MaterialGPencilStyle *mat_style = BKE_gpencil_material_settings(
                  object, materials[curve_i] + 1);
              if (mat_style->flag & GP_MATERIAL_HIDE) {
                continue;
              }
              const bool has_stroke = ((mat_style->flag & GP_MATERIAL_STROKE_SHOW) &&
                                       (mat_style->stroke_rgba[3] >
                                        GPENCIL_ALPHA_OPACITY_THRESHOLD));
              const bool has_fill = ((mat_style->flag & GP_MATERIAL_FILL_SHOW) &&
                                     (mat_style->fill_rgba[3] > GPENCIL_ALPHA_OPACITY_THRESHOLD));
              const bool use_texture = (mat_style->stroke_style ==
                                            GP_MATERIAL_STROKE_STYLE_TEXTURE &&
                                        mat_style->sima != nullptr && !has_fill);

              if (has_stroke) {
                render_strokes[curve_i].render_flag |= GP_ONDINE_STROKE_HAS_STROKE;
              }
              if (has_fill) {
                render_strokes[curve_i].render_flag |= GP_ONDINE_STROKE_HAS_FILL;

                /* Set fill color, in linear sRGB. */
                set_fill_color(fill_colors[curve_i], mat_style, layer, render_strokes[curve_i]);
              }
              if (cyclic[curve_i] || has_fill) {
                render_strokes[curve_i].render_flag |= GP_ONDINE_STROKE_IS_CYCLIC;
              }

              /* Init min/max calculations. */
              float min_y = FLT_MAX;
              float max_x = -FLT_MAX;
              int min_i1 = 0;
              float bbox_minx = FLT_MAX;
              float bbox_miny = FLT_MAX;
              float bbox_maxx = -FLT_MAX;
              float bbox_maxy = -FLT_MAX;
              float dist_to_cam = 0.0f;
              float min_dist_to_cam = -FLT_MAX;
              float max_dist_to_cam = FLT_MAX;
              int min_dist_point_index = 0;

              /* Convert 3D stroke points to 2D. */
              for (const int point : points_by_curve[curve_i]) {
                /* Convert coordinate to world space. */
                const float3 co = math::transform_point(layer_to_world, positions[point]);

                /* Convert to 2D space. */
                const float2 screen_co = get_point_in_2d(co);
                points_2d[point].x = screen_co.x;
                points_2d[point].y = screen_co.y;
                points_2d[point].alpha = opacities[point];

                /* Set vertex color. */
                get_vertex_color(
                    mat_style, vertex_colors[point], use_texture, &points_2d[point].color_r);

                /* Get distance to camera. */
                dist_to_cam = math::min(
                    0.0f, math::dot(co - this->camera_location, this->camera_normal_vec));
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
              bool radius_is_set = false;
              bool out_of_view = true;
              float max_radius = 0.001f;
              render_strokes[curve_i].render_stroke_radius = 0.0f;
              if (has_stroke) {
                /* Get the stroke thickness. */
                const float max_stroke_radius = get_stroke_point_radius(
                    positions[min_dist_point_index], layer_to_world);
                render_strokes[curve_i].render_stroke_radius = max_stroke_radius;

                /* Adjust point radius based on distance to camera.
                 * That way a stroke will get thinner when it is further away from the camera. */
                if ((min_dist_to_cam - max_dist_to_cam) > FLT_EPSILON) {
                  radius_is_set = true;

                  for (const int point : points_by_curve[curve_i]) {
                    /* Adjust point radius based on camera distance. Bit slow, but the most
                     * accurate way. */
                    float radius = get_stroke_point_radius(positions[point], layer_to_world);
                    points_2d[point].radius = math::max(
                        0.001f, radii[point] * math::min(1.0f, radius / max_stroke_radius));
                    max_radius = math::max(max_radius, points_2d[point].radius);

                    /* Point in view of camera? */
                    radius = max_stroke_radius * points_2d[point].radius;
                    if (out_of_view && (points_2d[point].x + radius) >= 0.0f &&
                        (points_2d[point].x - radius) <= this->render_width &&
                        (points_2d[point].y + radius) >= 0.0f &&
                        (points_2d[point].y - radius) <= this->render_height)
                    {
                      out_of_view = false;
                    }
                  }
                }
              }
              if (!radius_is_set) {
                for (const int point : points_by_curve[curve_i]) {
                  points_2d[point].radius = math::max(0.001f, radii[point]);
                  max_radius = math::max(max_radius, points_2d[point].radius);

                  /* Point in view of camera? */
                  if (out_of_view && points_2d[point].x >= 0.0f &&
                      points_2d[point].x <= this->render_width && points_2d[point].y >= 0.0f &&
                      points_2d[point].y <= this->render_height)
                  {
                    out_of_view = false;
                  }
                }
              }
              /* Normalize radius. */
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
                const int min_i0 = (min_i1 == curve_range.first()) ? curve_range.last() :
                                                                     min_i1 - 1;
                const int min_i2 = (min_i1 == curve_range.last()) ? curve_range.first() :
                                                                    min_i1 + 1;
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
      });
}

void ondine_set_unique_stroke_seeds(bContext *C, const bool current_frame_only)
{
  ondine_prepare_render->set_unique_stroke_seeds(C, current_frame_only);
}

void ondine_set_render_data(Object *ob, const float4x4 &object_instance_transform)
{
  ondine_prepare_render->set_render_data(ob, object_instance_transform);
}

void ondine_set_zdepth(Object *ob, const float4x4 &object_instance_transform)
{
  ondine_prepare_render->set_zdepth(ob, object_instance_transform);
}

bool ondine_render_init(bContext *C)
{
  ondine_prepare_render->init(C);
  return ondine_prepare_render->prepare_camera_params();
}

}  // namespace blender::ondine
