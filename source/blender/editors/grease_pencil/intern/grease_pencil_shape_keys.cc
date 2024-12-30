/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edgreasepencil
 */

#include "BKE_animsys.h"
#include "BKE_grease_pencil.hh"

#include "BLI_string.h"
#include "BLI_string_utils.hh"

#include "BLT_translation.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "DEG_depsgraph_query.hh"

#include "ED_curves.hh"
#include "ED_grease_pencil.hh"

namespace blender::ed::greasepencil {

constexpr StringRef SHAPE_KEY_ATTRIBUTE_PREFIX = ".shapekey-";
constexpr StringRef SHAPE_KEY_LAYER_LOCATION = "location-";
constexpr StringRef SHAPE_KEY_LAYER_ROTATION = "rotation-";
constexpr StringRef SHAPE_KEY_LAYER_SCALE = "scale-";
constexpr StringRef SHAPE_KEY_LAYER_OPACITY = "opacity-";
constexpr StringRef SHAPE_KEY_STROKE_FILL_COLOR = "fill-color-";
constexpr StringRef SHAPE_KEY_POINT_POS_QUATERNION = "pos-quaternion-";
constexpr StringRef SHAPE_KEY_POINT_POS_DISTANCE = "pos-distance-";
constexpr StringRef SHAPE_KEY_POINT_RADIUS = "radius-";
constexpr StringRef SHAPE_KEY_POINT_OPACITY = "opacity-";
constexpr StringRef SHAPE_KEY_POINT_VERTEX_COLOR = "vertex-color-";

/* State flag: is a shape key being edited? */
bool in_edit_mode = false;

static void shape_key_attribute_increase_index(bke::MutableAttributeAccessor &attributes,
                                               const StringRef shape_key_attribute,
                                               const int index,
                                               const int max_index)
{
  for (int i = max_index; i >= index; i--) {
    StringRef attribute_name = SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_attribute +
                               std::to_string(i);
    StringRef attribute_name_new = SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_attribute +
                                   std::to_string(i + 1);
    if (attributes.contains(attribute_name)) {
      attributes.rename(attribute_name, attribute_name_new);
    }
  }
}

/* When the order of shape keys has changed, adjust the indices of shape key attributes in layers,
 * strokes and points accordingly. */
static void shape_key_increase_index(GreasePencil &grease_pencil, const int index)
{
  const int max_index = BLI_listbase_count(&grease_pencil.shape_keys) - 1;

  /* Check shape key attributes on layers. */
  bke::MutableAttributeAccessor attributes = grease_pencil.attributes_for_write();
  shape_key_attribute_increase_index(attributes, SHAPE_KEY_LAYER_LOCATION, index, max_index);
  shape_key_attribute_increase_index(attributes, SHAPE_KEY_LAYER_ROTATION, index, max_index);
  shape_key_attribute_increase_index(attributes, SHAPE_KEY_LAYER_SCALE, index, max_index);
  shape_key_attribute_increase_index(attributes, SHAPE_KEY_LAYER_OPACITY, index, max_index);

  /* Check shape key attributes on drawings (strokes and points). */
  MutableSpan<GreasePencilDrawingBase *> drawings = grease_pencil.drawings();
  for (GreasePencilDrawingBase *drawing_base : drawings) {
    if (drawing_base->type != GP_DRAWING) {
      continue;
    }
    bke::greasepencil::Drawing *drawing =
        &(reinterpret_cast<GreasePencilDrawing *>(drawing_base))->wrap();

    bke::CurvesGeometry &curves = drawing->strokes_for_write();
    attributes = curves.attributes_for_write();

    shape_key_attribute_increase_index(attributes, SHAPE_KEY_STROKE_FILL_COLOR, index, max_index);
    shape_key_attribute_increase_index(
        attributes, SHAPE_KEY_POINT_POS_QUATERNION, index, max_index);
    shape_key_attribute_increase_index(attributes, SHAPE_KEY_POINT_POS_DISTANCE, index, max_index);
    shape_key_attribute_increase_index(attributes, SHAPE_KEY_POINT_RADIUS, index, max_index);
    shape_key_attribute_increase_index(attributes, SHAPE_KEY_POINT_OPACITY, index, max_index);
    shape_key_attribute_increase_index(attributes, SHAPE_KEY_POINT_VERTEX_COLOR, index, max_index);
  }
}

static int shape_key_add_exec(bContext *C, wmOperator *op)
{
  GreasePencil *grease_pencil = from_context(*C);

  /* Get (optional) shape key name. */
  bool name_given = false;
  char name[128];
  PropertyRNA *prop = RNA_struct_find_property(op->ptr, "name");
  if (RNA_property_is_set(op->ptr, prop)) {
    RNA_property_string_get(op->ptr, prop, name);
    name_given = true;
  }
  else {
    strcpy(name, "ShapeKey");
  }

  /* Create shape key and set default values. */
  GreasePencilShapeKey *shape_key_active = BKE_grease_pencil_shape_key_active_get(grease_pencil);
  GreasePencilShapeKey *shape_key_new = static_cast<GreasePencilShapeKey *>(
      MEM_callocN(sizeof(GreasePencilShapeKey), __func__));
  if (shape_key_active != nullptr) {
    BLI_insertlinkafter(&grease_pencil->shape_keys, shape_key_active, shape_key_new);
  }
  else {
    BLI_addtail(&grease_pencil->shape_keys, shape_key_new);
  }
  shape_key_new->range_min = 0.0f;
  shape_key_new->range_max = 1.0f;
  shape_key_new->value = 0.0f;
  shape_key_new->layer_order_compare = GREASE_PENCIL_SHAPE_KEY_COMPARE_GREATER_THAN;
  shape_key_new->layer_order_value = 0.5f;

  /* Copy values of currently active shape key. */
  const int index = BLI_findindex(&grease_pencil->shape_keys, shape_key_new);
  if (shape_key_active != nullptr) {
    if (!name_given) {
      strcpy(name, shape_key_active->name);
    }
    shape_key_new->range_min = shape_key_active->range_min;
    shape_key_new->range_max = shape_key_active->range_max;
    shape_key_new->layer_order_compare = shape_key_active->layer_order_compare;
    shape_key_new->layer_order_value = shape_key_active->layer_order_value;

    /* Renumber indices in the shape key attributes of layer and stroke shape keys. */
    if (shape_key_new->next != nullptr) {
      shape_key_increase_index(*grease_pencil, index);
    }
  }

  /* Auto-name shape key. */
  BLI_strncpy(shape_key_new->name, DATA_(name), sizeof(shape_key_new->name));
  BLI_uniquename(&grease_pencil->shape_keys,
                 shape_key_new,
                 DATA_("ShapeKey"),
                 '.',
                 offsetof(GreasePencilShapeKey, name),
                 sizeof(shape_key_new->name));

  /* Set active. */
  BKE_grease_pencil_shape_key_active_set(grease_pencil, index);

  /* TODO: Add shape key modifier automatically when there isn't one. */

  DEG_id_tag_update(&grease_pencil->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_shape_key_add(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Add New Shape Key";
  ot->idname = "GREASE_PENCIL_OT_shape_key_add";
  ot->description = "Add a new shape key to the Grease Pencil object";
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Callbacks. */
  ot->poll = editable_grease_pencil_poll;
  ot->exec = shape_key_add_exec;

  /* Properties. */
  PropertyRNA *prop;
  prop = RNA_def_string(ot->srna, "name", nullptr, MAX_NAME, "Name", "Name of the new shape key");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  ot->prop = prop;
}

static void shape_key_attribute_remove(bke::MutableAttributeAccessor &attributes,
                                       const StringRef shape_key_attribute,
                                       const int index,
                                       const int max_index)
{
  StringRef attribute_name = SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_attribute +
                             std::to_string(index);
  attributes.remove(attribute_name);

  for (int i = index + 1; i <= max_index; i++) {
    attribute_name = SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_attribute + std::to_string(i);
    StringRef attribute_name_new = SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_attribute +
                                   std::to_string(i - 1);
    if (attributes.contains(attribute_name)) {
      attributes.rename(attribute_name, attribute_name_new);
    }
  }
}

static int shape_key_remove_exec(bContext *C, wmOperator * /*op*/)
{
  GreasePencil *grease_pencil = from_context(*C);
  int index = grease_pencil->active_shape_key_index;
  const int max_index = BLI_listbase_count(&grease_pencil->shape_keys) - 1;

  /* Check shape key attributes on layers. */
  bke::MutableAttributeAccessor attributes = grease_pencil->attributes_for_write();
  shape_key_attribute_remove(attributes, SHAPE_KEY_LAYER_LOCATION, index, max_index);
  shape_key_attribute_remove(attributes, SHAPE_KEY_LAYER_ROTATION, index, max_index);
  shape_key_attribute_remove(attributes, SHAPE_KEY_LAYER_SCALE, index, max_index);
  shape_key_attribute_remove(attributes, SHAPE_KEY_LAYER_OPACITY, index, max_index);

  /* Check shape key attributes on drawings (strokes and points). */
  MutableSpan<GreasePencilDrawingBase *> drawings = grease_pencil->drawings();
  for (GreasePencilDrawingBase *drawing_base : drawings) {
    if (drawing_base->type != GP_DRAWING) {
      continue;
    }
    bke::greasepencil::Drawing *drawing =
        &(reinterpret_cast<GreasePencilDrawing *>(drawing_base))->wrap();

    bke::CurvesGeometry &curves = drawing->strokes_for_write();
    attributes = curves.attributes_for_write();

    shape_key_attribute_remove(attributes, SHAPE_KEY_STROKE_FILL_COLOR, index, max_index);
    shape_key_attribute_remove(attributes, SHAPE_KEY_POINT_POS_QUATERNION, index, max_index);
    shape_key_attribute_remove(attributes, SHAPE_KEY_POINT_POS_DISTANCE, index, max_index);
    shape_key_attribute_remove(attributes, SHAPE_KEY_POINT_RADIUS, index, max_index);
    shape_key_attribute_remove(attributes, SHAPE_KEY_POINT_OPACITY, index, max_index);
    shape_key_attribute_remove(attributes, SHAPE_KEY_POINT_VERTEX_COLOR, index, max_index);
  }

  /* Remove animation data. */
  GreasePencilShapeKey *shape_key = BKE_grease_pencil_shape_key_active_get(grease_pencil);
  char name_esc[sizeof(shape_key->name) * 2];
  char rna_path[sizeof(name_esc) + 32];
  BLI_str_escape(name_esc, shape_key->name, sizeof(name_esc));
  BLI_snprintf(rna_path, sizeof(rna_path), "shape_keys[\"%s\"]", name_esc);
  BKE_animdata_fix_paths_remove(&grease_pencil->id, rna_path);

  /* Set new active shape key. */
  if (index == max_index) {
    index = math::max(0, index - 1);
  }
  BKE_grease_pencil_shape_key_active_set(grease_pencil, index);

  /* Delete shape key. */
  BLI_freelinkN(&grease_pencil->shape_keys, shape_key);

  /* TODO: When all shape keys are deleted, remove shape key modifiers automatically. */
  if (BLI_listbase_is_empty(&grease_pencil->shape_keys)) {
  }

  DEG_id_tag_update(&grease_pencil->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);

  return OPERATOR_FINISHED;
}

static bool shape_key_active_poll(bContext *C)
{
  if (!editable_grease_pencil_poll(C)) {
    return false;
  }

  GreasePencil *grease_pencil = from_context(*C);
  return !BLI_listbase_is_empty(&grease_pencil->shape_keys) && !in_edit_mode;
}

static void GREASE_PENCIL_OT_shape_key_remove(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Remove Shape Key";
  ot->idname = "GREASE_PENCIL_OT_shape_key_remove";
  ot->description = "Remove the active shape key in the Grease Pencil object";
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Callbacks. */
  ot->poll = shape_key_active_poll;
  ot->exec = shape_key_remove_exec;
}

static void shape_key_attribute_move(bke::MutableAttributeAccessor attributes,
                                     const StringRef shape_key_attribute,
                                     const int old_index,
                                     const int new_index,
                                     const int max_index)
{
  StringRef attribute_old = SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_attribute +
                            std::to_string(old_index);
  StringRef attribute_new = SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_attribute +
                            std::to_string(new_index);
  StringRef attribute_temp = SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_attribute +
                             std::to_string(max_index + 1);

  /* Swap shape key attributes with old and new index. */
  if (attributes.contains(attribute_old)) {
    attributes.rename(attribute_old, attribute_temp);
  }
  if (attributes.contains(attribute_new)) {
    attributes.rename(attribute_new, attribute_old);
  }
  if (attributes.contains(attribute_temp)) {
    attributes.rename(attribute_temp, attribute_new);
  }
}

static int shape_key_move_exec(bContext *C, wmOperator *op)
{
  GreasePencil *grease_pencil = from_context(*C);
  const int old_index = grease_pencil->active_shape_key_index;
  const int max_index = BLI_listbase_count(&grease_pencil->shape_keys) - 1;

  const int direction = RNA_enum_get(op->ptr, "direction");
  const int new_index = old_index + direction;
  if (new_index < 0 || new_index > max_index) {
    return OPERATOR_CANCELLED;
  }

  /* Move shape key in list. */
  GreasePencilShapeKey *shape_key = BKE_grease_pencil_shape_key_active_get(grease_pencil);
  BLI_listbase_link_move(&grease_pencil->shape_keys, shape_key, direction);
  BKE_grease_pencil_shape_key_active_set(grease_pencil, new_index);

  /* Check shape key attributes on layers. */
  bke::MutableAttributeAccessor attributes = grease_pencil->attributes_for_write();
  shape_key_attribute_move(attributes, SHAPE_KEY_LAYER_LOCATION, old_index, new_index, max_index);
  shape_key_attribute_move(attributes, SHAPE_KEY_LAYER_ROTATION, old_index, new_index, max_index);
  shape_key_attribute_move(attributes, SHAPE_KEY_LAYER_SCALE, old_index, new_index, max_index);
  shape_key_attribute_move(attributes, SHAPE_KEY_LAYER_OPACITY, old_index, new_index, max_index);

  /* Check shape key attributes on drawings (strokes and points). */
  MutableSpan<GreasePencilDrawingBase *> drawings = grease_pencil->drawings();
  for (GreasePencilDrawingBase *drawing_base : drawings) {
    if (drawing_base->type != GP_DRAWING) {
      continue;
    }
    bke::greasepencil::Drawing *drawing =
        &(reinterpret_cast<GreasePencilDrawing *>(drawing_base))->wrap();

    bke::CurvesGeometry &curves = drawing->strokes_for_write();
    attributes = curves.attributes_for_write();

    shape_key_attribute_move(
        attributes, SHAPE_KEY_STROKE_FILL_COLOR, old_index, new_index, max_index);
    shape_key_attribute_move(
        attributes, SHAPE_KEY_POINT_POS_QUATERNION, old_index, new_index, max_index);
    shape_key_attribute_move(
        attributes, SHAPE_KEY_POINT_POS_DISTANCE, old_index, new_index, max_index);
    shape_key_attribute_move(attributes, SHAPE_KEY_POINT_RADIUS, old_index, new_index, max_index);
    shape_key_attribute_move(attributes, SHAPE_KEY_POINT_OPACITY, old_index, new_index, max_index);
    shape_key_attribute_move(
        attributes, SHAPE_KEY_POINT_VERTEX_COLOR, old_index, new_index, max_index);
  }

  DEG_id_tag_update(&grease_pencil->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_shape_key_move(wmOperatorType *ot)
{
  static const EnumPropertyItem shape_key_move_direction[] = {
      {-1, "UP", 0, "Up", ""},
      {1, "DOWN", 0, "Down", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  /* Identifiers. */
  ot->name = "Move Shape Key";
  ot->idname = "GREASE_PENCIL_OT_shape_key_move";
  ot->description = "Move the active shape key up/down in the list";
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Callbacks. */
  ot->poll = shape_key_active_poll;
  ot->exec = shape_key_move_exec;

  /* Properties. */
  RNA_def_enum(ot->srna,
               "direction",
               shape_key_move_direction,
               0,
               "Direction",
               "Direction to move the active shape key (up/down)");
}

static void shape_key_attributes_duplicate(bke::MutableAttributeAccessor &attributes,
                                           const StringRef shape_key_attribute,
                                           const int index_src,
                                           const int index_dst)
{
  StringRef attribute_id_src = SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_attribute +
                               std::to_string(index_src);
  StringRef attribute_id_dst = SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_attribute +
                               std::to_string(index_dst);

  if (!attributes.contains(attribute_id_src)) {
    return;
  }

  /* Duplicate shape key attribute by using implicit sharing. */
  const std::optional<bke::AttributeMetaData> meta_data = attributes.lookup_meta_data(
      attribute_id_src);
  const bke::GAttributeReader attribute_src = attributes.lookup(
      attribute_id_src, meta_data.value().domain, meta_data.value().data_type);
  const bke::AttributeInitShared init(attribute_src.varray.get_internal_span().data(),
                                      *attribute_src.sharing_info);
  attributes.add(attribute_id_dst, meta_data.value().domain, meta_data.value().data_type, init);
}

static int shape_key_duplicate_exec(bContext *C, wmOperator *op)
{
  GreasePencil *grease_pencil = from_context(*C);
  const int index_src = grease_pencil->active_shape_key_index;
  GreassePencilShapeKey *shape_key_src = BKE_grease_pencil_shape_key_active_get(grease_pencil);
  const float value_src = shape_key_src->value;

  /* Create new shape key, based on the active one. */
  if (shape_key_add_exec(C, op) == OPERATOR_CANCELLED) {
    return OPERATOR_CANCELLED;
  }
  GreassePencilShapeKey *shape_key_dst = BKE_grease_pencil_shape_key_active_get(grease_pencil);
  shape_key_dst->value = value_src;
  const int index_dst = grease_pencil->active_shape_key_index;

  /* Copy shape key attributes on layers. */
  bke::MutableAttributeAccessor attributes = grease_pencil->attributes_for_write();
  shape_key_attributes_duplicate(attributes, SHAPE_KEY_LAYER_LOCATION, index_src, index_dst);
  shape_key_attributes_duplicate(attributes, SHAPE_KEY_LAYER_ROTATION, index_src, index_dst);
  shape_key_attributes_duplicate(attributes, SHAPE_KEY_LAYER_SCALE, index_src, index_dst);
  shape_key_attributes_duplicate(attributes, SHAPE_KEY_LAYER_OPACITY, index_src, index_dst);

  /* Copy shape key attributes on strokes and points. */
  MutableSpan<GreasePencilDrawingBase *> drawings = grease_pencil->drawings();
  for (GreasePencilDrawingBase *drawing_base : drawings) {
    if (drawing_base->type != GP_DRAWING) {
      continue;
    }
    bke::greasepencil::Drawing *drawing =
        &(reinterpret_cast<GreasePencilDrawing *>(drawing_base))->wrap();

    bke::CurvesGeometry &curves = drawing->strokes_for_write();
    attributes = curves.attributes_for_write();

    shape_key_attributes_duplicate(attributes, SHAPE_KEY_STROKE_FILL_COLOR, index_src, index_dst);
    shape_key_attributes_duplicate(
        attributes, SHAPE_KEY_POINT_POS_QUATERNION, index_src, index_dst);
    shape_key_attributes_duplicate(attributes, SHAPE_KEY_POINT_POS_DISTANCE, index_src, index_dst);
    shape_key_attributes_duplicate(attributes, SHAPE_KEY_POINT_RADIUS, index_src, index_dst);
    shape_key_attributes_duplicate(attributes, SHAPE_KEY_POINT_OPACITY, index_src, index_dst);
    shape_key_attributes_duplicate(attributes, SHAPE_KEY_POINT_VERTEX_COLOR, index_src, index_dst);
  }

  DEG_id_tag_update(&grease_pencil->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_shape_key_duplicate(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Duplicate Shape Key";
  ot->idname = "GREASE_PENCIL_OT_shape_key_duplicate";
  ot->description = "Duplicate the active shape key";
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Callbacks. */
  ot->poll = shape_key_active_poll;
  ot->exec = shape_key_duplicate_exec;

  /* Properties. */
  PropertyRNA *prop;
  prop = RNA_def_string(ot->srna, "name", nullptr, MAX_NAME, "Name", "Name of the new shape key");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  ot->prop = prop;
}

// static void GREASE_PENCIL_OT_shape_key_remove_all(wmOperatorType *ot) {}

// static void GREASE_PENCIL_OT_shape_key_edit(wmOperatorType *ot) {}

// static void GREASE_PENCIL_OT_shape_key_edit_finish(wmOperatorType *ot) {}

// static void GREASE_PENCIL_OT_shape_key_apply_all(wmOperatorType *ot) {}

}  // namespace blender::ed::greasepencil

bool ED_grease_pencil_shape_key_in_edit_mode()
{
  return blender::ed::greasepencil::in_edit_mode;
}

void ED_operatortypes_grease_pencil_shape_keys()
{
  using namespace blender::ed::greasepencil;
  WM_operatortype_append(GREASE_PENCIL_OT_shape_key_add);
  WM_operatortype_append(GREASE_PENCIL_OT_shape_key_remove);
  WM_operatortype_append(GREASE_PENCIL_OT_shape_key_move);
  WM_operatortype_append(GREASE_PENCIL_OT_shape_key_duplicate);
  // WM_operatortype_append(GREASE_PENCIL_OT_shape_key_edit);
  // WM_operatortype_append(GREASE_PENCIL_OT_shape_key_edit_finish);
  // WM_operatortype_append(GREASE_PENCIL_OT_shape_key_remove_all);
  // WM_operatortype_append(GREASE_PENCIL_OT_shape_key_apply_all);
}
