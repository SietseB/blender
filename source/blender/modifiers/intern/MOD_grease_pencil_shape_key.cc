/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#include "DNA_defaults.h"
#include "DNA_modifier_types.h"

#include "BKE_curves.hh"
#include "BKE_geometry_set.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_material.hh"
#include "BKE_modifier.hh"

#include "BLI_listbase.h"
#include "BLI_string.h"

#include "BLO_read_write.hh"

#include "DEG_depsgraph_query.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "BLT_translation.hh"

#include "WM_types.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "MOD_grease_pencil_util.hh"
#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"

#include "ED_grease_pencil.hh"

namespace blender {

static void blend_read(BlendDataReader *reader, ModifierData *md)
{
  auto *smd = reinterpret_cast<GreasePencilShapeKeyModifierData *>(md);

  modifier::greasepencil::read_influence_data(reader, &smd->influence);

  /* Reset edit data. */
  smd->flag &= ~MOD_GREASE_PENCIL_SHAPE_KEY_IN_EDIT_MODE;
  smd->index_edited = -1;
  smd->shape_key_edit_data = nullptr;
}

static void blend_write(BlendWriter *writer, const ID * /*id_owner*/, const ModifierData *md)
{
  const auto *smd = reinterpret_cast<const GreasePencilShapeKeyModifierData *>(md);

  BLO_write_struct(writer, GreasePencilShapeKeyModifierData, smd);
  modifier::greasepencil::write_influence_data(writer, &smd->influence);
}

static void init_data(ModifierData *md)
{
  auto *smd = reinterpret_cast<GreasePencilShapeKeyModifierData *>(md);

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(smd, modifier));

  MEMCPY_STRUCT_AFTER(smd, DNA_struct_default_get(GreasePencilShapeKeyModifierData), modifier);
  modifier::greasepencil::init_influence_data(&smd->influence, false);
}

static void copy_data(const ModifierData *md, ModifierData *target, const int flag)
{
  const auto *smd = reinterpret_cast<const GreasePencilShapeKeyModifierData *>(md);
  auto *tsmd = reinterpret_cast<GreasePencilShapeKeyModifierData *>(target);

  modifier::greasepencil::free_influence_data(&tsmd->influence);

  BKE_modifier_copydata_generic(md, target, flag);
  modifier::greasepencil::copy_influence_data(&smd->influence, &tsmd->influence, flag);
}

static void free_data(ModifierData *md)
{
  auto *smd = reinterpret_cast<GreasePencilShapeKeyModifierData *>(md);
  modifier::greasepencil::free_influence_data(&smd->influence);
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  auto *smd = reinterpret_cast<GreasePencilShapeKeyModifierData *>(md);
  modifier::greasepencil::foreach_influence_ID_link(&smd->influence, ob, walk, user_data);
}

static void update_depsgraph(ModifierData * /*md*/, const ModifierUpdateDepsgraphContext *ctx)
{
  DEG_add_object_relation(
      ctx->node, ctx->object, DEG_OB_COMP_TRANSFORM, "Grease Pencil Shape Key Modifier");
}

static Array<bool> get_inactive_shape_keys(GreasePencilShapeKeyModifierData &smd,
                                           GreasePencil &grease_pencil)
{
  Array<bool> shape_key_is_inactive(BLI_listbase_count(&grease_pencil.shape_keys));

  /* Filter by muted or value is zero. */
  int shape_key_index;
  LISTBASE_FOREACH_INDEX (
      GreasePencilShapeKey *, shape_key, &grease_pencil.shape_keys, shape_key_index)
  {
    shape_key_is_inactive[shape_key_index] = (shape_key->value == 0.0f) ||
                                             (shape_key->flag & GREASE_PENCIL_SHAPE_KEY_MUTED) !=
                                                 0;
  }

  const bool filter_by_shape_key_name = (smd.shape_key_influence[0] != '\0');
  const bool inverted_shape_key_name = ((smd.flag &
                                         MOD_GREASE_PENCIL_INFLUENCE_INVERT_SHAPE_KEY) != 0);
  const bool filter_by_shape_key_pass =
      ((smd.flag & MOD_GREASE_PENCIL_INFLUENCE_USE_SHAPE_KEY_PASS_FILTER) != 0);
  const bool inverted_shape_key_pass =
      ((smd.flag & MOD_GREASE_PENCIL_INFLUENCE_INVERT_SHAPE_KEY_PASS_FILTER) != 0);

  /* Filter by shape key name and/or shape key pass index. */
  LISTBASE_FOREACH_INDEX (
      GreasePencilShapeKey *, shape_key, &grease_pencil.shape_keys, shape_key_index)
  {
    if (filter_by_shape_key_name) {
      shape_key_is_inactive[shape_key_index] |=
          (inverted_shape_key_name && BLI_strcaseeq(shape_key->name, smd.shape_key_influence)) ||
          (!inverted_shape_key_name && !BLI_strcaseeq(shape_key->name, smd.shape_key_influence));
    }
    if (filter_by_shape_key_pass) {
      shape_key_is_inactive[shape_key_index] |= (inverted_shape_key_pass &&
                                                 shape_key->pass_index == smd.shape_key_pass) ||
                                                (!inverted_shape_key_pass &&
                                                 shape_key->pass_index != smd.shape_key_pass);
    }
  }

  return shape_key_is_inactive;
}

