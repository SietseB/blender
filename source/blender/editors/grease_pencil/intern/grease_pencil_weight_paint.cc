/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edgreasepencil
 */

#include "BLI_task.hh"

#include "BKE_brush.hh"
#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_crazyspace.hh"
#include "BKE_deform.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_grease_pencil_vertex_groups.hh"
#include "BKE_modifier.hh"
#include "BKE_object_deform.h"
#include "BKE_paint.hh"
#include "BKE_report.hh"

#include "DNA_meshdata_types.h"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "ED_curves.hh"
#include "ED_view3d.hh"

#include "DEG_depsgraph_query.hh"

#include "ED_grease_pencil.hh"

namespace blender::ed::greasepencil {

Set<std::string> get_bone_deformed_vertex_group_names(const Object &object)
{
  /* Get all vertex group names in the object. */
  const ListBase *defbase = BKE_object_defgroup_list(&object);
  Set<std::string> defgroups;
  LISTBASE_FOREACH (bDeformGroup *, dg, defbase) {
    defgroups.add(dg->name);
  }

  /* Inspect all armature modifiers in the object. */
  Set<std::string> bone_deformed_vgroups;
  VirtualModifierData virtual_modifier_data;
  ModifierData *md = BKE_modifiers_get_virtual_modifierlist(&object, &virtual_modifier_data);
  for (; md; md = md->next) {
    if (!(md->mode & (eModifierMode_Realtime | eModifierMode_Virtual)) ||
        md->type != eModifierType_Armature)
    {
      continue;
    }
    ArmatureModifierData *amd = reinterpret_cast<ArmatureModifierData *>(md);
    if (!amd->object || !amd->object->pose) {
      continue;
    }

    bPose *pose = amd->object->pose;
    LISTBASE_FOREACH (bPoseChannel *, channel, &pose->chanbase) {
      if (channel->bone->flag & BONE_NO_DEFORM) {
        continue;
      }
      /* When a vertex group name matches the bone name, it is bone-deformed. */
      if (defgroups.contains(channel->name)) {
        bone_deformed_vgroups.add(channel->name);
      }
    }
  }

  return bone_deformed_vgroups;
}

/* Normalize the weights of vertex groups deformed by bones so that the sum is 1.0f.
 * Returns false when the normalization failed due to too many locked vertex groups. In that case a
 * second pass can be done with the active vertex group unlocked.
 */
static bool normalize_vertex_weights_try(MDeformVert &dvert,
                                         const int vertex_groups_num,
                                         const Span<bool> vertex_group_is_bone_deformed,
                                         const FunctionRef<bool(int)> vertex_group_is_locked)
{
  /* Nothing to normalize when there are less than two vertex group weights. */
  if (dvert.totweight <= 1) {
    return true;
  }

  /* Get the sum of weights of bone-deformed vertex groups. */
  float sum_weights_total = 0.0f;
  float sum_weights_locked = 0.0f;
  float sum_weights_unlocked = 0.0f;
  int locked_num = 0;
  int unlocked_num = 0;
  for (const int i : IndexRange(dvert.totweight)) {
    MDeformWeight &dw = dvert.dw[i];

    /* Auto-normalize is only applied on bone-deformed vertex groups that have weight already. */
    if (dw.def_nr >= vertex_groups_num || !vertex_group_is_bone_deformed[dw.def_nr] ||
        dw.weight <= FLT_EPSILON)
    {
      continue;
    }

    sum_weights_total += dw.weight;

    if (vertex_group_is_locked(dw.def_nr)) {
      locked_num++;
      sum_weights_locked += dw.weight;
    }
    else {
      unlocked_num++;
      sum_weights_unlocked += dw.weight;
    }
  }

  /* Already normalized? */
  if (sum_weights_total == 1.0f) {
    return true;
  }

  /* Any unlocked vertex group to normalize? */
  if (unlocked_num == 0) {
    /* We don't need a second pass when there is only one locked group (the active group). */
    return (locked_num == 1);
  }

  /* Locked groups can make it impossible to fully normalize. */
  if (sum_weights_locked >= 1.0f - VERTEX_WEIGHT_LOCK_EPSILON) {
    /* Zero out the weights we are allowed to touch and return false, indicating a second pass is
     * needed. */
    for (const int i : IndexRange(dvert.totweight)) {
      MDeformWeight &dw = dvert.dw[i];
      if (dw.def_nr < vertex_groups_num && vertex_group_is_bone_deformed[dw.def_nr] &&
          !vertex_group_is_locked(dw.def_nr))
      {
        dw.weight = 0.0f;
      }
    }

    return (sum_weights_locked == 1.0f);
  }

  /* When the sum of the unlocked weights isn't zero, we can use a multiplier to normalize them
   * to 1.0f. */
  if (sum_weights_unlocked != 0.0f) {
    const float normalize_factor = (1.0f - sum_weights_locked) / sum_weights_unlocked;

    for (const int i : IndexRange(dvert.totweight)) {
      MDeformWeight &dw = dvert.dw[i];
      if (dw.def_nr < vertex_groups_num && vertex_group_is_bone_deformed[dw.def_nr] &&
          dw.weight > FLT_EPSILON && !vertex_group_is_locked(dw.def_nr))
      {
        dw.weight = math::clamp(dw.weight * normalize_factor, 0.0f, 1.0f);
      }
    }

    return true;
  }

  /* Spread out the remainder of the locked weights over the unlocked weights. */
  const float weight_remainder = math::clamp(
      (1.0f - sum_weights_locked) / unlocked_num, 0.0f, 1.0f);

  for (const int i : IndexRange(dvert.totweight)) {
    MDeformWeight &dw = dvert.dw[i];
    if (dw.def_nr < vertex_groups_num && vertex_group_is_bone_deformed[dw.def_nr] &&
        dw.weight > FLT_EPSILON && !vertex_group_is_locked(dw.def_nr))
    {
      dw.weight = weight_remainder;
    }
  }

  return true;
}

void normalize_vertex_weights(MDeformVert &dvert,
                              const int active_vertex_group,
                              const Span<bool> vertex_group_is_locked,
                              const Span<bool> vertex_group_is_bone_deformed)
{
  /* Try to normalize the weights with both active and explicitly locked vertex groups restricted
   * from change. */
  const auto active_vertex_group_is_locked = [&](const int vertex_group_index) {
    return vertex_group_is_locked[vertex_group_index] || vertex_group_index == active_vertex_group;
  };
  const bool success = normalize_vertex_weights_try(dvert,
                                                    vertex_group_is_locked.size(),
                                                    vertex_group_is_bone_deformed,
                                                    active_vertex_group_is_locked);

  if (success) {
    return;
  }

  /* Do a second pass with the active vertex group unlocked. */
  const auto active_vertex_group_is_unlocked = [&](const int vertex_group_index) {
    return vertex_group_is_locked[vertex_group_index];
  };
  normalize_vertex_weights_try(dvert,
                               vertex_group_is_locked.size(),
                               vertex_group_is_bone_deformed,
                               active_vertex_group_is_unlocked);
}

struct ClosestGreasePencilDrawing {
  const bke::greasepencil::Drawing *drawing = nullptr;
  int active_defgroup_index;
  ed::curves::FindClosestData elem = {};
};

static int weight_sample_invoke(bContext *C, wmOperator * /*op*/, const wmEvent *event)
{
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);

