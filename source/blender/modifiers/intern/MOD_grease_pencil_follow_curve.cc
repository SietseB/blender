/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#include "DNA_defaults.h"
#include "DNA_modifier_types.h"

#include "RNA_access.hh"

#include "BKE_curve.hh"
#include "BKE_curves.hh"
#include "BKE_fcurve.hh"
#include "BKE_geometry_set.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_lib_query.hh"
#include "BKE_modifier.hh"

#include "BLI_hash.h"
#include "BLI_math_matrix.h"
#include "BLI_rand.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "BLO_read_write.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "BLT_translation.hh"

#include "WM_types.hh"

#include "RNA_prototypes.hh"

#include "MOD_grease_pencil_util.hh"
#include "MOD_ui_common.hh"

namespace blender {

using bke::greasepencil::Drawing;
using bke::greasepencil::FramesMapKeyT;
using bke::greasepencil::Layer;

static void init_data(ModifierData *md)
{
  auto *mmd = reinterpret_cast<GreasePencilFollowCurveModifierData *>(md);

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(mmd, modifier));

  MEMCPY_STRUCT_AFTER(mmd, DNA_struct_default_get(GreasePencilFollowCurveModifierData), modifier);
  modifier::greasepencil::init_influence_data(&mmd->influence, false);
}

static void copy_data(const ModifierData *md, ModifierData *target, const int flag)
{
  const auto *mmd = reinterpret_cast<const GreasePencilFollowCurveModifierData *>(md);
  auto *tmmd = reinterpret_cast<GreasePencilFollowCurveModifierData *>(target);

  modifier::greasepencil::free_influence_data(&tmmd->influence);

  BKE_modifier_copydata_generic(md, target, flag);
  modifier::greasepencil::copy_influence_data(&mmd->influence, &tmmd->influence, flag);
}

static void free_data(ModifierData *md)
{
  auto *mmd = reinterpret_cast<GreasePencilFollowCurveModifierData *>(md);
  modifier::greasepencil::free_influence_data(&mmd->influence);
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  auto *mmd = reinterpret_cast<GreasePencilFollowCurveModifierData *>(md);
  modifier::greasepencil::foreach_influence_ID_link(&mmd->influence, ob, walk, user_data);
  walk(user_data, ob, (ID **)&mmd->object, IDWALK_CB_NOP);
}

static void update_depsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  auto *mmd = reinterpret_cast<GreasePencilFollowCurveModifierData *>(md);
  if (mmd->object != nullptr) {
    DEG_add_object_relation(
        ctx->node, mmd->object, DEG_OB_COMP_TRANSFORM, "Grease Pencil Follow Curve Modifier");
    DEG_add_object_relation(
        ctx->node, mmd->object, DEG_OB_COMP_GEOMETRY, "Grease Pencil Follow Curve Modifier");
  }
  DEG_add_depends_on_transform_relation(ctx->node, "Grease Pencil Follow Curve Modifier");
}

static bool is_disabled(const Scene * /*scene*/, ModifierData *md, bool /*use_render_params*/)
{
  auto *mmd = reinterpret_cast<GreasePencilFollowCurveModifierData *>(md);

  return (mmd->object == nullptr);
}