static void modify_drawing(const GreasePencilShapeKeyModifierData &smd,
                           const ModifierEvalContext &ctx,
                           const GreasePencil &grease_pencil,
                           const Span<bool> shape_key_is_inactive,
                           bke::greasepencil::Drawing &drawing)
{
  modifier::greasepencil::ensure_no_bezier_curves(drawing);

  bke::CurvesGeometry &curves = drawing.strokes_for_write();
  IndexMaskMemory mask_memory;
  const IndexMask stroke_mask = modifier::greasepencil::get_filtered_stroke_mask(
      ctx.object, curves, smd.influence, mask_memory);
  const int edited_shape_key_index = (smd.flag & MOD_GREASE_PENCIL_SHAPE_KEY_IN_EDIT_MODE) != 0 ?
                                         smd.index_edited :
                                         -1;
  Vector<int> shape_key_indices;
  Vector<float> shape_key_factors;

  int shape_key_index;
  LISTBASE_FOREACH_INDEX (
      GreasePencilShapeKey *, shape_key, &grease_pencil.shape_keys, shape_key_index)
  {
    /* Skip shape keys that are muted or filtered out by the shape key influence of the modifier.
     * But apply a shape key that is currently edited, because in edit mode the shape key effect
     * must always be visible. */
    if (shape_key_is_inactive[shape_key_index] && shape_key_index != edited_shape_key_index) {
      continue;
    }

    /* When a shape key is edited, skip the onion-skin style drawings that are meant to show the
     * drawing WITHOUT the shape key applied. */
    if (shape_key_index == edited_shape_key_index &&
        drawing.runtime->is_shape_key_onion_skin_drawing)
    {
      continue;
    }

    const float factor = (shape_key_index == edited_shape_key_index) ?
                             1.0f :
                             shape_key->value * smd.factor;
    if (factor == 0.0f) {
      continue;
    }

    shape_key_indices.append(shape_key_index);
    shape_key_factors.append(factor);
  }

  if (shape_key_indices.is_empty()) {
    return;
  }

  if (ed::greasepencil::shape_key::apply_shape_keys_to_drawing(
          drawing, shape_key_indices, shape_key_factors, stroke_mask))
  {
    drawing.tag_positions_changed();
  }
}

static void before_modify_geometry_set(ModifierData *md,
                                       const ModifierEvalContext *ctx,
                                       bke::GeometrySet *geometry_set)
{
  using bke::greasepencil::Drawing;

  auto &smd = *reinterpret_cast<GreasePencilShapeKeyModifierData *>(md);

  if (!geometry_set->has_grease_pencil()) {
    return;
  }
  GreasePencil &grease_pencil = *geometry_set->get_grease_pencil_for_write();
  if (BLI_listbase_is_empty(&grease_pencil.shape_keys)) {
    return;
  }

  /* Update shape key deltas on the fly when a shape key is edited. */
  ed::greasepencil::shape_key::ShapeKeyEditData *edit_data =
      reinterpret_cast<ed::greasepencil::shape_key::ShapeKeyEditData *>(smd.shape_key_edit_data);
  const Scene &scene = *DEG_get_evaluated_scene(ctx->depsgraph);

  const Vector<Drawing *> drawings = ed::greasepencil::retrieve_visible_drawings_at_frame(
      scene, grease_pencil, grease_pencil.runtime->eval_frame);
  ed::greasepencil::shape_key::get_shape_key_stroke_deltas(*edit_data, drawings);
}