  /* Get the active vertex group. */
  const int object_defgroup_nr = BKE_object_defgroup_active_index_get(vc.obact) - 1;
  if (object_defgroup_nr == -1) {
    return OPERATOR_CANCELLED;
  }
  const bDeformGroup *object_defgroup = static_cast<const bDeformGroup *>(
      BLI_findlink(BKE_object_defgroup_list(vc.obact), object_defgroup_nr));

  /* Collect visible drawings. */
  const Object *ob_eval = DEG_get_evaluated_object(vc.depsgraph, const_cast<Object *>(vc.obact));
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(vc.obact->data);
  const Vector<DrawingInfo> drawings = retrieve_visible_drawings(*vc.scene, grease_pencil, false);

  /* Find stroke points closest to mouse cursor position. */
  const ClosestGreasePencilDrawing closest = threading::parallel_reduce(
      drawings.index_range(),
      1L,
      ClosestGreasePencilDrawing(),
      [&](const IndexRange range, const ClosestGreasePencilDrawing &init) {
        ClosestGreasePencilDrawing new_closest = init;
        for (const int i : range) {
          DrawingInfo info = drawings[i];
          const bke::greasepencil::Layer &layer = *grease_pencil.layer(info.layer_index);

          /* Skip drawing when it doesn't use the active vertex group. */
          const int drawing_defgroup_nr = BLI_findstringindex(
              &info.drawing.strokes().vertex_group_names,
              object_defgroup->name,
              offsetof(bDeformGroup, name));
          if (drawing_defgroup_nr == -1) {
            continue;
          }

          /* Get deformation by modifiers. */
          bke::crazyspace::GeometryDeformation deformation =
              bke::crazyspace::get_evaluated_grease_pencil_drawing_deformation(
                  ob_eval, *vc.obact, info.layer_index, info.frame_number);

          IndexMaskMemory memory;
          const IndexMask points = retrieve_visible_points(*vc.obact, info.drawing, memory);
          if (points.is_empty()) {
            continue;
          }
          const float4x4 layer_to_world = layer.to_world_space(*ob_eval);
          const float4x4 projection = ED_view3d_ob_project_mat_get_from_obmat(vc.rv3d,
                                                                              layer_to_world);
          const bke::CurvesGeometry &curves = info.drawing.strokes();
          std::optional<ed::curves::FindClosestData> new_closest_elem =
              ed::curves::closest_elem_find_screen_space(vc,
                                                         curves.points_by_curve(),
                                                         deformation.positions,
                                                         curves.cyclic(),
                                                         projection,
                                                         points,
                                                         bke::AttrDomain::Point,
                                                         event->mval,
                                                         new_closest.elem);
          if (new_closest_elem) {
            new_closest.elem = *new_closest_elem;
            new_closest.drawing = &info.drawing;
            new_closest.active_defgroup_index = drawing_defgroup_nr;
          }
        }
        return new_closest;
      },
      [](const ClosestGreasePencilDrawing &a, const ClosestGreasePencilDrawing &b) {
        return (a.elem.distance < b.elem.distance) ? a : b;
      });

