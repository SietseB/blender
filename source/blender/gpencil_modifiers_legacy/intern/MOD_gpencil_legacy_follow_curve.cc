/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2017 Blender Foundation. */

/** \file
 * \ingroup modifiers
 */

#include <stdio.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "BLI_hash.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_rand.h"
#include "BLI_string.h"

#include "BLT_translation.hh"

#include "DNA_anim_types.h"
#include "DNA_collection_types.h"
#include "DNA_defaults.h"
#include "DNA_gpencil_legacy_types.h"
#include "DNA_gpencil_modifier_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"

#include "BKE_context.hh"
#include "BKE_curve.hh"
#include "BKE_curves.h"
#include "BKE_deform.hH"
#include "BKE_fcurve.hh"
#include "BKE_gpencil_geom_legacy.h"
#include "BKE_gpencil_modifier_legacy.h"
#include "BKE_lib_query.hh"
#include "BKE_main.hh"
#include "BKE_modifier.hh"
#include "BKE_scene.hH"
#include "BKE_screen.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "MOD_gpencil_legacy_modifiertypes.h"
#include "MOD_gpencil_legacy_ui_common.h"
#include "MOD_gpencil_legacy_util.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

static void init_data(GpencilModifierData *md)
{
  FollowCurveGpencilModifierData *mmd = (FollowCurveGpencilModifierData *)md;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(mmd, modifier));

  MEMCPY_STRUCT_AFTER(mmd, DNA_struct_default_get(FollowCurveGpencilModifierData), modifier);
}

static void copy_data(const GpencilModifierData *md, GpencilModifierData *target)
{
  BKE_gpencil_modifier_copydata_generic(md, target);
}