static void modify_geometry_set(ModifierData *md,
                                const ModifierEvalContext *ctx,
                                bke::GeometrySet *geometry_set)
{
  using namespace modifier::greasepencil;
  using bke::greasepencil::Drawing;
  using bke::greasepencil::Layer;

  auto &smd = *reinterpret_cast<GreasePencilShapeKeyModifierData *>(md);

  if (!geometry_set->has_grease_pencil()) {
    return;
  }
  GreasePencil &grease_pencil = *geometry_set->get_grease_pencil_for_write();
  const int frame = grease_pencil.runtime->eval_frame;
  if (BLI_listbase_is_empty(&grease_pencil.shape_keys)) {
    return;
  }
  Array<bool> shape_key_is_inactive = get_inactive_shape_keys(smd, grease_pencil);

  /* Modify layers. */
  IndexMaskMemory mask_memory;
  const IndexMask layer_mask = modifier::greasepencil::get_filtered_layer_mask(
      grease_pencil, smd.influence, mask_memory);

  Vector<int> shape_key_indices;
  Vector<float> shape_key_factors;
  int shape_key_index;
  LISTBASE_FOREACH_INDEX (
      GreasePencilShapeKey *, shape_key, &grease_pencil.shape_keys, shape_key_index)
  {
    /* Skip muted shape keys and shapes keys excluded by the influence filters. */
    if (shape_key_is_inactive[shape_key_index]) {
      continue;
    }

    /* Skip a shape key when it is currently edited, because the layer properties are already
     * applied to the layers (to be visible in the UI).*/
    if ((smd.flag & MOD_GREASE_PENCIL_SHAPE_KEY_IN_EDIT_MODE) != 0 &&
        shape_key_index == smd.index_edited)
    {
      continue;
    }

    shape_key_indices.append(shape_key_index);
    shape_key_factors.append(shape_key->value * smd.factor);
  }
  if (!shape_key_indices.is_empty()) {
    ed::greasepencil::shape_key::apply_shape_keys_to_layers(
        grease_pencil, shape_key_indices, shape_key_factors, layer_mask);
  }

  /* Modify drawings. */
  const Vector<LayerDrawingInfo> drawing_infos = get_drawing_infos_by_layer(
      grease_pencil, layer_mask, frame);
  threading::parallel_for(drawing_infos.index_range(), 1, [&](const IndexRange info_range) {
    for (const int info_i : info_range) {
      Drawing &drawing = *drawing_infos[info_i].drawing;
      modify_drawing(smd, *ctx, grease_pencil, shape_key_is_inactive, drawing);
    }
  });

  /* When in shape key editing mode, we have to check for changes in the layer transforms. We want
   * to keep the base onion-skin style drawing in the original position, so when a layer transform
   * changes, we have to compensate for that.
   * Note: we can't compensate for layer opacity changes.
   */
  if ((smd.flag & MOD_GREASE_PENCIL_SHAPE_KEY_IN_EDIT_MODE) == 0) {
    return;
  }
  const ed::greasepencil::shape_key::ShapeKeyEditData &edit_data =
      *reinterpret_cast<ed::greasepencil::shape_key::ShapeKeyEditData *>(smd.shape_key_edit_data);
  const GreasePencil &grease_pencil_orig = *reinterpret_cast<GreasePencil *>(
      DEG_get_original_object(ctx->object)->data);

  threading::parallel_for(drawing_infos.index_range(), 1, [&](const IndexRange info_range) {
    for (const int info_i : info_range) {
      /* Check for onion-skin style base drawings. */
      Drawing &drawing = *drawing_infos[info_i].drawing;
      if (!drawing.runtime->is_shape_key_onion_skin_drawing) {
        continue;
      }

      /* Check for changes in layer transform. */
      const int layer_index =
          grease_pencil.layer(drawing_infos[info_i].layer_index).shape_key_edit_index - 1;
      if (layer_index == -1) {
        continue;
      }
      const Layer &layer = grease_pencil_orig.layer(layer_index);
      const float3 translation_delta = float3(layer.translation) -
                                       ed::greasepencil::shape_key::get_base_layer_translation(
                                           edit_data, layer_index);
      const float3 rotation_delta = float3(layer.rotation) -
                                    ed::greasepencil::shape_key::get_base_layer_rotation(
                                        edit_data, layer_index);
      const float3 scale_delta = float3(layer.scale) /
                                 ed::greasepencil::shape_key::get_base_layer_scale(edit_data,
                                                                                   layer_index);
      if (math::is_zero(translation_delta) && math::is_zero(rotation_delta) &&
          math::is_equal(scale_delta, {1.0f, 1.0f, 1.0f}))
      {
        continue;
      }

      /* Change all point positions in the drawing to compensate for the layer transform change. */
      const float4x4 transform_matrix = math::invert(
          math::from_loc_rot_scale<float4x4, math::EulerXYZ>(
              translation_delta, rotation_delta, scale_delta));
      bke::CurvesGeometry &curves = drawing.strokes_for_write();
      MutableSpan<float3> positions = curves.positions_for_write();
      threading::parallel_for(curves.points_range(), 512, [&](const IndexRange point_range) {
        for (const int point : point_range) {
          positions[point] = math::transform_point(transform_matrix, positions[point]);
        }
      });
    }
  });
}