static void frame_init(GreasePencilFollowCurveModifierData *mmd,
                       const ModifierEvalContext *ctx,
                       GreasePencil &grease_pencil)
{
  /* Get animated speed and speed variation. */
  mmd->speed_per_frame_len = 0;
  FCurve *speed_fcurve = id_data_find_fcurve(
      &ctx->object->id, mmd, &RNA_FollowCurveGpencilModifier, "speed", 0, nullptr);
  FCurve *speed_var_fcurve = id_data_find_fcurve(
      &ctx->object->id, mmd, &RNA_FollowCurveGpencilModifier, "speed_var", 0, nullptr);
  if (speed_fcurve != nullptr || speed_var_fcurve != nullptr) {
    mmd->speed_per_frame_len = mmd->cfra;
  }

  /* When animated, create array with speed and speed variation per frame. */
  mmd->speed_per_frame = nullptr;
  if (mmd->speed_per_frame_len > 0) {
    /* One stride contains: speed, speed variation. */
    mmd->speed_per_frame = static_cast<float *>(
        MEM_malloc_arrayN(mmd->cfra * 2, sizeof(float), __func__));
    for (int frame = 1; frame <= mmd->cfra; frame++) {
      float speed = speed_fcurve ? evaluate_fcurve(speed_fcurve, float(frame)) : mmd->speed;
      float speed_var = speed_var_fcurve ? evaluate_fcurve(speed_var_fcurve, float(frame)) :
                                           mmd->speed_variation;
      int stride = frame * 2;
      mmd->speed_per_frame[stride] = speed;
      mmd->speed_per_frame[stride + 1] = speed_var;
    }
  }

  /* Count Bezier curves in object. */
  mmd->curves_len = 0;
  if (mmd->object && mmd->object->type == OB_CURVES_LEGACY) {
    /* Loop splines. */
    Curve *curve = static_cast<Curve *>(mmd->object->data);
    LISTBASE_FOREACH (Nurb *, nurb, &curve->nurb) {
      if (nurb->type == CU_BEZIER) {
        mmd->curves_len++;
      }
    }
  }

  /* Convert Bezier curves to points. */
  const Object *ob_eval = static_cast<Object *>(
      DEG_get_evaluated_object(ctx->depsgraph, mmd->object));
  mmd->curves = nullptr;
  if (mmd->curves_len > 0) {
    mmd->curves = static_cast<GreasePencilFollowCurve *>(
        MEM_calloc_arrayN(mmd->curves_len, sizeof(GreasePencilFollowCurve), __func__));

    Curve *curve = static_cast<Curve *>(ob_eval->data);
    int curve_index = -1;

    /* Loop splines. */
    LISTBASE_FOREACH (Nurb *, nurb, &curve->nurb) {
      if (nurb->type != CU_BEZIER) {
        continue;
      }
      curve_index++;
      GreasePencilFollowCurve *follow_curve = &mmd->curves[curve_index];

      /* Count points in spline segments. */
      int segments = nurb->pntsu;
      if ((nurb->flagu & CU_NURB_CYCLIC) == 0) {
        segments--;
      }
      follow_curve->points_len = segments * mmd->curve_resolution;
      follow_curve->curve = curve;

      /* Create array for curve point data. */
      const int stride = sizeof(GreasePencilFollowCurvePoint);
      follow_curve->points = static_cast<GreasePencilFollowCurvePoint *>(
          MEM_mallocN((follow_curve->points_len + 1) * stride, __func__));
      GreasePencilFollowCurvePoint *point_offset = follow_curve->points;

      /* Convert spline segments of Bezier curve to points. */
      threading::parallel_for(IndexRange(segments), 1, [&](const IndexRange range) {
        for (const int i : range) {
          const int i_next = (i + 1) % nurb->pntsu;
          BezTriple *bezt = &nurb->bezt[i];
          BezTriple *bezt_next = &nurb->bezt[i_next];
          for (int axis = 0; axis < 3; axis++) {
            BKE_curve_forward_diff_bezier(
                bezt->vec[1][axis],
                bezt->vec[2][axis],
                bezt_next->vec[0][axis],
                bezt_next->vec[1][axis],
                static_cast<float *>(POINTER_OFFSET(point_offset, sizeof(float) * axis)),
                mmd->curve_resolution,
                stride);
          }
          point_offset = static_cast<GreasePencilFollowCurvePoint *>(
              POINTER_OFFSET(point_offset, stride * mmd->curve_resolution));
        }
      });

      /* Transform to world space. */
      for (int i = 0; i < follow_curve->points_len; i++) {
        GreasePencilFollowCurvePoint *point = &follow_curve->points[i];
        mul_m4_v3(ob_eval->object_to_world().ptr(), point->co);
      }

      /* Calculate the vectors from one point to the next.
       * And the (accumulative) length of these vectors. */
      float len_accumulative = 0.0f;
      for (int i = 0; i < follow_curve->points_len - 1; i++) {
        GreasePencilFollowCurvePoint *point = &follow_curve->points[i];
        GreasePencilFollowCurvePoint *point_next = &follow_curve->points[i + 1];
        sub_v3_v3v3(point->vec_to_next, point_next->co, point->co);
        point->vec_len = len_v3(point->vec_to_next);
        point->vec_len_accumulative = len_accumulative;
        len_accumulative += point->vec_len;
        normalize_v3(point->vec_to_next);

        if (i == follow_curve->points_len - 2) {
          copy_v3_v3(point_next->vec_to_next, point->vec_to_next);
          point_next->vec_len = 0;
          point_next->vec_len_accumulative = len_accumulative;
        }
      }

      follow_curve->length = len_accumulative;
    }
  }

  /* When projecting the entire GP object to the curve, create an object profile. */
  mmd->flag &= ~MOD_GREASE_PENCIL_FOLLOWCURVE_CURVE_TAIL_FIRST;
  if ((mmd->flag & MOD_GREASE_PENCIL_FOLLOWCURVE_ENTIRE_OBJECT) != 0) {
    std::optional<Bounds<float3>> maybe_bbox = grease_pencil.bounds_min_max_eval();
    if (!maybe_bbox.has_value()) {
      return;
    }
    Bounds<float3> bbox = maybe_bbox.value();

    /* Calculate profile using GP object bounding box. */
    zero_v3(mmd->profile_vec);

    switch (mmd->object_axis) {
      case MOD_GREASE_PENCIL_FOLLOWCURVE_AXIS_X: {
        mmd->profile_start[0] = bbox.min.x;
        mmd->profile_start[1] = bbox.min.y + (bbox.max.y - bbox.min.y) * mmd->object_center;
        mmd->profile_start[2] = bbox.min.z + (bbox.max.z - bbox.min.z) * mmd->object_center;
        mmd->profile_vec[0] = bbox.max.x - bbox.min.x;
        break;
      }

      case MOD_GREASE_PENCIL_FOLLOWCURVE_AXIS_Y: {
        mmd->profile_start[0] = bbox.min.x + (bbox.max.x - bbox.min.x) * mmd->object_center;
        mmd->profile_start[1] = bbox.min.y;
        mmd->profile_start[2] = bbox.min.z + (bbox.max.z - bbox.min.z) * mmd->object_center;
        mmd->profile_vec[1] = bbox.max.y - bbox.min.y;
        break;
      }

      case MOD_GREASE_PENCIL_FOLLOWCURVE_AXIS_Z: {
        mmd->profile_start[0] = bbox.min.x + (bbox.max.x - bbox.min.x) * mmd->object_center;
        mmd->profile_start[1] = bbox.min.y + (bbox.max.y - bbox.min.y) * mmd->object_center;
        mmd->profile_start[2] = bbox.min.z;
        mmd->profile_vec[2] = bbox.max.z - bbox.min.z;
        break;
      }
    }
    mul_m4_v3(ctx->object->object_to_world().ptr(), mmd->profile_start);
    float profile_length = len_v3(mmd->profile_vec);
    normalize_v3(mmd->profile_vec);

    if (mmd->curves_len > 0) {
      /* Set profile scale so that the GP object covers the curve over the full length. */
      mmd->profile_scale = (profile_length != 0.0f) ? mmd->curves[0].length / profile_length :
                                                      1.0f;

      /* Find nearest curve point to profile start: curve head or tail. */
      const float dist_head = fabsf(
          len_squared_v3v3(mmd->curves[0].points[0].co, mmd->profile_start));
      const float dist_tail = fabsf(len_squared_v3v3(
          mmd->curves[0].points[mmd->curves[0].points_len - 1].co, mmd->profile_start));
      if (dist_tail < dist_head) {
        mmd->flag |= MOD_GREASE_PENCIL_FOLLOWCURVE_CURVE_TAIL_FIRST;
      }
    }
  }
}