void MOD_gpencil_follow_curve_frame_init(Depsgraph *depsgraph,
                                         const GpencilModifierData *md,
                                         Scene *scene,
                                         Object *ob)
{
  FollowCurveGpencilModifierData *mmd = (FollowCurveGpencilModifierData *)md;

  /* Get frame range up to current frame. */
  int frame_current = scene->r.cfra;
  mmd->cfra = frame_current;

  /* Get animated speed and speed variation. */
  mmd->speed_per_frame_len = 0;
  FCurve *speed_fcurve = id_data_find_fcurve(
      &ob->id, mmd, &RNA_FollowCurveGpencilModifier, "speed", 0, nullptr);
  FCurve *speed_var_fcurve = id_data_find_fcurve(
      &ob->id, mmd, &RNA_FollowCurveGpencilModifier, "speed_var", 0, nullptr);
  if (speed_fcurve != nullptr || speed_var_fcurve != nullptr) {
    mmd->speed_per_frame_len = frame_current;
  }

  /* When animated, create array with speed and speed variation per frame. */
  mmd->speed_per_frame = nullptr;
  if (mmd->speed_per_frame_len > 0) {
    /* One stride contains: speed, speed variation. */
    mmd->speed_per_frame = static_cast<float *>(
        MEM_malloc_arrayN(frame_current * 2, sizeof(float), __func__));
    for (int frame = 1; frame <= frame_current; frame++) {
      float speed = (speed_fcurve) ? evaluate_fcurve(speed_fcurve, (float)frame) : mmd->speed;
      float speed_var = (speed_var_fcurve) ? evaluate_fcurve(speed_var_fcurve, (float)frame) :
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
    Curve *curve = (Curve *)mmd->object->data;
    LISTBASE_FOREACH (Nurb *, nurb, &curve->nurb) {
      if (nurb->type == CU_BEZIER) {
        mmd->curves_len++;
      }
    }
  }

  /* Convert Bezier curves to points. */
  Object *ob_eval = (Object *)DEG_get_evaluated_object(depsgraph, mmd->object);
  mmd->curves = nullptr;
  if (mmd->curves_len > 0) {
    mmd->curves = static_cast<GPFollowCurve *>(
        MEM_calloc_arrayN(mmd->curves_len, sizeof(GPFollowCurve), __func__));

    Curve *curve = (Curve *)ob_eval->data;
    int curve_index = -1;

    /* Loop splines. */
    LISTBASE_FOREACH (Nurb *, nurb, &curve->nurb) {
      if (nurb->type != CU_BEZIER) {
        continue;
      }
      curve_index++;
      GPFollowCurve *follow_curve = &mmd->curves[curve_index];

      /* Count points in spline segments. */
      int segments = nurb->pntsu;
      if ((nurb->flagu & CU_NURB_CYCLIC) == 0) {
        segments--;
      }
      follow_curve->points_len = segments * mmd->curve_resolution;
      follow_curve->curve = curve;

      /* Create array for curve point data. */
      const int stride = sizeof(GPFollowCurvePoint);
      follow_curve->points = static_cast<GPFollowCurvePoint *>(
          MEM_mallocN((follow_curve->points_len + 1) * stride, __func__));
      GPFollowCurvePoint *point_offset = follow_curve->points;

      /* Convert spline segments of Bezier curve to points. */
      for (int i = 0; i < segments; i++) {
        int i_next = (i + 1) % nurb->pntsu;
        BezTriple *bezt = &nurb->bezt[i];
        BezTriple *bezt_next = &nurb->bezt[i_next];
        for (int axis = 0; axis < 3; axis++) {
          BKE_curve_forward_diff_bezier(
              bezt->vec[1][axis],
              bezt->vec[2][axis],
              bezt_next->vec[0][axis],
              bezt_next->vec[1][axis],
              (float *)POINTER_OFFSET(point_offset, sizeof(float) * axis),
              mmd->curve_resolution,
              stride);
        }
        point_offset = (GPFollowCurvePoint *)POINTER_OFFSET(point_offset,
                                                            stride * mmd->curve_resolution);
      }

      /* Transform to world space. */
      for (int i = 0; i < follow_curve->points_len; i++) {
        GPFollowCurvePoint *point = &follow_curve->points[i];
        mul_m4_v3(ob_eval->object_to_world().ptr(), point->co);
      }

      /* Calculate the vectors from one point to the next.
       * And the (accumulative) length of these vectors.
       */
      float len_accumulative = 0;
      for (int i = 0; i < follow_curve->points_len - 1; i++) {
        GPFollowCurvePoint *point = &follow_curve->points[i];
        GPFollowCurvePoint *point_next = &follow_curve->points[i + 1];
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
  mmd->flag &= ~GP_FOLLOWCURVE_CURVE_TAIL_FIRST;
  if ((mmd->flag & GP_FOLLOWCURVE_ENTIRE_OBJECT) != 0) {
    /* Object bounding box is unfortunately unreliable, so we collect the min and max coordinates
     * ourselves. */
    float x_min = FLT_MAX, y_min = FLT_MAX, z_min = FLT_MAX;
    float x_max = -FLT_MAX, y_max = -FLT_MAX, z_max = -FLT_MAX;

    bGPdata *gpd = (bGPdata *)ob->data;
    LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
      bGPDframe *gpf = BKE_gpencil_frame_retime_get(depsgraph, scene, ob, gpl);
      if (gpf == nullptr) {
        continue;
      }

      LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
        x_min = min_ff(x_min, gps->boundbox_min[0]);
        y_min = min_ff(y_min, gps->boundbox_min[1]);
        z_min = min_ff(z_min, gps->boundbox_min[2]);
        x_max = max_ff(x_max, gps->boundbox_max[0]);
        y_max = max_ff(y_max, gps->boundbox_max[1]);
        z_max = max_ff(z_max, gps->boundbox_max[2]);
      }
    }

    /* Calculate profile using GP object bounding box. */
    zero_v3(mmd->profile_vec);

    switch (mmd->object_axis) {
      case GP_FOLLOWCURVE_AXIS_X: {
        mmd->profile_start[0] = x_min;
        mmd->profile_start[1] = y_min + (y_max - y_min) * mmd->object_center;
        mmd->profile_start[2] = z_min + (z_max - z_min) * mmd->object_center;
        mmd->profile_vec[0] = x_max - x_min;
        break;
      }

      case GP_FOLLOWCURVE_AXIS_Y: {
        mmd->profile_start[0] = x_min + (x_max - x_min) * mmd->object_center;
        mmd->profile_start[1] = y_min;
        mmd->profile_start[2] = z_min + (z_max - z_min) * mmd->object_center;
        mmd->profile_vec[1] = y_max - y_min;
        break;
      }

      case GP_FOLLOWCURVE_AXIS_Z: {
        mmd->profile_start[0] = x_min + (x_max - x_min) * mmd->object_center;
        mmd->profile_start[1] = y_min + (y_max - y_min) * mmd->object_center;
        mmd->profile_start[2] = z_min;
        mmd->profile_vec[2] = z_max - z_min;
        break;
      }
    }
    mul_m4_v3(ob->object_to_world().ptr(), mmd->profile_start);
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
        mmd->flag |= GP_FOLLOWCURVE_CURVE_TAIL_FIRST;
      }
    }
  }
}

