/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2017 Blender Foundation. */

/** \file
 * \ingroup modifiers
 */

#include <stdio.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "BLI_hash.h"
#include "BLI_math_vector.h"
#include "BLI_rand.h"

#include "BLT_translation.h"

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

#include "BKE_context.h"
#include "BKE_curve.h"
#include "BKE_curves.h"
#include "BKE_deform.h"
#include "BKE_fcurve.h"
#include "BKE_gpencil_geom_legacy.h"
#include "BKE_gpencil_modifier_legacy.h"
#include "BKE_lib_query.h"
#include "BKE_main.h"
#include "BKE_modifier.h"
#include "BKE_scene.h"
#include "BKE_screen.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "RNA_access.h"
#include "RNA_prototypes.h"

#include "MOD_gpencil_legacy_modifiertypes.h"
#include "MOD_gpencil_legacy_ui_common.h"
#include "MOD_gpencil_legacy_util.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_build.h"
#include "DEG_depsgraph_query.h"

void MOD_gpencil_follow_curve_frame_init(const Depsgraph *depsgraph,
                                         const GpencilModifierData *md,
                                         const Scene *scene,
                                         Object *ob)
{
  FollowCurveGpencilModifierData *mmd = (FollowCurveGpencilModifierData *)md;

  /* Get frame range up to current frame. */
  int frame_current = scene->r.cfra;
  mmd->cfra = frame_current;

  /* Get animated speed and speed variation. */
  mmd->speed_per_frame_len = 0;
  FCurve *speed_fcurve = id_data_find_fcurve(
      &ob->id, mmd, &RNA_FollowCurveGpencilModifier, "speed", 0, NULL);
  FCurve *speed_var_fcurve = id_data_find_fcurve(
      &ob->id, mmd, &RNA_FollowCurveGpencilModifier, "speed_var", 0, NULL);
  if (speed_fcurve != NULL || speed_var_fcurve != NULL) {
    mmd->speed_per_frame_len = frame_current;
  }

  /* When animated, create array with speed and speed variation per frame. */
  mmd->speed_per_frame = NULL;
  if (mmd->speed_per_frame_len > 0) {
    /* One stride contains: speed, speed variation. */
    mmd->speed_per_frame = MEM_malloc_arrayN(frame_current * 2, sizeof(float), __func__);
    for (int frame = 1; frame <= frame_current; frame++) {
      float speed = (speed_fcurve) ? evaluate_fcurve(speed_fcurve, (float)frame) : mmd->speed;
      float speed_var = (speed_var_fcurve) ? evaluate_fcurve(speed_var_fcurve, (float)frame) :
                                             mmd->speed_variation;
      int stride = frame * 2;
      mmd->speed_per_frame[stride] = speed;
      mmd->speed_per_frame[stride + 1] = speed_var;
    }
  }

  /* Count Bezier curves in collection. */
  mmd->curves_len = 0;
  if (mmd->collection) {
    LISTBASE_FOREACH (CollectionObject *, cob, &mmd->collection->gobject) {
      if (cob->ob == NULL || cob->ob->type != OB_CURVES_LEGACY) {
        continue;
      }

      /* Loop splines. */
      Curve *curve = (Curve *)cob->ob->data;
      LISTBASE_FOREACH (Nurb *, nurb, &curve->nurb) {
        if (nurb->type == CU_BEZIER) {
          mmd->curves_len++;
          break;
        }
      }
    }
  }

  /* Convert Bezier curves to points. */
  mmd->curves = NULL;
  if (mmd->curves_len > 0) {
    mmd->curves = MEM_calloc_arrayN(mmd->curves_len, sizeof(GPFollowCurve), __func__);

    LISTBASE_FOREACH (CollectionObject *, cob, &mmd->collection->gobject) {
      Object *ob_eval = (Object *)DEG_get_evaluated_object(depsgraph, cob->ob);
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
        follow_curve->points = MEM_mallocN((follow_curve->points_len + 1) * stride, __func__);
        GPFollowCurvePoint *point_offset = follow_curve->points;

        /* Convert spline segments of Bezier curve to points. */
        for (int i = 0; i < segments; i++) {
          int i_next = (i + 1) % nurb->pntsu;
          BezTriple *bezt = &nurb->bezt[i];
          BezTriple *bezt_next = &nurb->bezt[i_next];
          for (int axis = 0; axis < 3; axis++) {
            BKE_curve_forward_diff_bezier(bezt->vec[1][axis],
                                          bezt->vec[2][axis],
                                          bezt_next->vec[0][axis],
                                          bezt_next->vec[1][axis],
                                          POINTER_OFFSET(point_offset, sizeof(float) * axis),
                                          mmd->curve_resolution,
                                          stride);
          }
          point_offset = POINTER_OFFSET(point_offset, stride * mmd->curve_resolution);
        }

        /* Transform to world space. */
        for (int i = 0; i < follow_curve->points_len; i++) {
          GPFollowCurvePoint *point = &follow_curve->points[i];
          mul_m4_v3(ob_eval->object_to_world, point->co);
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
}

#undef CURVE_RESOLUTION

static void get_random_float(const int seed, float *random_value)
{
  RNG *rng = BLI_rng_new(seed);
  for (int i = 0; i < 3; i++) {
    random_value[i] = BLI_rng_get_float(rng);
  }
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

  /* Get the radius (= the shortest distance of the point to the line) and the direction. */
  sub_v3_v3(p_on_line, point);
  mul_v3_v3v3(vec_dir, point, line_vec);
  float direction = (dot_v3v3(vec_dir, line_vec) < 0) ? -1.0f : 1.0f;
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

static GPFollowCurve *stroke_get_current_curve_and_distance(const GpencilModifierData *md,
                                                            const Object *ob,
                                                            const bGPDframe *gpf,
                                                            const bGPDstroke *gps,
                                                            const float gps_length,
                                                            float *dist_on_curve,
                                                            float *radius_initial,
                                                            float *angle_initial,
                                                            bool *start_at_tail)
{
  FollowCurveGpencilModifierData *mmd = (FollowCurveGpencilModifierData *)md;

  /* Get initial random values for this stroke. */
  float random_val[3];
  int seed = mmd->seed;
  seed += BLI_hash_string(ob->id.name + 2);
  seed += BLI_hash_string(md->name);
  seed += BLI_findindex(&gpf->strokes, gps);
  get_random_float(seed, random_val);

  const float speed_var_f = (random_val[0] - 0.5f) * 2.0f;
  float speed = mmd->speed + mmd->speed_variation * speed_var_f;
  if ((mmd->flag & GP_FOLLOWCURVE_VARY_DIR) && random_val[1] < 0.5f) {
    speed *= -1.0f;
  }
  *start_at_tail = (speed < 0.0f);

  if (fabsf(mmd->spiral_factor) < FLT_EPSILON) {
    *angle_initial = mmd->angle;
  }
  else {
    *angle_initial = mmd->angle + M_PI_2 * random_val[1];
  }

  int curve_index = (int)(mmd->curves_len * random_val[2]);
  GPFollowCurve *curve = &mmd->curves[curve_index];

  /* Get initial distance from stroke to curve. */
  const bool tail_first = ((mmd->flag & GP_FOLLOWCURVE_STROKE_TAIL_FIRST) != 0);
  float dist_to_curve_initial;
  float *stroke_start = (tail_first) ? (&gps->points[gps->totpoints - 1].x) : (&gps->points[0].x);

  get_distance_of_point_to_line(stroke_start,
                                curve->points[0].co,
                                curve->points[0].vec_to_next,
                                &dist_to_curve_initial,
                                radius_initial);

  /* We always start at the beginning of a curve, so limit the distance to zero or less. */
  if (dist_to_curve_initial > 0.0f) {
    dist_to_curve_initial = 0.0f;
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
  dist_travelled = fabsf(dist_travelled) + dist_to_curve_initial;

  /* When the animation is not repeated, we can finish here. */
  if ((mmd->flag & GP_FOLLOWCURVE_REPEAT) == 0) {
    *dist_on_curve = dist_travelled;

    return curve;
  }

  /* When animated, we search for the curve the stroke is currently projected on. */
  while (dist_travelled > (curve->length + gps_length)) {
    /* Step over current curve. */
    dist_travelled -= curve->length + gps_length;

    /* Select next curve randomly. */
    seed += 1731;
    get_random_float(seed, random_val);
    curve_index = (int)(mmd->curves_len * random_val[2]);
    curve = &mmd->curves[curve_index];
  }

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

static void curve_get_point_by_distance(const float dist,
                                        GPFollowCurve *curve,
                                        float *point,
                                        float *point_vec)
{
  /* Check boundaries. */
  if (dist < 0.0f) {
    /* Before curve start, project point on vector of first curve point. */
    copy_v3_v3(point_vec, curve->points[0].vec_to_next);
    mul_v3_v3fl(point, point_vec, dist);
    add_v3_v3(point, curve->points[0].co);
    return;
  }
  else if (dist > curve->length) {
    /* After curve end, project point on vector of last curve point. */
    const int index = curve->points_len - 1;
    copy_v3_v3(point_vec, curve->points[index].vec_to_next);
    mul_v3_v3fl(point, point_vec, (dist - curve->length));
    add_v3_v3(point, curve->points[index].co);
    return;
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
}

static void initData(GpencilModifierData *md)
{
  FollowCurveGpencilModifierData *mmd = (FollowCurveGpencilModifierData *)md;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(mmd, modifier));

  MEMCPY_STRUCT_AFTER(mmd, DNA_struct_default_get(FollowCurveGpencilModifierData), modifier);
}

static void copyData(const GpencilModifierData *md, GpencilModifierData *target)
{
  BKE_gpencil_modifier_copydata_generic(md, target);
}

/* deform stroke */
static void deformStroke(GpencilModifierData *md,
                         Depsgraph *UNUSED(depsgraph),
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
  float *gps_segment_length = MEM_malloc_arrayN(gps->totpoints, sizeof(float), __func__);
  float gps_length = stroke_get_length(gps, gps_segment_length);

  /* Get current curve to project the stroke on. */
  float dist_on_curve, radius_initial, angle_initial;
  bool curve_start_at_tail;
  GPFollowCurve *curve = stroke_get_current_curve_and_distance(md,
                                                               ob,
                                                               gpf,
                                                               gps,
                                                               gps_length,
                                                               &dist_on_curve,
                                                               &radius_initial,
                                                               &angle_initial,
                                                               &curve_start_at_tail);

  /* Get the direction of the stroke points. */
  const bool gps_start_at_tail = ((mmd->flag & GP_FOLLOWCURVE_STROKE_TAIL_FIRST) != 0);
  const int gps_dir = (gps_start_at_tail) ? -1 : 1;
  const int gps_start_index = (gps_start_at_tail) ? (gps->totpoints - 1) : 0;
  const int gps_end_index = (gps_start_at_tail) ? 0 : (gps->totpoints - 1);
  float gps_start[3], gps_end[3];
  copy_v3_v3(gps_start, &gps->points[gps_start_index].x);
  copy_v3_v3(gps_end, &gps->points[gps_end_index].x);

  /* Create stroke profile. For now this is just a straight line between the
   * first and last point of the stroke.
   *
   * Stroke   __/\  _/\  /\____
   *              \/   \/
   *
   * Profile  _________________
   *
   */
  float profile[3];
  sub_v3_v3v3(profile, gps_end, gps_start);
  normalize_v3(profile);

  /* Get rotation plane for spiral angle. */
  float rotation_plane[3] = {0.0f};
  get_rotation_plane(mmd->angle_axis, angle_initial, rotation_plane);

  /* Get spiral setting. */
  const bool use_spiral = (fabsf(mmd->spiral_factor) > FLT_EPSILON);

  /* Loop all stroke points and project them on the curve. */
  for (int i = gps_start_index; i >= 0 && i < gps->totpoints; i += gps_dir) {
    /* For notational convenience, copy stroke point. */
    float gps_p[3];
    copy_v3_v3(gps_p, &gps->points[i].x);

    /* Get distance and radius of point to stroke profile. */
    float gps_p_dist, gps_p_radius;
    get_distance_of_point_to_line(gps_p, gps_start, profile, &gps_p_dist, &gps_p_radius);

    /* Find closest point on curve given a distance. */
    float curve_p[3], curve_p_vec[3];
    float curve_dist = dist_on_curve - gps_p_dist;
    if (curve_start_at_tail) {
      curve_dist = curve->length - curve_dist;
    }
    curve_get_point_by_distance(curve_dist, curve, curve_p, curve_p_vec);

    /* Project stroke point on curve segment by finding the orthogonal vector
     * in the plane of the spiral angle.
     */
    float p_rotated[3];
    if (use_spiral) {
      // TODO!!! Take speed into account... (how??)
      const float angle = angle_initial +
                          mmd->spiral_factor * M_PI_2 * (curve_dist / curve->length);
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
static void bakeModifier(Main *UNUSED(bmain),
                         Depsgraph *depsgraph,
                         GpencilModifierData *md,
                         Object *ob)
{
  FollowCurveGpencilModifierData *mmd = (FollowCurveGpencilModifierData *)md;

  if (mmd->collection == NULL) {
    return;
  }

  generic_bake_deform_stroke(depsgraph, md, ob, true, deformStroke);
}

static bool isDisabled(GpencilModifierData *md, int UNUSED(userRenderParams))
{
  FollowCurveGpencilModifierData *mmd = (FollowCurveGpencilModifierData *)md;

  return !mmd->collection;
}

static void updateDepsgraph(GpencilModifierData *md,
                            const ModifierUpdateDepsgraphContext *ctx,
                            const int UNUSED(mode))
{
  FollowCurveGpencilModifierData *lmd = (FollowCurveGpencilModifierData *)md;
  if (lmd->collection != NULL) {
    DEG_add_collection_geometry_relation(ctx->node, lmd->collection, "Follow Curve Modifier");
  }
}

static void foreachIDLink(GpencilModifierData *md, Object *ob, IDWalkFunc walk, void *userData)
{
  FollowCurveGpencilModifierData *mmd = (FollowCurveGpencilModifierData *)md;

  walk(userData, ob, (ID **)&mmd->material, IDWALK_CB_USER);
  walk(userData, ob, (ID **)&mmd->collection, IDWALK_CB_NOP);
}

static void panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  uiLayout *col, *row;
  uiLayout *layout = panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = gpencil_modifier_panel_get_property_pointers(panel, &ob_ptr);

  uiLayoutSetPropSep(layout, true);

  col = uiLayoutColumn(layout, false);
  uiItemR(col, ptr, "collection", 0, NULL, ICON_NONE);

  col = uiLayoutColumn(layout, false);
  uiItemR(col, ptr, "curve_resolution", 0, NULL, ICON_NONE);

  col = uiLayoutColumn(layout, true);
  uiItemR(col, ptr, "speed", UI_ITEM_R_SLIDER, NULL, ICON_NONE);
  uiItemR(col, ptr, "speed_variation", UI_ITEM_R_SLIDER, NULL, ICON_NONE);

  col = uiLayoutColumn(layout, false);
  uiItemR(col, ptr, "vary_dir", 0, NULL, ICON_NONE);

  col = uiLayoutColumn(layout, false);
  uiItemR(col, ptr, "seed", 0, NULL, ICON_NONE);

  col = uiLayoutColumn(layout, true);
  uiItemR(col, ptr, "angle", 0, NULL, ICON_NONE);
  uiItemR(col, ptr, "spiral_factor", 0, NULL, ICON_NONE);

  row = uiLayoutRow(layout, false);
  uiItemR(row, ptr, "axis", UI_ITEM_R_EXPAND, NULL, ICON_NONE);

  col = uiLayoutColumn(layout, true);
  uiItemR(col, ptr, "tail_first", 0, NULL, ICON_NONE);
  uiItemR(col, ptr, "repeat", 0, NULL, ICON_NONE);
  uiItemR(col, ptr, "dissolve", 0, NULL, ICON_NONE);
  uiItemR(col, ptr, "scatter", 0, NULL, ICON_NONE);

  gpencil_modifier_panel_end(layout, ptr);
}

static void mask_panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  gpencil_modifier_masking_panel_draw(panel, true, false);
}

static void panelRegister(ARegionType *region_type)
{
  PanelType *panel_type = gpencil_modifier_panel_register(
      region_type, eGpencilModifierType_FollowCurve, panel_draw);
  gpencil_modifier_subpanel_register(
      region_type, "mask", "Influence", NULL, mask_panel_draw, panel_type);
}

GpencilModifierTypeInfo modifierType_Gpencil_FollowCurve = {
    /*name*/ N_("Follow Curve"),
    /*structName*/ "FollowCurveGpencilModifierData",
    /*structSize*/ sizeof(FollowCurveGpencilModifierData),
    /*type*/ eGpencilModifierTypeType_Gpencil,
    /*flags*/ eGpencilModifierTypeFlag_SupportsEditmode,

    /*copyData*/ copyData,

    /*deformStroke*/ deformStroke,
    /*generateStrokes*/ NULL,
    /*bakeModifier*/ bakeModifier,
    /*remapTime*/ NULL,

    /*initData*/ initData,
    /*freeData*/ NULL,
    /*isDisabled*/ isDisabled,
    /*updateDepsgraph*/ updateDepsgraph,
    /*dependsOnTime*/ NULL,
    /*foreachIDLink*/ foreachIDLink,
    /*foreachTexLink*/ NULL,
    /*panelRegister*/ panelRegister,
};