static void frame_clear(GreasePencilFollowCurveModifierData *mmd)
{
  /* Clear animated speed data. */
  MEM_SAFE_FREE(mmd->speed_per_frame);

  /* Clear curve data. */
  for (int i = 0; i < mmd->curves_len; i++) {
    GreasePencilFollowCurve *curve = &mmd->curves[i];
    MEM_SAFE_FREE(curve->points);
  }
  MEM_SAFE_FREE(mmd->curves);
  mmd->curves_len = 0;
}

static void get_random_float(const int seed, const int count, float *r_random_value)
{
  RNG *rng = BLI_rng_new(seed);
  for (int i = 0; i < count; i++) {
    r_random_value[i] = BLI_rng_get_float(rng);
  }
  BLI_rng_free(rng);
}

static void get_rotation_plane(const int axis, const float angle, float3 &r_rotation_plane)
{
  switch (axis) {
    case MOD_GREASE_PENCIL_FOLLOWCURVE_AXIS_X: {
      /* Plane XY. */
      r_rotation_plane[0] = cos(angle);
      r_rotation_plane[1] = sin(angle);
      break;
    }
    case MOD_GREASE_PENCIL_FOLLOWCURVE_AXIS_Y: {
      /* Plane YZ. */
      r_rotation_plane[1] = cos(angle);
      r_rotation_plane[2] = sin(angle);
      break;
    }
    case MOD_GREASE_PENCIL_FOLLOWCURVE_AXIS_Z: {
      /* Plane ZX. */
      r_rotation_plane[0] = sin(angle);
      r_rotation_plane[2] = cos(angle);
      break;
    }
  }
}