void MOD_gpencil_follow_curve_frame_clear(const GpencilModifierData *md)
{
  FollowCurveGpencilModifierData *mmd = (FollowCurveGpencilModifierData *)md;

  /* Clear animated speed data. */
  MEM_SAFE_FREE(mmd->speed_per_frame);

  /* Clear curve data. */
  for (int i = 0; i < mmd->curves_len; i++) {
    GPFollowCurve *curve = &mmd->curves[i];
    MEM_SAFE_FREE(curve->points);
  }
  MEM_SAFE_FREE(mmd->curves);
  mmd->curves_len = 0;
}

#undef CURVE_RESOLUTION

static void get_random_float(const int seed, const int count, float *random_value)
{
  RNG *rng = BLI_rng_new(seed);
  for (int i = 0; i < count; i++) {
    random_value[i] = BLI_rng_get_float(rng);
  }
  BLI_rng_free(rng);
}

static void get_rotation_plane(const int axis, const float angle, float *rotation_plane)
{
  switch (axis) {
    case GP_FOLLOWCURVE_AXIS_X: {
      /* Plane XY. */
      rotation_plane[0] = cos(angle);
      rotation_plane[1] = sin(angle);
      break;
    }
    case GP_FOLLOWCURVE_AXIS_Y: {
      /* Plane YZ. */
      rotation_plane[1] = cos(angle);
      rotation_plane[2] = sin(angle);
      break;
    }
    case GP_FOLLOWCURVE_AXIS_Z: {
      /* Plane ZX. */
      rotation_plane[0] = sin(angle);
      rotation_plane[2] = cos(angle);
      break;
    }
  }
}

static void get_distance_of_point_to_line(const float *point,
                                          const float *line_start,
                                          const float *line_vec,
                                          const float *plane,
                                          float *dist_on_line,
                                          float *radius)
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
  *dist_on_line = dist;

  /* Get point on line. */
  mul_v3_v3fl(vec_t, line_vec, dist);
  add_v3_v3v3(p_on_line, line_start, vec_t);

  /* Get the direction of the radius (on which side of the line). */
  sub_v3_v3v3(vec_dir, point, p_on_line);
  cross_v3_v3v3(vec_t, vec_dir, line_vec);
  float direction = (dot_v3v3(vec_t, plane) < 0.0f) ? -1.0f : 1.0f;

  /* Get the radius (= the shortest distance of the point to the line). */
  sub_v3_v3(p_on_line, point);
  *radius = len_v3(p_on_line) * direction;
}

static float stroke_get_length(const bGPDstroke *gps, float *segment_len)
{
  float length = 0;
  for (int i = 0; i < gps->totpoints - 1; i++) {
    bGPDspoint *pt = &gps->points[i];
    bGPDspoint *pt_next = &gps->points[i + 1];
    segment_len[i] = len_v3v3(&pt_next->x, &pt->x);
    length += segment_len[i];
  }
  return length;
}

static GPFollowCurve *object_stroke_get_current_curve_and_distance(
    const FollowCurveGpencilModifierData *mmd,
    const bGPDstroke *gps,
    const float *side_plane,
    float *dist_on_curve,
    float *radius_initial,
    float *angle_initial,
    bool *start_at_tail)
{
  /* Get distance of stroke start to object profile. */
  float dist_on_profile;
  get_distance_of_point_to_line(&gps->points[0].x,
                                mmd->profile_start,
                                mmd->profile_vec,
                                side_plane,
                                &dist_on_profile,
                                radius_initial);
  *radius_initial = 0.0f;
  *dist_on_curve = 0.0f;

  /* Set initial spiral angle. */
  *angle_initial = mmd->angle;

  /* Start at tail of curve? */
  *start_at_tail = ((mmd->flag & GP_FOLLOWCURVE_CURVE_TAIL_FIRST) != 0);

  /* Objects can follow only one curve, so return the first. */
  return &mmd->curves[0];
}