  if (!closest.drawing) {
    return OPERATOR_CANCELLED;
  }

  /* From the closest point found, get the vertex weight in the active vertex group. */
  const VArray<float> point_weights = bke::varray_for_deform_verts(
      closest.drawing->strokes().deform_verts(), closest.active_defgroup_index);
  const float new_weight = math::clamp(point_weights[closest.elem.index], 0.0f, 1.0f);

  /* Set the new brush weight. */
  const ToolSettings *ts = vc.scene->toolsettings;
  Brush *brush = BKE_paint_brush(&ts->wpaint->paint);
  BKE_brush_weight_set(vc.scene, brush, new_weight);

  /* Update brush settings in UI. */
  WM_main_add_notifier(NC_BRUSH | NA_EDITED, nullptr);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_weight_sample(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Weight Paint Sample Weight";
  ot->idname = "GREASE_PENCIL_OT_weight_sample";
  ot->description =
      "Set the weight of the Draw tool to the weight of the vertex under the mouse cursor";

  /* Callbacks. */
  ot->poll = grease_pencil_weight_painting_poll;
  ot->invoke = weight_sample_invoke;

  /* Flags. */
  ot->flag = OPTYPE_UNDO | OPTYPE_DEPENDS_ON_CURSOR;
}

static int toggle_weight_tool_direction(bContext *C, wmOperator * /*op*/)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *brush = BKE_paint_brush(paint);

  /* Toggle direction flag. */
  brush->flag ^= BRUSH_DIR_IN;

  /* Update brush settings in UI. */
  WM_main_add_notifier(NC_BRUSH | NA_EDITED, nullptr);

  return OPERATOR_FINISHED;
}

static bool toggle_weight_tool_direction_poll(bContext *C)
{
  if (!(grease_pencil_weight_painting_poll(C) || grease_pencil_weight_gradient_poll(C))) {
    return false;
  }

  Paint *paint = BKE_paint_get_active_from_context(C);
  if (paint == nullptr) {
    return false;
  }
  Brush *brush = BKE_paint_brush(paint);
  if (brush == nullptr) {
    return false;
  }
  return ELEM(brush->gpencil_weight_tool, GPWEIGHT_TOOL_DRAW, GPWEIGHT_TOOL_GRADIENT);
}