static void get_distance_of_point_to_line(const float3 &point,
                                          const float3 &line_start,
                                          const float3 &line_vec,
                                          const float3 &plane,
                                          float *r_dist_on_line,
                                          float *r_radius)
{
  /* Getting closest distance of a point to a line. See:
   * https://math.stackexchange.com/questions/1905533/find-perpendicular-distance-from-point-to-line-in-3d
   * https://en.wikipedia.org/wiki/Distance_from_a_point_to_a_line
   */
  float vec_to_sp[3], p_on_line[3], vec_t[3], vec_dir[3];

  /* Get vector from line start to point. */
  sub_v3_v3v3(vec_to_sp, point, line_start);

  /* Project point orthogonally on line. */
  float dist = dot_v3v3(vec_to_sp, line_vec);
  *r_dist_on_line = dist;

  /* Get point on line. */
  mul_v3_v3fl(vec_t, line_vec, dist);
  add_v3_v3v3(p_on_line, line_start, vec_t);

  /* Get the direction of the radius (on which side of the line). */
  sub_v3_v3v3(vec_dir, point, p_on_line);
  cross_v3_v3v3(vec_t, vec_dir, line_vec);
  float direction = (dot_v3v3(vec_t, plane) < 0.0f) ? -1.0f : 1.0f;

  /* Get the radius (= the shortest distance of the point to the line). */
  sub_v3_v3(p_on_line, point);
  *r_radius = len_v3(p_on_line) * direction;
}

static float stroke_get_length(Span<float3> positions,
                               const IndexRange point_range,
                               MutableSpan<float> r_segment_len)
{
  float length = 0.0f;
  for (const int i : point_range.drop_back(1)) {
    r_segment_len[i] = math::distance(positions[i], positions[i + 1]);
    length += r_segment_len[i];
  }
  return length;
}

static GreasePencilFollowCurve *object_stroke_get_current_curve_and_distance(
    const GreasePencilFollowCurveModifierData *mmd,
    Span<float3> positions,
    const IndexRange point_range,
    const float3 &side_plane,
    float *r_dist_on_curve,
    float *r_radius_initial,
    float *r_angle_initial,
    bool *r_start_at_tail)
{
  /* Get distance of stroke start to object profile. */
  float dist_on_profile;
  get_distance_of_point_to_line(positions[point_range.first()],
                                mmd->profile_start,
                                mmd->profile_vec,
                                side_plane,
                                &dist_on_profile,
                                r_radius_initial);
  *r_radius_initial = 0.0f;
  *r_dist_on_curve = 0.0f;

  /* Set initial spiral angle. */
  *r_angle_initial = mmd->angle;

  /* Start at tail of curve? */
  *r_start_at_tail = ((mmd->flag & MOD_GREASE_PENCIL_FOLLOWCURVE_CURVE_TAIL_FIRST) != 0);

  /* Objects can follow only one curve, so return the first. */
  return &mmd->curves[0];
}