static GPFollowCurve *stroke_get_current_curve_and_distance(const GpencilModifierData *md,
                                                            const Object *ob,
                                                            const bGPDframe *gpf,
                                                            const bGPDstroke *gps,
                                                            const float gps_length,
                                                            const float *side_plane,
                                                            float *dist_on_curve,
                                                            float *radius_initial,
                                                            float *angle_initial,
                                                            bool *start_at_tail)
{
  FollowCurveGpencilModifierData *mmd = (FollowCurveGpencilModifierData *)md;

  /* Handle stroke projection when projecting the entire GP object on a curve. */
  if ((mmd->flag & GP_FOLLOWCURVE_ENTIRE_OBJECT) != 0) {
    return object_stroke_get_current_curve_and_distance(
        mmd, gps, side_plane, dist_on_curve, radius_initial, angle_initial, start_at_tail);
  }

  /* Get random values for this stroke. */
  float random_val[3];
  int seed = mmd->seed;
  seed += BLI_hash_string(ob->id.name + 2);
  seed += BLI_hash_string(md->name);
  seed += BLI_findindex(&gpf->strokes, gps);
  get_random_float(seed, 3, random_val);

  const float speed_var_f = (random_val[0] - 0.5f) * 2.0f;
  float speed = mmd->speed + mmd->speed_variation * speed_var_f;
  if ((mmd->flag & GP_FOLLOWCURVE_VARY_DIR) && random_val[1] < 0.5f) {
    speed *= -1.0f;
  }
  *start_at_tail = (speed < 0.0f);
  *angle_initial = mmd->angle;

  /* Get stroke starting point. */
  const bool stroke_tail_first = ((mmd->flag & GP_FOLLOWCURVE_STROKE_TAIL_FIRST) != 0);
  float *stroke_start = (stroke_tail_first) ? (&gps->points[gps->totpoints - 1].x) :
                                              (&gps->points[0].x);

  /* Get the curve this stroke belongs to (= the nearest stroke). */
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
  GPFollowCurve *curve = &mmd->curves[curve_index];

  /* Get initial distance from stroke to curve. */
  float dist_on_curve_initial;
  get_distance_of_point_to_line(stroke_start,
                                curve->points[0].co,
                                curve->points[0].vec_to_next,
                                side_plane,
                                &dist_on_curve_initial,
                                radius_initial);

  /* We always start at the beginning of a curve, so limit the distance to zero or less. */
  if (dist_on_curve_initial > 0.0f) {
    dist_on_curve_initial = 0.0f;
  }

  /* Take care of scatter when there is no animation. */
  if ((mmd->flag & GP_FOLLOWCURVE_SCATTER) && mmd->speed_per_frame_len == 0 &&
      fabsf(mmd->speed) < FLT_EPSILON && mmd->speed_variation < FLT_EPSILON)
  {
    /* Distribute stroke randomly over curve. */
    float delta = curve->length - gps_length;
    *dist_on_curve = gps_length + delta * random_val[1];

    return curve;
  }

  /* Scatter when animated: vary the starting point of the stroke. */
  if (mmd->flag & GP_FOLLOWCURVE_SCATTER) {
    dist_on_curve_initial -= curve->length * 0.5 * random_val[2];
  }

  /* Get the distance the stroke travelled so far, up to current keyframe. */
  float dist_travelled = 0;
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
  if ((mmd->flag & GP_FOLLOWCURVE_REPEAT) == 0) {
    *dist_on_curve = dist_travelled;

    return curve;
  }

  /* When the animation is repeated, we take the modulo to get the current distance
   * on the curve.
   */
  const float curve_gps_length = curve->length + gps_length;
  if ((dist_travelled > curve_gps_length) && (fabsf(mmd->spirals) > FLT_EPSILON)) {
    /* When spiraling, pick a random start angle (for variation). */
    seed += ((int)(dist_travelled / curve_gps_length) * 1731);
    get_random_float(seed, 1, random_val);
    *angle_initial = mmd->angle + M_PI * 2 * random_val[0];
  }
  dist_travelled = fmodf(dist_travelled, curve_gps_length);

  *dist_on_curve = dist_travelled;
  return curve;
}