static void GREASE_PENCIL_OT_weight_toggle_direction(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Weight Paint Toggle Direction";
  ot->idname = "GREASE_PENCIL_OT_weight_toggle_direction";
  ot->description = "Toggle Add/Subtract for the weight paint draw tool";

  /* Callbacks. */
  ot->poll = toggle_weight_tool_direction_poll;
  ot->exec = toggle_weight_tool_direction;

  /* Flags. */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* -------------------------------------------------------------------- */
/** \name Weight Gradient Operator
 * \{ */

enum class WeightGradientType : uint8_t {
  Linear,
  Radial,
};

enum class BrushMode : uint8_t {
  Normal,
  Invert,
};

enum WeightGradientFlags : uint8_t {
  WPAINT_GRADIENT_POINT_DW_EXISTS = (1 << 0),
  WPAINT_GRADIENT_POINT_IS_MODIFIED = (1 << 1),
};
ENUM_OPERATORS(WeightGradientFlags, WPAINT_GRADIENT_POINT_IS_MODIFIED);

struct WeightGradientDrawingCache {
  int active_vertex_group;
  float multi_frame_falloff;
  MutableSpan<MDeformVert> deform_verts;
  VMutableArray<float> deform_weights;

  Vector<bool> locked_vgroups;
  Vector<bool> bone_deformed_vgroups;

  Array<float2> point_positions;
  Array<float> point_original_weights;
  Array<uint8_t> point_flags;
};

struct WeightGradientToolData {
  Object *object;
  GreasePencil *grease_pencil;
  Brush *brush;
  float brush_strength;
  float brush_weight;
  float weight_direction;

  bool auto_normalize;
  Array<bke::greasepencil::Drawing *> drawings;
  Array<WeightGradientDrawingCache> drawing_cache;
};

static void weight_gradient_finish(WeightGradientToolData *tool)
{
  MEM_delete(tool);
}

static int weight_gradient_exec(bContext *C, wmOperator *op)
{
  wmGesture *gesture = static_cast<wmGesture *>(op->customdata);
  if (gesture == nullptr || gesture->user_data.data == nullptr) {
    return OPERATOR_CANCELLED;
  }
  WeightGradientToolData &tool = *static_cast<WeightGradientToolData *>(gesture->user_data.data);

  /* Get gradient type (linear/radial). */
  const WeightGradientType gradient_type = WeightGradientType(RNA_enum_get(op->ptr, "type"));

  /* Get position and length of the interactive gradient line in the viewport. */
  const int x_start = RNA_int_get(op->ptr, "xstart");
  const int y_start = RNA_int_get(op->ptr, "ystart");
  const int x_end = RNA_int_get(op->ptr, "xend");
  const int y_end = RNA_int_get(op->ptr, "yend");
  const float2 gradient_start = {float(x_start), float(y_start)};
  const float2 gradient_end = {float(x_end), float(y_end)};

  const float2 gradient_vector = gradient_end - gradient_start;
  const float gradient_length_sq = math::length_squared(gradient_vector);
  const float gradient_length = sqrtf(gradient_length_sq);
  if (gradient_length_sq == 0.0f) {
    return OPERATOR_FINISHED;
  }

  /* Update vertex weights based on the interactive gradient. */
  threading::parallel_for_each(tool.drawing_cache, [&](WeightGradientDrawingCache &cache) {
    threading::parallel_for(
        cache.point_flags.index_range(), 1024, [&](const IndexRange point_range) {
          for (const int point : point_range) {
            /* Reset vertex weight. */
            if ((cache.point_flags[point] & WPAINT_GRADIENT_POINT_IS_MODIFIED) &&
                cache.deform_verts[point].dw)
            {
              cache.deform_weights.set(point, cache.point_original_weights[point]);
            }

            /* Get vector of gradient line starting point to the stroke point. */
            const float2 vec_point_to_gradient = cache.point_positions[point] - gradient_start;

            /* Calculate weight change. */
            float gradient_factor = 0.0f;
            switch (gradient_type) {
              case WeightGradientType::Linear: {
                /* For the linear gradient, get the orthogonal position of the stroke point towards
                 * the gradient line. */
                const float dist_on_gradient_line = math::max(
                    0.0f, math::dot(vec_point_to_gradient, gradient_vector));
                if (dist_on_gradient_line > gradient_length_sq) {
                  continue;
                }
                gradient_factor = (dist_on_gradient_line / gradient_length_sq) * gradient_length;
                break;
              }
              case WeightGradientType::Radial: {
                /* For the radial gradient, get the distance of the stroke point to the center of
                 * the gradient. */
                gradient_factor = math::length(vec_point_to_gradient);
                if (gradient_factor > gradient_length) {
                  continue;
                }
                break;
              }
              default:
                BLI_assert_unreachable();
                break;
            }

            /* Set new weight. */
            const float gradient_falloff = BKE_brush_curve_strength(
                tool.brush, gradient_factor, gradient_length);
            const float weight_change = tool.brush_weight * tool.brush_strength *
                                        tool.weight_direction * gradient_falloff *
                                        cache.multi_frame_falloff;
            const float new_weight = math::clamp(
                cache.point_original_weights[point] + weight_change, 0.0f, 1.0f);
            cache.deform_weights.set(point, new_weight);
            cache.point_flags[point] |= WPAINT_GRADIENT_POINT_IS_MODIFIED;

            /* Auto-normalize weights. */
            if (tool.auto_normalize) {
              normalize_vertex_weights(cache.deform_verts[point],
                                       cache.active_vertex_group,
                                       cache.locked_vgroups,
                                       cache.bone_deformed_vgroups);
            }
          }
        });
  });

  /* Update viewport. */
  DEG_id_tag_update(&tool.grease_pencil->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, tool.grease_pencil);

  return OPERATOR_FINISHED;
}

static void weight_gradient_cancel(const bContext &C, WeightGradientToolData &tool)
{
  /* Reset vertex weights. */
  threading::parallel_for_each(tool.drawing_cache, [&](WeightGradientDrawingCache &cache) {
    threading::parallel_for(
        cache.point_flags.index_range(), 1024, [&](const IndexRange point_range) {
          for (const int point : point_range) {
            if ((cache.point_flags[point] & WPAINT_GRADIENT_POINT_IS_MODIFIED) == 0) {
              continue;
            }
            if (cache.point_flags[point] & WPAINT_GRADIENT_POINT_DW_EXISTS) {
              cache.deform_weights.set(point, cache.point_original_weights[point]);
            }
            else {
              BKE_defvert_remove_group(&cache.deform_verts[point], cache.deform_verts[point].dw);
            }
          }
        });
  });

  /* Update viewport. */
  DEG_id_tag_update(&tool.grease_pencil->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(&C, NC_GEOM | ND_DATA, tool.grease_pencil);

  weight_gradient_finish(&tool);
}

static int weight_gradient_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  wmGesture &gesture = *static_cast<wmGesture *>(op->customdata);
  WeightGradientToolData &tool = *static_cast<WeightGradientToolData *>(gesture.user_data.data);

  int result = WM_gesture_straightline_modal(C, op, event);

  /* Check for mouse release. */
  if (result & OPERATOR_RUNNING_MODAL) {
    if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {
      weight_gradient_finish(&tool);
      WM_gesture_straightline_cancel(C, op);
      result &= ~OPERATOR_RUNNING_MODAL;
      result |= OPERATOR_FINISHED;
    }
  }

  /* Operator canceled. */
  if (result & OPERATOR_CANCELLED) {
    weight_gradient_cancel(*C, tool);
  }

  return result;
}

static bool active_vertex_group_is_locked(const bContext &C)
{
  const Object *object = CTX_data_active_object(&C);
  int object_defgroup_nr = BKE_object_defgroup_active_index_get(object) - 1;
  if (object_defgroup_nr == -1) {
    return false;
  }
  bDeformGroup *object_defgroup = static_cast<bDeformGroup *>(
      BLI_findlink(BKE_object_defgroup_list(object), object_defgroup_nr));

  return (object_defgroup->flag & DG_LOCK_WEIGHT);
}

static void init_weight_gradient_cache(const bContext &C,
                                       const wmOperator &op,
                                       WeightGradientToolData &tool)
{
  const Scene &scene = *CTX_data_scene(&C);
  Paint *paint = BKE_paint_get_active_from_context(&C);
  tool.object = CTX_data_active_object(&C);
  tool.grease_pencil = static_cast<GreasePencil *>(tool.object->data);
  tool.brush = BKE_paint_brush(paint);
  tool.brush_strength = BKE_brush_alpha_get(&scene, tool.brush);
  tool.brush_weight = BKE_brush_weight_get(&scene, tool.brush);

  BKE_curvemapping_init(tool.brush->curve);

  /* Get the add/subtract mode of the gradient tool. */
  tool.weight_direction = (tool.brush->flag & BRUSH_DIR_IN) ? -1.0f : 1.0f;
  if (RNA_enum_get(op.ptr, "mode") == int(BrushMode::Invert)) {
    tool.weight_direction *= -1.0f;
  }

  /* Auto-normalize weights is only applied when the object is deformed by an armature. */
  const ToolSettings *ts = CTX_data_tool_settings(&C);
  tool.auto_normalize = ts->auto_normalize &&
                        (BKE_modifiers_is_deformed_by_armature(tool.object) != nullptr);

  /* Get or create active vertex group in GP object. */
  int object_defgroup_nr = BKE_object_defgroup_active_index_get(tool.object) - 1;
  if (object_defgroup_nr == -1) {
    BKE_object_defgroup_add(tool.object);
    object_defgroup_nr = 0;
  }
  bDeformGroup *object_defgroup = static_cast<bDeformGroup *>(
      BLI_findlink(BKE_object_defgroup_list(tool.object), object_defgroup_nr));

  /* Get locked and bone-deformed vertex groups in GP object. */
  Set<std::string> object_locked_defgroups;
  Set<std::string> object_bone_deformed_defgroups;
  const ListBase *defgroups = BKE_object_defgroup_list(tool.object);
  LISTBASE_FOREACH (bDeformGroup *, dg, defgroups) {
    if ((dg->flag & DG_LOCK_WEIGHT) != 0) {
      object_locked_defgroups.add(dg->name);
    }
  }
  object_bone_deformed_defgroups = get_bone_deformed_vertex_group_names(*tool.object);

  /* Build a cache with screen space positions and initial weights of the stroke points in all
   * editable drawings. */
  const Depsgraph *depsgraph = CTX_data_depsgraph_pointer(&C);
  const Object *ob_eval = DEG_get_evaluated_object(depsgraph, tool.object);
  const RegionView3D *rv3d = CTX_wm_region_view3d(&C);
  const ARegion *region = CTX_wm_region(&C);
  const float4x4 projection = ED_view3d_ob_project_mat_get(rv3d, tool.object);

  const Vector<MutableDrawingInfo> drawing_infos = retrieve_editable_drawings_with_falloff(
      scene, *tool.grease_pencil);
  tool.drawings = Array<bke::greasepencil::Drawing *>(drawing_infos.size());
  tool.drawing_cache = Array<WeightGradientDrawingCache>(drawing_infos.size());

  threading::parallel_for(tool.drawings.index_range(), 1, [&](const IndexRange drawing_range) {
    for (const int drawing_index : drawing_range) {
      const MutableDrawingInfo &drawing_info = drawing_infos[drawing_index];

      tool.drawings[drawing_index] = &drawing_infos[drawing_index].drawing;

      /* Find or create the active vertex group in the drawing. */
      bke::CurvesGeometry &curves = drawing_info.drawing.strokes_for_write();
      WeightGradientDrawingCache &cache = tool.drawing_cache[drawing_index];
      cache.active_vertex_group = bke::greasepencil::ensure_vertex_group(
          object_defgroup->name, curves.vertex_group_names);

      cache.multi_frame_falloff = drawing_info.multi_frame_falloff;
      cache.deform_verts = curves.deform_verts_for_write();
      cache.deform_weights = bke::varray_for_mutable_deform_verts(cache.deform_verts,
                                                                  cache.active_vertex_group);

      /* Create boolean arrays indicating whether a vertex group is locked/bone deformed
       * or not. */
      if (tool.auto_normalize) {
        LISTBASE_FOREACH (bDeformGroup *, dg, &curves.vertex_group_names) {
          cache.locked_vgroups.append(object_locked_defgroups.contains(dg->name));
          cache.bone_deformed_vgroups.append(object_bone_deformed_defgroups.contains(dg->name));
        }
      }

      /* Convert stroke points to screen space positions. */
      bke::crazyspace::GeometryDeformation deformation =
          bke::crazyspace::get_evaluated_grease_pencil_drawing_deformation(
              ob_eval, *tool.object, drawing_info.layer_index, drawing_info.frame_number);

      cache.point_positions.reinitialize(deformation.positions.size());
      cache.point_original_weights.reinitialize(deformation.positions.size());
      cache.point_flags.reinitialize(deformation.positions.size());
      cache.point_flags.fill(0);

      threading::parallel_for(curves.points_range(), 1024, [&](const IndexRange point_range) {
        for (const int point : point_range) {
          cache.point_positions[point] = ED_view3d_project_float_v2_m4(
              region, deformation.positions[point], projection);

          /* Store original weight. */
          if (cache.deform_verts[point].dw != nullptr) {
            cache.point_flags[point] = WPAINT_GRADIENT_POINT_DW_EXISTS;
          }
          cache.point_original_weights[point] = cache.deform_weights[point];
        }
      });
    }
  });
}

static int weight_gradient_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  /* Check if active vertex group is locked. */
  if (active_vertex_group_is_locked(*C)) {
    BKE_report(op->reports, RPT_WARNING, "Active group is locked, aborting");
    return OPERATOR_CANCELLED;
  }

  /* Invoke interactive line drawing (representing the gradient) in viewport. */
  const int result = WM_gesture_straightline_invoke(C, op, event);
  if ((result & OPERATOR_RUNNING_MODAL) == 0) {
    return result;
  }

  /* Initialize point weight caching for all editable drawings. */
  wmGesture *gesture = static_cast<wmGesture *>(op->customdata);
  gesture->user_data.data = MEM_new<WeightGradientToolData>(__func__);
  gesture->user_data.use_free = false;
  WeightGradientToolData *tool = static_cast<WeightGradientToolData *>(gesture->user_data.data);

  init_weight_gradient_cache(*C, *op, *tool);

  return result;
}