static void draw_shape_key_filter_settings(uiLayout *layout, PointerRNA *ptr)
{
  PointerRNA ob_ptr = RNA_pointer_create_discrete(ptr->owner_id, &RNA_Object, ptr->owner_id);
  PointerRNA obj_data_ptr = RNA_pointer_get(&ob_ptr, "data");
  const bool has_shape_key = RNA_string_length(ptr, "shape_key_name") != 0;
  const bool use_shape_key_pass = RNA_boolean_get(ptr, "use_shape_key_pass_filter");
  uiLayout *row, *col, *sub, *subsub;

  uiLayoutSetPropSep(layout, true);

  col = uiLayoutColumn(layout, true);
  row = uiLayoutRow(col, true);
  uiLayoutSetPropDecorate(row, false);
  uiItemPointerR(row, ptr, "shape_key_name", &obj_data_ptr, "shape_keys", std::nullopt, ICON_NONE);
  sub = uiLayoutRow(row, true);
  uiLayoutSetActive(sub, has_shape_key);
  uiLayoutSetPropDecorate(sub, false);
  uiItemR(sub, ptr, "invert_shape_key", UI_ITEM_NONE, "", ICON_ARROW_LEFTRIGHT);

  row = uiLayoutRowWithHeading(col, true, IFACE_("Shape Key Pass"));
  uiLayoutSetPropDecorate(row, false);
  sub = uiLayoutRow(row, true);
  uiItemR(sub, ptr, "use_shape_key_pass_filter", UI_ITEM_NONE, "", ICON_NONE);
  subsub = uiLayoutRow(sub, true);
  uiLayoutSetActive(subsub, use_shape_key_pass);
  uiItemR(subsub, ptr, "shape_key_pass_filter", UI_ITEM_NONE, "", ICON_NONE);
  uiItemR(subsub, ptr, "invert_shape_key_pass_filter", UI_ITEM_NONE, "", ICON_ARROW_LEFTRIGHT);
}

static void panel_draw(const bContext *C, Panel *panel)
{
  uiLayout *layout = panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  uiLayoutSetPropSep(layout, true);

  uiItemR(layout, ptr, "factor", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  if (uiLayout *influence_panel = uiLayoutPanelProp(
          C, layout, ptr, "open_influence_panel", IFACE_("Influence")))
  {
    modifier::greasepencil::draw_layer_filter_settings(C, influence_panel, ptr);
    modifier::greasepencil::draw_material_filter_settings(C, influence_panel, ptr);
    draw_shape_key_filter_settings(influence_panel, ptr);
  }

  modifier_panel_end(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_GreasePencilShapeKey, panel_draw);
}

}  // namespace blender

ModifierTypeInfo modifierType_GreasePencilShapeKey = {
    /*idname*/ "GreasePencilShapeKey",
    /*name*/ N_("Shape Key"),
    /*struct_name*/ "GreasePencilShapeKeyModifierData",
    /*struct_size*/ sizeof(GreasePencilShapeKeyModifierData),
    /*srna*/ &RNA_GreasePencilShapeKeyModifier,
    /*type*/ ModifierTypeType::OnlyDeform,
    /*flags*/ eModifierTypeFlag_AcceptsGreasePencil | eModifierTypeFlag_SupportsEditmode |
        eModifierTypeFlag_EnableInEditmode | eModifierTypeFlag_SupportsMapping,
    /*icon*/ ICON_SHAPEKEY_DATA,

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
    /*is_disabled*/ nullptr,
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
    /*before_modify_geometry_set*/ blender::before_modify_geometry_set,
};