static GPFollowCurvePoint *curve_search_point_by_distance(const float dist,
                                                          GPFollowCurvePoint *points,
                                                          const int index_start,
                                                          const int index_end,
                                                          float *dist_remaining)
{
  /* Binary search: stop conditions. */
  if (index_start == index_end) {
    *dist_remaining = dist - points[index_start].vec_len_accumulative;
    return &points[index_start];
  }
  if (index_start == (index_end - 1)) {
    const float ds = dist - points[index_start].vec_len_accumulative;
    const float de = points[index_end].vec_len_accumulative - dist;
    if (ds < de) {
      *dist_remaining = ds;
      return &points[index_start];
    }
    *dist_remaining = de;
    return &points[index_end];
  }

  /* Binary search: split the search area by half. */
  const int index_half = (int)((index_start + index_end) * 0.5);
  if (points[index_half].vec_len_accumulative < dist) {
    return curve_search_point_by_distance(dist, points, index_half, index_end, dist_remaining);
  }
  else {
    return curve_search_point_by_distance(dist, points, index_start, index_half, dist_remaining);
  }
}

static void curve_get_point_by_distance(const float dist_init,
                                        GPFollowCurve *curve,
                                        float *point,
                                        float *point_vec)
{
  /* When outside curve boundaries, find the mirrored curve point. */
  float mirror_at[3];
  bool mirrored = false;
  float dist = dist_init;

  if (dist < 0.0f) {
    dist = std::min(-dist, curve->length);
    mirrored = true;
    copy_v3_v3(mirror_at, curve->points[0].co);
  }
  else if (dist > curve->length) {
    dist = std::max(2 * curve->length - dist, 0.0f);
    mirrored = true;
    copy_v3_v3(mirror_at, curve->points[curve->points_len - 1].co);
  }

  /* Find closest curve point by binary search. */
  float dist_remaining;
  GPFollowCurvePoint *curve_p = curve_search_point_by_distance(
      dist, curve->points, 0, curve->points_len - 1, &dist_remaining);
  copy_v3_v3(point_vec, curve_p->vec_to_next);

  /* Find exact point by interpolating the segment vector. */
  float delta[3];
  copy_v3_v3(point, curve_p->co);
  mul_v3_v3fl(delta, curve_p->vec_to_next, dist_remaining);
  add_v3_v3(point, delta);

  /* Mirror curve point. */
  if (mirrored) {
    sub_v3_v3v3(delta, mirror_at, point);
    add_v3_v3v3(point, mirror_at, delta);
  }
}