static GreasePencilFollowCurve *stroke_get_current_curve_and_distance(const ModifierData *md,
                                                                      const Object *ob,
                                                                      Span<float3> positions,
                                                                      const IndexRange point_range,
                                                                      const int stroke_index,
                                                                      const float stroke_length,
                                                                      const float3 &side_plane,
                                                                      float *r_dist_on_curve,
                                                                      float *r_radius_initial,
                                                                      float *r_angle_initial,
                                                                      bool *r_start_at_tail)
{
  const GreasePencilFollowCurveModifierData *mmd =
      reinterpret_cast<const GreasePencilFollowCurveModifierData *>(md);

  /* Handle stroke projection when projecting the entire GP object on a curve. */
  if ((mmd->flag & MOD_GREASE_PENCIL_FOLLOWCURVE_ENTIRE_OBJECT) != 0) {
    return object_stroke_get_current_curve_and_distance(mmd,
                                                        positions,
                                                        point_range,
                                                        side_plane,
                                                        r_dist_on_curve,
                                                        r_radius_initial,
                                                        r_angle_initial,
                                                        r_start_at_tail);
  }

  /* Get random values for this stroke. */
  float random_val[3];
  int seed = mmd->seed;
  seed += BLI_hash_string(ob->id.name + 2);
  seed += BLI_hash_string(md->name);
  seed += stroke_index;
  get_random_float(seed, 3, random_val);

  const float speed_var_f = (random_val[0] - 0.5f) * 2.0f;
  float speed = mmd->speed + mmd->speed_variation * speed_var_f;
  if ((mmd->flag & MOD_GREASE_PENCIL_FOLLOWCURVE_VARY_DIR) && random_val[1] < 0.5f) {
    speed *= -1.0f;
  }
  *r_start_at_tail = (speed < 0.0f);
  *r_angle_initial = mmd->angle;

  /* Get stroke starting point. */
  const bool stroke_tail_first = ((mmd->flag & MOD_GREASE_PENCIL_FOLLOWCURVE_STROKE_TAIL_FIRST) !=
                                  0);
  float3 stroke_start = (stroke_tail_first) ? positions[point_range.last()] :
                                              positions[point_range.first()];

  /* Get the curve this stroke belongs to (= the nearest curve). */
  int curve_index = 0;
  if (mmd->curves_len > 1) {
    float dist_min = FLT_MAX;
    for (int i = 0; i < mmd->curves_len; i++) {
      const float dist = len_squared_v3v3(stroke_start, mmd->curves[i].points[0].co);
      if (dist < dist_min) {
        dist_min = dist;
        curve_index = i;
      }
    }
  }
  GreasePencilFollowCurve *curve = &mmd->curves[curve_index];

  /* Get initial distance from stroke to curve. */
  float dist_on_curve_initial;
  get_distance_of_point_to_line(stroke_start,
                                curve->points[0].co,
                                curve->points[0].vec_to_next,
                                side_plane,
                                &dist_on_curve_initial,
                                r_radius_initial);

  /* We always start at the beginning of a curve, so limit the distance to zero or less. */
  if (dist_on_curve_initial > 0.0f) {
    dist_on_curve_initial = 0.0f;
  }

  /* Take care of scatter when there is no animation. */
  if ((mmd->flag & MOD_GREASE_PENCIL_FOLLOWCURVE_SCATTER) && mmd->speed_per_frame_len == 0 &&
      fabsf(mmd->speed) < FLT_EPSILON && mmd->speed_variation < FLT_EPSILON)
  {
    /* Distribute stroke randomly over curve. */
    float delta = curve->length - stroke_length;
    *r_dist_on_curve = stroke_length + delta * random_val[1];

    return curve;
  }

  /* Scatter when animated: vary the starting point of the stroke. */
  if (mmd->flag & MOD_GREASE_PENCIL_FOLLOWCURVE_SCATTER) {
    dist_on_curve_initial -= curve->length * 0.5 * random_val[2];
  }

  /* Get the distance the stroke travelled so far, up to current keyframe. */
  float dist_travelled = 0.0f;
  if (mmd->speed_per_frame_len > 0) {
    /* Speed is animated, sum the speed of all the frames up to current (but not inclusive). */
    for (int frame = 0; frame < mmd->speed_per_frame_len - 1; frame++) {
      int stride = frame * 2;
      dist_travelled += mmd->speed_per_frame[stride] +
                        mmd->speed_per_frame[stride + 1] * speed_var_f;
    }
  }
  else {
    /* Fixed speed. */
    dist_travelled = (mmd->cfra - 1) * (mmd->speed + mmd->speed_variation * speed_var_f);
  }
  dist_travelled = fabsf(dist_travelled) + dist_on_curve_initial;

  /* When the animation is not repeated, we can finish here. */
  if ((mmd->flag & MOD_GREASE_PENCIL_FOLLOWCURVE_REPEAT) == 0) {
    *r_dist_on_curve = dist_travelled;

    return curve;
  }

  /* When the animation is repeated, we take the modulo to get the current distance
   * on the curve. */
  const float curve_gps_length = curve->length + stroke_length;
  if ((dist_travelled > curve_gps_length) && (fabsf(mmd->spirals) > FLT_EPSILON)) {
    /* When spiraling, pick a random start angle (for variation). */
    seed += ((int)(dist_travelled / curve_gps_length) * 1731);
    get_random_float(seed, 1, random_val);
    *r_angle_initial = mmd->angle + M_PI * 2 * random_val[0];
  }
  dist_travelled = fmodf(dist_travelled, curve_gps_length);

  *r_dist_on_curve = dist_travelled;
  return curve;
}

static GreasePencilFollowCurvePoint *curve_search_point_by_distance(
    const float dist,
    GreasePencilFollowCurvePoint *points,
    const int index_start,
    const int index_end,
    float *r_dist_remaining)
{
  /* Binary search: stop conditions. */
  if (index_start == index_end) {
    *r_dist_remaining = dist - points[index_start].vec_len_accumulative;
    return &points[index_start];
  }
  if (index_start == (index_end - 1)) {
    const float ds = dist - points[index_start].vec_len_accumulative;
    const float de = points[index_end].vec_len_accumulative - dist;
    if (ds < de) {
      *r_dist_remaining = ds;
      return &points[index_start];
    }
    *r_dist_remaining = de;
    return &points[index_end];
  }

  /* Binary search: split the search area by half. */
  const int index_half = (int)((index_start + index_end) * 0.5);
  if (points[index_half].vec_len_accumulative < dist) {
    return curve_search_point_by_distance(dist, points, index_half, index_end, r_dist_remaining);
  }
  else {
    return curve_search_point_by_distance(dist, points, index_start, index_half, r_dist_remaining);
  }
}