static void GREASE_PENCIL_OT_weight_gradient(wmOperatorType *ot)
{
  static const EnumPropertyItem gradient_types[] = {
      {int(WeightGradientType::Linear), "LINEAR", 0, "Linear", ""},
      {int(WeightGradientType::Radial), "RADIAL", 0, "Radial", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };
  static const EnumPropertyItem brush_modes[] = {
      {int(BrushMode::Normal), "NORMAL", 0, "Normal", ""},
      {int(BrushMode::Invert), "INVERT", 0, "Invert", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  /* Identifiers. */
  ot->name = "Weight Gradient";
  ot->idname = "GREASE_PENCIL_OT_weight_gradient";
  ot->description = "Draw a line to apply a weight gradient to the active vertex group";

  /* Api callbacks. */
  ot->invoke = weight_gradient_invoke;
  ot->modal = weight_gradient_modal;
  ot->exec = weight_gradient_exec;
  ot->poll = grease_pencil_weight_gradient_poll;
  ot->cancel = WM_gesture_straightline_cancel;

  /* Flags. */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_DEPENDS_ON_CURSOR;

  /* Properties. */
  PropertyRNA *prop;
  prop = RNA_def_enum(
      ot->srna, "type", gradient_types, 0, "Type", "The gradient type (linear or gradient)");
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_enum(ot->srna, "mode", brush_modes, 0, "Mode", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);

  WM_operator_properties_gesture_straightline(ot, WM_CURSOR_EDIT);
}

/** \} */

}  // namespace blender::ed::greasepencil

void ED_operatortypes_grease_pencil_weight_paint()
{
  using namespace blender::ed::greasepencil;
  WM_operatortype_append(GREASE_PENCIL_OT_weight_toggle_direction);
  WM_operatortype_append(GREASE_PENCIL_OT_weight_sample);
  WM_operatortype_append(GREASE_PENCIL_OT_weight_gradient);
}