/* deform stroke */
static void deform_stroke(GpencilModifierData *md,
                          Depsgraph * /*depsgraph*/,
                          Object *ob,
                          bGPDlayer *gpl,
                          bGPDframe *gpf,
                          bGPDstroke *gps)
{
  FollowCurveGpencilModifierData *mmd = (FollowCurveGpencilModifierData *)md;
  if (mmd->curves_len == 0) {
    return;
  }

  if (!is_stroke_affected_by_modifier(ob,
                                      mmd->layername,
                                      mmd->material,
                                      mmd->pass_index,
                                      mmd->layer_pass,
                                      2,
                                      gpl,
                                      gps,
                                      mmd->flag & GP_HOOK_INVERT_LAYER,
                                      mmd->flag & GP_HOOK_INVERT_PASS,
                                      mmd->flag & GP_HOOK_INVERT_LAYERPASS,
                                      mmd->flag & GP_HOOK_INVERT_MATERIAL))
  {
    return;
  }

  /* Get length of stroke and segments. */
  float *gps_segment_length = static_cast<float *>(
      MEM_malloc_arrayN(gps->totpoints, sizeof(float), __func__));
  float gps_length = stroke_get_length(gps, gps_segment_length);

  /* Get 'entire object' settings. */
  const bool entire_object = ((mmd->flag & GP_FOLLOWCURVE_ENTIRE_OBJECT) != 0);

  /* Get plane for sprial radius direction (on which side of the curve is a stroke point.) */
  float side_plane[3] = {0.0f};
  switch (mmd->angle_axis) {
    case GP_FOLLOWCURVE_AXIS_X: {
      side_plane[0] = 1.0f;
      break;
    }
    case GP_FOLLOWCURVE_AXIS_Y: {
      side_plane[1] = 1.0f;
      break;
    }
    case GP_FOLLOWCURVE_AXIS_Z: {
      side_plane[2] = 1.0f;
      break;
    }
  }

  /* Get current curve to project the stroke on. */
  float dist_on_curve, radius_initial, angle_initial;
  bool curve_start_at_tail;
  GPFollowCurve *curve = stroke_get_current_curve_and_distance(md,
                                                               ob,
                                                               gpf,
                                                               gps,
                                                               gps_length,
                                                               side_plane,
                                                               &dist_on_curve,
                                                               &radius_initial,
                                                               &angle_initial,
                                                               &curve_start_at_tail);

  /* Get the direction of the stroke points. */
  const bool gps_start_at_tail = ((mmd->flag & GP_FOLLOWCURVE_STROKE_TAIL_FIRST) != 0) &&
                                 ((mmd->flag & GP_FOLLOWCURVE_ENTIRE_OBJECT) == 0);
  const int gps_dir = (gps_start_at_tail) ? -1 : 1;
  const int gps_start_index = (gps_start_at_tail) ? (gps->totpoints - 1) : 0;
  const int gps_end_index = (gps_start_at_tail) ? 0 : (gps->totpoints - 1);
  float gps_start[3], gps_end[3];
  copy_v3_v3(gps_start, &gps->points[gps_start_index].x);
  copy_v3_v3(gps_end, &gps->points[gps_end_index].x);

  /* Create profile: a line along which the stroke is projected on the curve. */
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
    sub_v3_v3v3(mmd->profile_vec, gps_end, gps_start);
    normalize_v3(mmd->profile_vec);
  }

  /* Get rotation plane for spiral angle. */
  float rotation_plane[3] = {0.0f};
  get_rotation_plane(mmd->angle_axis, angle_initial, rotation_plane);

  /* Get spiral setting. */
  const bool use_spiral = (fabsf(mmd->spirals) > FLT_EPSILON);

  /* Loop all stroke points and project them on the curve. */
  for (int i = gps_start_index; i >= 0 && i < gps->totpoints; i += gps_dir) {
    /* For notational convenience, copy stroke point. */
    float gps_p[3];
    copy_v3_v3(gps_p, &gps->points[i].x);

    /* Get distance and radius of point to profile. */
    float gps_p_dist, gps_p_radius;
    get_distance_of_point_to_line(
        gps_p, mmd->profile_start, mmd->profile_vec, side_plane, &gps_p_dist, &gps_p_radius);

    /* Find closest point on curve given a distance. */
    float curve_p[3], curve_p_vec[3];
    float curve_dist = (entire_object) ? (gps_p_dist * mmd->profile_scale +
                                          (mmd->completion - 1.0f) * curve->length) :
                                         (dist_on_curve - gps_p_dist);
    if (curve_start_at_tail) {
      curve_dist = curve->length - curve_dist;
    }
    curve_get_point_by_distance(curve_dist, curve, curve_p, curve_p_vec);

    /* Project stroke point on curve segment by finding the orthogonal vector
     * in the plane of the spiral angle.
     */
    float p_rotated[3];
    if (use_spiral) {
      const float angle = angle_initial + mmd->spirals * M_PI * 2 * (curve_dist / curve->length);
      get_rotation_plane(mmd->angle_axis, angle, rotation_plane);
    }
    cross_v3_v3v3(p_rotated, curve_p_vec, rotation_plane);

    /* Apply radius. */
    const float radius = radius_initial + gps_p_radius;
    mul_v3_fl(p_rotated, radius);

    /* Add curve point. */
    add_v3_v3(p_rotated, curve_p);

    /* Set new coordinates of stroke point. */
    copy_v3_v3(&gps->points[i].x, p_rotated);

    /* Dissolve when outside the curve. */
    if ((mmd->flag & GP_FOLLOWCURVE_DISSOLVE) && (curve_dist < 0.0f || curve_dist > curve->length))
    {
      gps->points[i].strength = 0.0f;
    }
  }

  /* Mark stroke for geometry update. */
  gps->runtime.flag |= GP_STROKE_UPDATE_GEOMETRY;

  /* Cleanup. */
  MEM_freeN(gps_segment_length);
}