static void curve_get_point_by_distance(const float dist_init,
                                        GreasePencilFollowCurve *curve,
                                        float3 &r_point,
                                        float3 &r_point_vec)
{
  /* When outside curve boundaries, find the mirrored curve point. */
  float mirror_at[3];
  bool mirrored = false;
  float dist = dist_init;

  if (dist < 0.0f) {
    dist = math::min(-dist, curve->length);
    mirrored = true;
    copy_v3_v3(mirror_at, curve->points[0].co);
  }
  else if (dist > curve->length) {
    dist = math::max(2 * curve->length - dist, 0.0f);
    mirrored = true;
    copy_v3_v3(mirror_at, curve->points[curve->points_len - 1].co);
  }

  /* Find closest curve point by binary search. */
  float dist_remaining;
  GreasePencilFollowCurvePoint *curve_p = curve_search_point_by_distance(
      dist, curve->points, 0, curve->points_len - 1, &dist_remaining);
  copy_v3_v3(r_point_vec, curve_p->vec_to_next);

  /* Find exact point by interpolating the segment vector. */
  float delta[3];
  copy_v3_v3(r_point, curve_p->co);
  mul_v3_v3fl(delta, curve_p->vec_to_next, dist_remaining);
  add_v3_v3(r_point, delta);

  /* Mirror curve point. */
  if (mirrored) {
    sub_v3_v3v3(delta, mirror_at, r_point);
    add_v3_v3v3(r_point, mirror_at, delta);
  }
}

static void deform_drawing(ModifierData *md, const ModifierEvalContext *ctx, Drawing &drawing)
{
  auto *mmd = reinterpret_cast<GreasePencilFollowCurveModifierData *>(md);
  bke::CurvesGeometry &strokes = drawing.strokes_for_write();
  if (strokes.points_num() == 0) {
    return;
  }
  IndexMaskMemory memory;
  const IndexMask filtered_strokes = modifier::greasepencil::get_filtered_stroke_mask(
      ctx->object, strokes, mmd->influence, memory);
  if (filtered_strokes.is_empty()) {
    return;
  }

  /* Get 'entire object' settings. */
  const bool entire_object = ((mmd->flag & MOD_GREASE_PENCIL_FOLLOWCURVE_ENTIRE_OBJECT) != 0);

  /* Get plane for sprial radius direction (on which side of the curve is a stroke point.) */
  float side_plane[3] = {0.0f};
  switch (mmd->angle_axis) {
    case MOD_GREASE_PENCIL_FOLLOWCURVE_AXIS_X: {
      side_plane[0] = 1.0f;
      break;
    }
    case MOD_GREASE_PENCIL_FOLLOWCURVE_AXIS_Y: {
      side_plane[1] = 1.0f;
      break;
    }
    case MOD_GREASE_PENCIL_FOLLOWCURVE_AXIS_Z: {
      side_plane[2] = 1.0f;
      break;
    }
  }

  const OffsetIndices<int> points_by_stroke = strokes.points_by_curve();
  MutableSpan<float3> positions = strokes.positions_for_write();
  MutableSpan<float> opacities = drawing.opacities_for_write();

  filtered_strokes.foreach_index(GrainSize(8), [&](const int stroke_index) {
    const IndexRange points = points_by_stroke[stroke_index];
    Array<float> stroke_segment_lengths(points.size());
    const float stroke_length = stroke_get_length(positions, points, stroke_segment_lengths);

    /* Get current curve to project the stroke on. */
    float dist_on_curve, radius_initial, angle_initial;
    bool curve_start_at_tail;
    GreasePencilFollowCurve *curve = stroke_get_current_curve_and_distance(md,
                                                                           ctx->object,
                                                                           positions,
                                                                           points,
                                                                           stroke_index,
                                                                           stroke_length,
                                                                           side_plane,
                                                                           &dist_on_curve,
                                                                           &radius_initial,
                                                                           &angle_initial,
                                                                           &curve_start_at_tail);

    /* Get the direction of the stroke points. */
    const bool stroke_start_at_tail =
        ((mmd->flag & MOD_GREASE_PENCIL_FOLLOWCURVE_STROKE_TAIL_FIRST) != 0) &&
        ((mmd->flag & MOD_GREASE_PENCIL_FOLLOWCURVE_ENTIRE_OBJECT) == 0);
    const int stroke_dir = (stroke_start_at_tail) ? -1 : 1;
    const int stroke_start_index = (stroke_start_at_tail) ? points.last() : points.first();
    const int stroke_end_index = (stroke_start_at_tail) ? points.first() : points.last();
    float3 stroke_start = positions[stroke_start_index];
    float3 stroke_end = positions[stroke_end_index];

    /* Create profile: a line along which the stroke is projected on the curve. */
    float3 profile_start, profile_vector;
    copy_v3_v3(profile_start, mmd->profile_start);
    copy_v3_v3(profile_vector, mmd->profile_vec);
    if (!entire_object) {
      /* Create stroke profile. For now this is just a straight line between the
       * first and last point of the stroke.
       *
       * Stroke   __/\  _/\  /\____
       *              \/   \/
       *
       * Profile  _________________
       *
       */
      sub_v3_v3v3(profile_vector, stroke_end, stroke_start);
      normalize_v3(profile_vector);
      copy_v3_v3(profile_start, stroke_start);
    }

    /* Get rotation plane for spiral angle. */
    float3 rotation_plane = {0.0f, 0.0f, 0.0f};
    get_rotation_plane(mmd->angle_axis, angle_initial, rotation_plane);

    /* Get spiral setting. */
    const bool use_spiral = (math::abs(mmd->spirals) > FLT_EPSILON);

    /* Loop all stroke points and project them on the curve. */
    for (int point_i = stroke_start_index; point_i >= 0 && point_i <= points.last();
         point_i += stroke_dir)
    {
      /* Get distance and radius of point to profile. */
      float stroke_p_dist, stroke_p_radius;
      get_distance_of_point_to_line(positions[point_i],
                                    profile_start,
                                    profile_vector,
                                    side_plane,
                                    &stroke_p_dist,
                                    &stroke_p_radius);

      /* Find closest point on curve given a distance. */
      float curve_dist = (entire_object) ? (stroke_p_dist * mmd->profile_scale +
                                            (mmd->completion - 1.0f) * curve->length) :
                                           (dist_on_curve - stroke_p_dist);
      if (curve_start_at_tail) {
        curve_dist = curve->length - curve_dist;
      }
      float3 curve_p, curve_p_vec;
      curve_get_point_by_distance(curve_dist, curve, curve_p, curve_p_vec);

      /* Project stroke point on curve segment by finding the orthogonal vector
       * in the plane of the spiral angle. */
      float3 p_rotated;
      if (use_spiral) {
        const float angle = angle_initial + mmd->spirals * M_PI * 2 * (curve_dist / curve->length);
        get_rotation_plane(mmd->angle_axis, angle, rotation_plane);
      }
      cross_v3_v3v3(p_rotated, curve_p_vec, rotation_plane);

      /* Apply radius. */
      const float radius = radius_initial + stroke_p_radius;
      mul_v3_fl(p_rotated, radius);

      /* Add curve point. */
      add_v3_v3(p_rotated, curve_p);

      /* Set new coordinates of stroke point. */
      positions[point_i] = p_rotated;

      /* Dissolve when outside the curve. */
      if ((mmd->flag & MOD_GREASE_PENCIL_FOLLOWCURVE_DISSOLVE) &&
          (curve_dist < 0.0f || curve_dist > curve->length))
      {
        opacities[point_i] = 0.0f;
      }
    }
  });

  drawing.tag_positions_changed();
}