/* FIXME: Ideally we be doing this on a copy of the main depsgraph
 * (i.e. one where we don't have to worry about restoring state)
 */
static void bake_modifier(Main * /*bmain*/,
                          Depsgraph *depsgraph,
                          GpencilModifierData *md,
                          Object *ob)
{
  FollowCurveGpencilModifierData *mmd = (FollowCurveGpencilModifierData *)md;

  if (mmd->object == nullptr || mmd->curves_len == 0) {
    return;
  }

  generic_bake_deform_stroke(depsgraph, md, ob, true, deform_stroke);
}

static bool is_disabled(GpencilModifierData *md, bool /*use_render_params*/)
{
  FollowCurveGpencilModifierData *mmd = (FollowCurveGpencilModifierData *)md;

  return (mmd->object == nullptr);
}

static void update_depsgraph(GpencilModifierData *md,
                             const ModifierUpdateDepsgraphContext *ctx,
                             const int /*mode*/)
{
  FollowCurveGpencilModifierData *mmd = (FollowCurveGpencilModifierData *)md;
  if (mmd->object != nullptr) {
    DEG_add_object_relation(ctx->node, mmd->object, DEG_OB_COMP_GEOMETRY, "Follow Curve Modifier");
    DEG_add_object_relation(
        ctx->node, mmd->object, DEG_OB_COMP_TRANSFORM, "Follow Curve Modifier");
  }
  DEG_add_object_relation(ctx->node, ctx->object, DEG_OB_COMP_TRANSFORM, "Follow Curve Modifier");
}

static void foreach_ID_link(GpencilModifierData *md, Object *ob, IDWalkFunc walk, void *userData)
{
  FollowCurveGpencilModifierData *mmd = (FollowCurveGpencilModifierData *)md;

  walk(userData, ob, (ID **)&mmd->material, IDWALK_CB_USER);
  walk(userData, ob, (ID **)&mmd->object, IDWALK_CB_NOP);
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  uiLayout *col, *row;
  uiLayout *layout = panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = gpencil_modifier_panel_get_property_pointers(panel, &ob_ptr);

  bool entire_object = RNA_boolean_get(ptr, "entire_object");

  uiLayoutSetPropSep(layout, true);

  col = uiLayoutColumn(layout, false);
  uiItemR(col, ptr, "object", UI_ITEM_NONE, nullptr, ICON_NONE);

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

  gpencil_modifier_panel_end(layout, ptr);
}

static void mask_panel_draw(const bContext * /*C*/, Panel *panel)
{
  gpencil_modifier_masking_panel_draw(panel, true, false);
}

static void panel_register(ARegionType *region_type)
{
  PanelType *panel_type = gpencil_modifier_panel_register(
      region_type, eGpencilModifierType_FollowCurve, panel_draw);
  gpencil_modifier_subpanel_register(
      region_type, "mask", "Influence", nullptr, mask_panel_draw, panel_type);
}

GpencilModifierTypeInfo modifierType_Gpencil_FollowCurve = {
    /*name*/ N_("Follow Curve"),
    /*struct_name*/ "FollowCurveGpencilModifierData",
    /*struct_size*/ sizeof(FollowCurveGpencilModifierData),
    /*type*/ eGpencilModifierTypeType_Gpencil,
    /*flags*/ eGpencilModifierTypeFlag_SupportsEditmode,

    /*copy_data*/ copy_data,

    /*deform_stroke*/ deform_stroke,
    /*generateStrokes*/ nullptr,
    /*bake_modifier*/ bake_modifier,
    /*remap_time*/ nullptr,

    /*init_data*/ init_data,
    /*free_data*/ nullptr,
    /*is_disabled*/ is_disabled,
    /*update_depsgraph*/ update_depsgraph,
    /*depends_on_time*/ nullptr,
    /*foreach_ID_link*/ foreach_ID_link,
    /*foreach_tex_link*/ nullptr,
    /*panel_register*/ panel_register,
};