static void modify_geometry_set(ModifierData *md,
                                const ModifierEvalContext *ctx,
                                bke::GeometrySet *geometry_set)
{
  auto *mmd = reinterpret_cast<GreasePencilFollowCurveModifierData *>(md);

  if (!geometry_set->has_grease_pencil()) {
    return;
  }
  GreasePencil &grease_pencil = *geometry_set->get_grease_pencil_for_write();
  const int frame = grease_pencil.runtime->eval_frame;
  mmd->cfra = frame;

  /* Init curve data for this frame. */
  frame_init(mmd, ctx, grease_pencil);
  if (mmd->curves_len == 0) {
    return;
  }

  IndexMaskMemory mask_memory;
  const IndexMask layer_mask = modifier::greasepencil::get_filtered_layer_mask(
      grease_pencil, mmd->influence, mask_memory);

  const Vector<Drawing *> drawings = modifier::greasepencil::get_drawings_for_write(
      grease_pencil, layer_mask, frame);
  threading::parallel_for(drawings.index_range(), 1, [&](const IndexRange range) {
    for (const int drawing_i : range) {
      deform_drawing(md, ctx, *drawings[drawing_i]);
    }
  });

  frame_clear(mmd);
}

static void panel_draw(const bContext *C, Panel *panel)
{
  uiLayout *col, *row;
  uiLayout *layout = panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  bool entire_object = RNA_boolean_get(ptr, "entire_object");

  uiLayoutSetPropSep(layout, true);

  uiItemR(layout, ptr, "object", UI_ITEM_NONE, nullptr, ICON_NONE);

  col = uiLayoutColumn(layout, false);
  uiItemR(col, ptr, "curve_resolution", UI_ITEM_NONE, nullptr, ICON_NONE);

  uiItemS(layout);
  col = uiLayoutColumn(layout, false);
  uiItemR(col, ptr, "entire_object", UI_ITEM_NONE, nullptr, ICON_NONE);
  if (entire_object) {
    row = uiLayoutRow(col, false);
    uiItemR(row, ptr, "object_axis", UI_ITEM_R_EXPAND, nullptr, ICON_NONE);
    uiItemR(col, ptr, "object_center", UI_ITEM_R_SLIDER, nullptr, ICON_NONE);
    uiItemR(col, ptr, "completion", UI_ITEM_R_SLIDER, nullptr, ICON_NONE);
    uiItemS(layout);
  }

  col = uiLayoutColumn(layout, false);
  uiItemR(col, ptr, "angle", UI_ITEM_NONE, nullptr, ICON_NONE);
  uiItemR(col, ptr, "spirals", UI_ITEM_NONE, nullptr, ICON_NONE);
  row = uiLayoutRow(col, false);
  uiItemR(row, ptr, "axis", UI_ITEM_R_EXPAND, nullptr, ICON_NONE);

  if (!entire_object) {
    uiItemS(layout);
    col = uiLayoutColumn(layout, false);
    uiItemR(col, ptr, "speed", UI_ITEM_R_SLIDER, nullptr, ICON_NONE);
    uiItemR(col, ptr, "speed_variation", UI_ITEM_R_SLIDER, nullptr, ICON_NONE);
    uiItemR(col, ptr, "seed", UI_ITEM_NONE, nullptr, ICON_NONE);
  }

  col = uiLayoutColumn(layout, true);
  if (!entire_object) {
    uiItemR(col, ptr, "vary_dir", UI_ITEM_NONE, nullptr, ICON_NONE);
    uiItemS(layout);
    col = uiLayoutColumn(layout, false);
    uiItemR(col, ptr, "tail_first", UI_ITEM_NONE, nullptr, ICON_NONE);
    uiItemR(col, ptr, "repeat", UI_ITEM_NONE, nullptr, ICON_NONE);
    uiItemR(col, ptr, "scatter", UI_ITEM_NONE, nullptr, ICON_NONE);
  }
  uiItemR(col, ptr, "dissolve", UI_ITEM_NONE, nullptr, ICON_NONE);

  if (uiLayout *influence_panel = uiLayoutPanelProp(
          C, layout, ptr, "open_influence_panel", "Influence"))
  {
    modifier::greasepencil::draw_layer_filter_settings(C, influence_panel, ptr);
    modifier::greasepencil::draw_material_filter_settings(C, influence_panel, ptr);
  }

  modifier_panel_end(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_GreasePencilFollowCurve, panel_draw);
}

static void blend_write(BlendWriter *writer, const ID * /*id_owner*/, const ModifierData *md)
{
  const auto *mmd = reinterpret_cast<const GreasePencilFollowCurveModifierData *>(md);

  BLO_write_struct(writer, GreasePencilFollowCurveModifierData, mmd);
  modifier::greasepencil::write_influence_data(writer, &mmd->influence);
}

static void blend_read(BlendDataReader *reader, ModifierData *md)
{
  auto *mmd = reinterpret_cast<GreasePencilFollowCurveModifierData *>(md);

  modifier::greasepencil::read_influence_data(reader, &mmd->influence);
}

}  // namespace blender

ModifierTypeInfo modifierType_GreasePencilFollowCurve = {
    /*idname*/ "GreasePencilFollowCurve",
    /*name*/ N_("Follow Curve"),
    /*struct_name*/ "GreasePencilFollowCurveModifierData",
    /*struct_size*/ sizeof(GreasePencilFollowCurveModifierData),
    /*srna*/ &RNA_GreasePencilFollowCurveModifier,
    /*type*/ ModifierTypeType::OnlyDeform,
    /*flags*/ eModifierTypeFlag_AcceptsGreasePencil | eModifierTypeFlag_SupportsEditmode |
        eModifierTypeFlag_EnableInEditmode | eModifierTypeFlag_SupportsMapping,
    /*icon*/ ICON_FORCE_CURVE,

    /*copy_data*/ blender::copy_data,

    /*deform_verts*/ nullptr,
    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ nullptr,
    /*modify_geometry_set*/ blender::modify_geometry_set,

    /*init_data*/ blender::init_data,
    /*required_data_mask*/ nullptr,
    /*free_data*/ blender::free_data,
    /*is_disabled*/ blender::is_disabled,
    /*update_depsgraph*/ blender::update_depsgraph,
    /*depends_on_time*/ nullptr,
    /*depends_on_normals*/ nullptr,
    /*foreach_ID_link*/ blender::foreach_ID_link,
    /*foreach_tex_link*/ nullptr,
    /*free_runtime_data*/ nullptr,
    /*panel_register*/ blender::panel_register,
    /*blend_write*/ blender::blend_write,
    /*blend_read*/ blender::blend_read,
    /*foreach_cache*/ nullptr,
};
