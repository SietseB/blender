/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edgreasepencil
 */

#include "BKE_animsys.h"
#include "BKE_context.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_modifier.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "BLI_math_matrix.h"
#include "BLI_math_quaternion.hh"
#include "BLI_math_rotation.h"
#include "BLI_string.h"
#include "BLI_string_utils.hh"

#include "BLT_translation.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "DEG_depsgraph_query.hh"

#include "ED_curves.hh"
#include "ED_grease_pencil.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "ED_undo.hh"
#include "ED_view3d.hh"

#include "BLF_api.hh"
#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

namespace blender::ed::greasepencil::shape_key {

constexpr StringRef SHAPE_KEY_BASE_STROKE_INDEX = "sk-stroke-index";
constexpr StringRef SHAPE_KEY_ATTRIBUTE_PREFIX = "sk-";
constexpr StringRef SHAPE_KEY_LAYER_TRANSLATION = "translation-";
constexpr StringRef SHAPE_KEY_LAYER_ROTATION = "rotation-";
constexpr StringRef SHAPE_KEY_LAYER_SCALE = "scale-";
constexpr StringRef SHAPE_KEY_LAYER_OPACITY = "opacity-";
constexpr StringRef SHAPE_KEY_STROKE_FILL_COLOR = "fill-color-";
constexpr StringRef SHAPE_KEY_STROKE_FILL_OPACITY = "fill-opacity-";
constexpr StringRef SHAPE_KEY_POINT_POS_QUATERNION = "pos-quaternion-";
constexpr StringRef SHAPE_KEY_POINT_POS_DISTANCE = "pos-distance-";
constexpr StringRef SHAPE_KEY_POINT_RADIUS = "radius-";
constexpr StringRef SHAPE_KEY_POINT_OPACITY = "opacity-";
constexpr StringRef SHAPE_KEY_POINT_VERTEX_COLOR = "vertex-color-";

/* State flag: is a shape key being edited? */
bool in_edit_mode = false;

/* Minimum value a property must be changed to consider it a shape key change. */
constexpr float EPSILON = 1e-5f;

/* Storage for the base drawings and layers when editing a shape key. */
struct LayerBase {
  float3 translation;
  float3 rotation;
  float3 scale;
  float opactity;
};

struct ShapeKeyEditData {
  int edited_shape_key_index;
  GreasePencilShapeKey *edited_shape_key;
  GreasePencil *grease_pencil;

  ScrArea *area;
  ARegion *region;
  int header_height;
  int npanel_width;
  void *draw_handle;

  Array<LayerBase> base_layers;
  Array<bke::CurvesGeometry> base_geometry;
};

float3 get_base_layer_translation(const ShapeKeyEditData &edit_data, const int layer_index)
{
  return edit_data.base_layers[layer_index].translation;
}

float3 get_base_layer_rotation(const ShapeKeyEditData &edit_data, const int layer_index)
{
  return edit_data.base_layers[layer_index].rotation;
}

float3 get_base_layer_scale(const ShapeKeyEditData &edit_data, const int layer_index)
{
  return edit_data.base_layers[layer_index].scale;
}

/* Change shape key attribute '.shapekey-...-<n>' to '.shapekey-...-<n+1>'. */
static void attribute_increase_index(bke::MutableAttributeAccessor &attributes,
                                     const StringRef shape_key_attribute,
                                     const int index,
                                     const int max_index)
{
  for (int i = max_index; i >= index; i--) {
    std::string attribute_name = SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_attribute +
                                 std::to_string(i);
    std::string attribute_name_new = SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_attribute +
                                     std::to_string(i + 1);
    if (attributes.contains(attribute_name)) {
      attributes.rename(attribute_name, attribute_name_new);
    }
  }
}

/* When the order of shape keys has changed, adjust the indices of shape key attributes in layers,
 * strokes and points accordingly. */
static void increase_index(GreasePencil &grease_pencil, const int index)
{
  const int max_index = BLI_listbase_count(&grease_pencil.shape_keys) - 1;

  /* Check shape key attributes on layers. */
  bke::MutableAttributeAccessor attributes = grease_pencil.attributes_for_write();
  attribute_increase_index(attributes, SHAPE_KEY_LAYER_TRANSLATION, index, max_index);
  attribute_increase_index(attributes, SHAPE_KEY_LAYER_ROTATION, index, max_index);
  attribute_increase_index(attributes, SHAPE_KEY_LAYER_SCALE, index, max_index);
  attribute_increase_index(attributes, SHAPE_KEY_LAYER_OPACITY, index, max_index);

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

    attribute_increase_index(attributes, SHAPE_KEY_STROKE_FILL_COLOR, index, max_index);
    attribute_increase_index(attributes, SHAPE_KEY_STROKE_FILL_OPACITY, index, max_index);
    attribute_increase_index(attributes, SHAPE_KEY_POINT_POS_QUATERNION, index, max_index);
    attribute_increase_index(attributes, SHAPE_KEY_POINT_POS_DISTANCE, index, max_index);
    attribute_increase_index(attributes, SHAPE_KEY_POINT_RADIUS, index, max_index);
    attribute_increase_index(attributes, SHAPE_KEY_POINT_OPACITY, index, max_index);
    attribute_increase_index(attributes, SHAPE_KEY_POINT_VERTEX_COLOR, index, max_index);
  }
}

static int add_exec(bContext *C, wmOperator *op)
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
      increase_index(*grease_pencil, index);
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

  /* Add a shape key modifier automatically when there isn't one. */
  Object *object = CTX_data_active_object(C);
  ModifierData *md = BKE_modifiers_findby_type(object, eModifierType_GreasePencilShapeKey);
  if (md == nullptr) {
    Main *bmain = CTX_data_main(C);
    Scene *scene = CTX_data_scene(C);

    md = ed::object::modifier_add(
        op->reports, bmain, scene, object, "Shape Key", eModifierType_GreasePencilShapeKey);
    if (md == nullptr) {
      BKE_report(op->reports, RPT_ERROR, "Unable to add a Shape Key modifier to the object");
    }
  }

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
  ot->exec = add_exec;

  /* Properties. */
  PropertyRNA *prop;
  prop = RNA_def_string(ot->srna, "name", nullptr, MAX_NAME, "Name", "Name of the new shape key");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  ot->prop = prop;
}

static void attribute_remove(bke::MutableAttributeAccessor &attributes,
                             const StringRef shape_key_attribute,
                             const int index,
                             const int max_index)
{
  std::string attribute_name = SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_attribute +
                               std::to_string(index);
  attributes.remove(attribute_name);

  for (int i = index + 1; i <= max_index; i++) {
    attribute_name = SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_attribute + std::to_string(i);
    std::string attribute_name_new = SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_attribute +
                                     std::to_string(i - 1);
    if (attributes.contains(attribute_name)) {
      attributes.rename(attribute_name, attribute_name_new);
    }
  }
}

static int remove_exec(bContext *C, wmOperator *op)
{
  GreasePencil *grease_pencil = from_context(*C);
  int index = grease_pencil->active_shape_key_index;
  const int max_index = BLI_listbase_count(&grease_pencil->shape_keys) - 1;

  /* Remove and renumber shape key attributes on layers. */
  bke::MutableAttributeAccessor attributes = grease_pencil->attributes_for_write();
  attribute_remove(attributes, SHAPE_KEY_LAYER_TRANSLATION, index, max_index);
  attribute_remove(attributes, SHAPE_KEY_LAYER_ROTATION, index, max_index);
  attribute_remove(attributes, SHAPE_KEY_LAYER_SCALE, index, max_index);
  attribute_remove(attributes, SHAPE_KEY_LAYER_OPACITY, index, max_index);

  /* Remove and renumber shape key attributes on drawings (strokes and points). */
  MutableSpan<GreasePencilDrawingBase *> drawings = grease_pencil->drawings();
  for (GreasePencilDrawingBase *drawing_base : drawings) {
    if (drawing_base->type != GP_DRAWING) {
      continue;
    }
    bke::greasepencil::Drawing *drawing =
        &(reinterpret_cast<GreasePencilDrawing *>(drawing_base))->wrap();

    bke::CurvesGeometry &curves = drawing->strokes_for_write();
    attributes = curves.attributes_for_write();

    attribute_remove(attributes, SHAPE_KEY_STROKE_FILL_COLOR, index, max_index);
    attribute_remove(attributes, SHAPE_KEY_STROKE_FILL_OPACITY, index, max_index);
    attribute_remove(attributes, SHAPE_KEY_POINT_POS_QUATERNION, index, max_index);
    attribute_remove(attributes, SHAPE_KEY_POINT_POS_DISTANCE, index, max_index);
    attribute_remove(attributes, SHAPE_KEY_POINT_RADIUS, index, max_index);
    attribute_remove(attributes, SHAPE_KEY_POINT_OPACITY, index, max_index);
    attribute_remove(attributes, SHAPE_KEY_POINT_VERTEX_COLOR, index, max_index);
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

  /* When all shape keys are deleted, remove shape key modifiers automatically. */
  if (BLI_listbase_is_empty(&grease_pencil->shape_keys)) {
    Main *bmain = CTX_data_main(C);
    Scene *scene = CTX_data_scene(C);
    Object *object = CTX_data_active_object(C);

    LISTBASE_FOREACH_MUTABLE (ModifierData *, md, &object->modifiers) {
      if (md->type != eModifierType_GreasePencilShapeKey) {
        continue;
      }
      ed::object::modifier_remove(op->reports, bmain, scene, object, md);
    }
  }

  DEG_id_tag_update(&grease_pencil->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);

  return OPERATOR_FINISHED;
}

static int remove_all_exec(bContext *C, wmOperator *op)
{
  GreasePencil *grease_pencil = from_context(*C);
  grease_pencil->active_shape_key_index = -1;
  const int max_index = BLI_listbase_count(&grease_pencil->shape_keys) - 1;

  /* Remove shape key attributes on layers. */
  bke::MutableAttributeAccessor attributes = grease_pencil->attributes_for_write();
  for (int index = max_index; index >= 0; index--) {
    attribute_remove(attributes, SHAPE_KEY_LAYER_TRANSLATION, index, max_index);
    attribute_remove(attributes, SHAPE_KEY_LAYER_ROTATION, index, max_index);
    attribute_remove(attributes, SHAPE_KEY_LAYER_SCALE, index, max_index);
    attribute_remove(attributes, SHAPE_KEY_LAYER_OPACITY, index, max_index);
  }

  /* Remove shape key attributes on drawings (strokes and points). */
  MutableSpan<GreasePencilDrawingBase *> drawings = grease_pencil->drawings();
  for (GreasePencilDrawingBase *drawing_base : drawings) {
    if (drawing_base->type != GP_DRAWING) {
      continue;
    }
    bke::greasepencil::Drawing *drawing =
        &(reinterpret_cast<GreasePencilDrawing *>(drawing_base))->wrap();

    bke::CurvesGeometry &curves = drawing->strokes_for_write();
    attributes = curves.attributes_for_write();

    for (int index = max_index; index >= 0; index--) {
      attribute_remove(attributes, SHAPE_KEY_STROKE_FILL_COLOR, index, max_index);
      attribute_remove(attributes, SHAPE_KEY_STROKE_FILL_OPACITY, index, max_index);
      attribute_remove(attributes, SHAPE_KEY_POINT_POS_QUATERNION, index, max_index);
      attribute_remove(attributes, SHAPE_KEY_POINT_POS_DISTANCE, index, max_index);
      attribute_remove(attributes, SHAPE_KEY_POINT_RADIUS, index, max_index);
      attribute_remove(attributes, SHAPE_KEY_POINT_OPACITY, index, max_index);
      attribute_remove(attributes, SHAPE_KEY_POINT_VERTEX_COLOR, index, max_index);
    }
  }

  /* Remove animation data. */
  LISTBASE_FOREACH (GreasePencilShapeKey *, shape_key, &grease_pencil->shape_keys) {
    char name_esc[sizeof(shape_key->name) * 2];
    char rna_path[sizeof(name_esc) + 32];
    BLI_str_escape(name_esc, shape_key->name, sizeof(name_esc));
    BLI_snprintf(rna_path, sizeof(rna_path), "shape_keys[\"%s\"]", name_esc);
    BKE_animdata_fix_paths_remove(&grease_pencil->id, rna_path);
  }

  /* Delete all shape keys. */
  BLI_freelistN(&grease_pencil->shape_keys);

  /* Remove all shape key modifiers. */
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);

  LISTBASE_FOREACH_MUTABLE (ModifierData *, md, &object->modifiers) {
    if (md->type != eModifierType_GreasePencilShapeKey) {
      continue;
    }
    ed::object::modifier_remove(op->reports, bmain, scene, object, md);
  }

  DEG_id_tag_update(&grease_pencil->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);

  return OPERATOR_FINISHED;
}

static bool active_poll(bContext *C)
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
  ot->poll = active_poll;
  ot->exec = remove_exec;
}

static void GREASE_PENCIL_OT_shape_key_remove_all(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Remove All Shape Keys";
  ot->idname = "GREASE_PENCIL_OT_shape_key_remove_all";
  ot->description = "Remove alls shape keys in the Grease Pencil object";
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Callbacks. */
  ot->poll = active_poll;
  ot->exec = remove_all_exec;
}

static void attribute_move(bke::MutableAttributeAccessor attributes,
                           const StringRef shape_key_attribute,
                           const int old_index,
                           const int new_index,
                           const int max_index)
{
  std::string attribute_old = SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_attribute +
                              std::to_string(old_index);
  std::string attribute_new = SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_attribute +
                              std::to_string(new_index);
  std::string attribute_temp = SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_attribute +
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

static int move_exec(bContext *C, wmOperator *op)
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
  attribute_move(attributes, SHAPE_KEY_LAYER_TRANSLATION, old_index, new_index, max_index);
  attribute_move(attributes, SHAPE_KEY_LAYER_ROTATION, old_index, new_index, max_index);
  attribute_move(attributes, SHAPE_KEY_LAYER_SCALE, old_index, new_index, max_index);
  attribute_move(attributes, SHAPE_KEY_LAYER_OPACITY, old_index, new_index, max_index);

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

    attribute_move(attributes, SHAPE_KEY_STROKE_FILL_COLOR, old_index, new_index, max_index);
    attribute_move(attributes, SHAPE_KEY_STROKE_FILL_OPACITY, old_index, new_index, max_index);
    attribute_move(attributes, SHAPE_KEY_POINT_POS_QUATERNION, old_index, new_index, max_index);
    attribute_move(attributes, SHAPE_KEY_POINT_POS_DISTANCE, old_index, new_index, max_index);
    attribute_move(attributes, SHAPE_KEY_POINT_RADIUS, old_index, new_index, max_index);
    attribute_move(attributes, SHAPE_KEY_POINT_OPACITY, old_index, new_index, max_index);
    attribute_move(attributes, SHAPE_KEY_POINT_VERTEX_COLOR, old_index, new_index, max_index);
  }

  DEG_id_tag_update(&grease_pencil->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_shape_key_move(wmOperatorType *ot)
{
  static const EnumPropertyItem move_direction[] = {
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
  ot->poll = active_poll;
  ot->exec = move_exec;

  /* Properties. */
  RNA_def_enum(ot->srna,
               "direction",
               move_direction,
               0,
               "Direction",
               "Direction to move the active shape key (up/down)");
}

static void attributes_duplicate(bke::MutableAttributeAccessor &attributes,
                                 const StringRef shape_key_attribute,
                                 const int index_src,
                                 const int index_dst)
{
  std::string attribute_id_src = SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_attribute +
                                 std::to_string(index_src);
  std::string attribute_id_dst = SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_attribute +
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

static int duplicate_exec(bContext *C, wmOperator *op)
{
  GreasePencil *grease_pencil = from_context(*C);
  const int index_src = grease_pencil->active_shape_key_index;
  GreassePencilShapeKey *shape_key_src = BKE_grease_pencil_shape_key_active_get(grease_pencil);
  const float value_src = shape_key_src->value;

  /* Create new shape key, based on the active one. */
  if (add_exec(C, op) == OPERATOR_CANCELLED) {
    return OPERATOR_CANCELLED;
  }
  GreassePencilShapeKey *shape_key_dst = BKE_grease_pencil_shape_key_active_get(grease_pencil);
  shape_key_dst->value = value_src;
  const int index_dst = grease_pencil->active_shape_key_index;

  /* Copy shape key attributes on layers. */
  bke::MutableAttributeAccessor attributes = grease_pencil->attributes_for_write();
  attributes_duplicate(attributes, SHAPE_KEY_LAYER_TRANSLATION, index_src, index_dst);
  attributes_duplicate(attributes, SHAPE_KEY_LAYER_ROTATION, index_src, index_dst);
  attributes_duplicate(attributes, SHAPE_KEY_LAYER_SCALE, index_src, index_dst);
  attributes_duplicate(attributes, SHAPE_KEY_LAYER_OPACITY, index_src, index_dst);

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

    attributes_duplicate(attributes, SHAPE_KEY_STROKE_FILL_COLOR, index_src, index_dst);
    attributes_duplicate(attributes, SHAPE_KEY_STROKE_FILL_OPACITY, index_src, index_dst);
    attributes_duplicate(attributes, SHAPE_KEY_POINT_POS_QUATERNION, index_src, index_dst);
    attributes_duplicate(attributes, SHAPE_KEY_POINT_POS_DISTANCE, index_src, index_dst);
    attributes_duplicate(attributes, SHAPE_KEY_POINT_RADIUS, index_src, index_dst);
    attributes_duplicate(attributes, SHAPE_KEY_POINT_OPACITY, index_src, index_dst);
    attributes_duplicate(attributes, SHAPE_KEY_POINT_VERTEX_COLOR, index_src, index_dst);
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
  ot->poll = active_poll;
  ot->exec = duplicate_exec;

  /* Properties. */
  PropertyRNA *prop;
  prop = RNA_def_string(ot->srna, "name", nullptr, MAX_NAME, "Name", "Name of the new shape key");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  ot->prop = prop;
}

static void restore_base_layers(ShapeKeyEditData &edit_data)
{
  using namespace bke::greasepencil;

  /* Restore layer properties possibly affected by shape key. */
  for (Layer *layer : edit_data.grease_pencil->layers_for_write()) {
    if (layer->runtime->shape_key_edit_index == 0) {
      continue;
    }
    const int base_layer_index = layer->runtime->shape_key_edit_index - 1;
    const LayerBase &base_layer = edit_data.base_layers[base_layer_index];
    copy_v3_v3(layer->translation, base_layer.translation);
    copy_v3_v3(layer->rotation, base_layer.rotation);
    copy_v3_v3(layer->scale, base_layer.scale);
    layer->opacity = base_layer.opactity;
  }
}

static void remove_stroke_index_attributes(ShapeKeyEditData &edit_data)
{
  using namespace bke::greasepencil;

  for (const int drawing_i : edit_data.grease_pencil->drawings().index_range()) {
    GreasePencilDrawingBase *drawing_base = edit_data.grease_pencil->drawing(drawing_i);
    Drawing &drawing = (reinterpret_cast<GreasePencilDrawing *>(drawing_base))->wrap();
    if (drawing_base->type != GP_DRAWING) {
      continue;
    }

    bke::MutableAttributeAccessor attributes = drawing.strokes_for_write().attributes_for_write();
    attributes.remove(SHAPE_KEY_BASE_STROKE_INDEX);
  }
}

static void edit_exit(bContext *C, wmOperator *op)
{
  ShapeKeyEditData *edit_data = static_cast<ShapeKeyEditData *>(op->customdata);

  /* Shape key is no longer in edit mode. */
  in_edit_mode = false;

  if (edit_data == nullptr) {
    return;
  }

  /* Restore base layers. */
  restore_base_layers(*edit_data);

  /* Remove temporary stroke index attributes. */
  remove_stroke_index_attributes(*edit_data);

  /* Clear edit state of shape key in shape key modifiers. */
  Object *object = CTX_data_active_object(C);
  LISTBASE_FOREACH (ModifierData *, md, &object->modifiers) {
    if (md->type != eModifierType_GreasePencilShapeKey) {
      continue;
    }
    auto &skd = *reinterpret_cast<GreasePencilShapeKeyModifierData *>(md);
    skd.flag &= ~MOD_GREASE_PENCIL_SHAPE_KEY_IN_EDIT_MODE;
    skd.index_edited = -1;
    skd.shape_key_edit_data = nullptr;
  }

  /* Remove viewport draw handler. */
  if (edit_data->draw_handle) {
    ED_region_draw_cb_exit(edit_data->region->runtime->type, edit_data->draw_handle);
  }

  DEG_id_tag_update(&edit_data->grease_pencil->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, edit_data->grease_pencil);

  /* Remove operator data. */
  MEM_delete(edit_data);
  op->customdata = nullptr;
}

static void edit_viewport_draw(const bContext * /*C*/, ARegion *region, void *arg)
{
  ShapeKeyEditData &edit_data = *static_cast<ShapeKeyEditData *>(arg);

  /* Draw only in the region set by the operator. */
  if (region != edit_data.region) {
    return;
  }

  /* Draw rectangle outline. */
  float half_line_w = 3.0f * UI_SCALE_FAC;
  rcti *rect = &region->winrct;
  float color[4];
  UI_GetThemeColor4fv(TH_SELECT_ACTIVE, color);
  GPUVertFormat *format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(format, "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  immUniformColor4fv(color);
  GPU_line_width(2 * half_line_w);
  imm_draw_box_wire_2d(pos,
                       half_line_w,
                       half_line_w,
                       rect->xmax - rect->xmin - edit_data.npanel_width - half_line_w,
                       rect->ymax - rect->ymin - edit_data.header_height - 2);
  immUnbindProgram();

  /* Draw text. */
  const int font_id = BLF_default();
  const uiStyle *style = UI_style_get();
  BLF_size(font_id, style->widget.points * UI_SCALE_FAC);
  BLF_color4fv(font_id, color);
  BLF_enable(font_id, BLF_SHADOW);
  BLF_shadow(font_id, FontShadowType::Outline, blender::float4{0.0f, 0.0f, 0.0f, 0.3f});
  BLF_shadow_offset(font_id, 1, -1);
  const char *text;
  text = TIP_("Editing Shape Key");
  float x = (rect->xmax - rect->xmin - edit_data.npanel_width) * 0.5f -
            BLF_width(font_id, text, strlen(text)) * 0.5f;
  float y = rect->ymax - rect->ymin - edit_data.header_height -
            style->widget.points * UI_SCALE_FAC - half_line_w * 3;
  BLF_position(font_id, x, y, 0);
  BLF_draw(font_id, text, strlen(text));
  BLF_disable(font_id, BLF_SHADOW);
}

bool apply_shape_key_to_drawing(bke::greasepencil::Drawing &drawing,
                                const StringRef shape_key_id,
                                const IndexMask &stroke_mask,
                                const float factor)
{
  using namespace blender::bke;

  CurvesGeometry &curves = drawing.strokes_for_write();
  if (curves.is_empty()) {
    return false;
  }
  MutableAttributeAccessor attributes = curves.attributes_for_write();
  const OffsetIndices points_by_curve = curves.points_by_curve();
  const VArray<int> base_stroke_indices = *attributes.lookup_or_default<int>(
      SHAPE_KEY_BASE_STROKE_INDEX, AttrDomain::Curve, 0);
  const VArray<bool> cyclic = curves.cyclic();

  /* Get shape key attributes. */
  MutableSpan<ColorGeometry4f> fill_colors;
  MutableSpan<float> fill_opacities;
  MutableSpan<float> radii;
  MutableSpan<float> opacities;
  MutableSpan<ColorGeometry4f> vertex_colors;
  AttributeReader<math::Quaternion> position_quaternions;
  MutableSpan<float3> positions;

  const AttributeReader fill_color_deltas = attributes.lookup<ColorGeometry4f>(
      SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_STROKE_FILL_COLOR + shape_key_id, AttrDomain::Curve);
  const AttributeReader fill_opacity_deltas = attributes.lookup<float>(
      SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_STROKE_FILL_OPACITY + shape_key_id,
      AttrDomain::Curve);
  const AttributeReader radius_deltas = attributes.lookup<float>(
      SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_POINT_RADIUS + shape_key_id, AttrDomain::Point);
  const AttributeReader opacity_deltas = attributes.lookup<float>(
      SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_POINT_OPACITY + shape_key_id, AttrDomain::Point);
  const AttributeReader vertex_color_deltas = attributes.lookup<ColorGeometry4f>(
      SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_POINT_VERTEX_COLOR + shape_key_id, AttrDomain::Point);
  const AttributeReader position_distance = attributes.lookup<float>(
      SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_POINT_POS_DISTANCE + shape_key_id, AttrDomain::Point);

  const bool has_fill_color = fill_color_deltas && !fill_color_deltas.varray.is_empty();
  const bool has_fill_opacity = fill_opacity_deltas && !fill_opacity_deltas.varray.is_empty();
  const bool has_radius = radius_deltas && !radius_deltas.varray.is_empty();
  const bool has_opacity = opacity_deltas && !opacity_deltas.varray.is_empty();
  const bool has_vertex_color = vertex_color_deltas && !vertex_color_deltas.varray.is_empty();
  const bool has_distance = position_distance && !position_distance.varray.is_empty();

  if (has_fill_color) {
    fill_colors = drawing.fill_colors_for_write();
  }
  if (has_fill_opacity) {
    fill_opacities = drawing.fill_opacities_for_write();
  }
  if (has_radius) {
    radii = drawing.radii_for_write();
  }
  if (has_opacity) {
    opacities = drawing.opacities_for_write();
  }
  if (has_vertex_color) {
    vertex_colors = drawing.vertex_colors_for_write();
  }
  if (has_distance) {
    positions = curves.positions_for_write();
    position_quaternions = attributes.lookup<math::Quaternion>(
        SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_POINT_POS_QUATERNION + shape_key_id,
        AttrDomain::Point);
  }

  /* Apply shape key to strokes and points. */
  stroke_mask.foreach_index(GrainSize(512), [&](const int stroke) {
    /* Shape key: stroke fill color. */
    if (has_fill_color) {
      add_v4_v4(fill_colors[stroke], float4(fill_color_deltas.varray[stroke]) * factor);
      clamp_v4(fill_colors[stroke], 0.0f, 1.0f);
    }

    /* Shape key: stroke fill opacity. */
    if (has_fill_opacity) {
      fill_opacities[stroke] = math::clamp(
          fill_opacities[stroke] + fill_opacity_deltas.varray[stroke] * factor, 0.0f, 1.0f);
    }

    const IndexRange points = points_by_curve[stroke];
    for (const int point : points) {
      /* Shape key: point radius. */
      if (has_radius) {
        radii[point] = math::max(radii[point] + radius_deltas.varray[point] * factor, 0.0f);
      }
      /* Shape key: point opacity. */
      if (has_opacity) {
        opacities[point] = math::clamp(
            opacities[point] + opacity_deltas.varray[point] * factor, 0.0f, 1.0f);
      }
      /* Shape key: vertex colors. */
      if (has_vertex_color) {
        add_v4_v4(vertex_colors[point], float4(vertex_color_deltas.varray[point]) * factor);
        clamp_v4(vertex_colors[point], 0.0f, 1.0f);
      }
    }

    /* Shape key: point position. */
    if (has_distance) {
      float3 vector_to_next_point;
      for (const int point : points) {
        if (position_distance.varray[point] == 0.0f) {
          continue;
        }

        /* Convert quaternion rotation and distance to a point position delta. */
        float matrix[3][3];
        const math::Quaternion quaternion = position_quaternions.varray[point];
        quat_to_mat3(matrix, &quaternion.w);
        if (point < points.last() || cyclic[stroke]) {
          const int next_point = (point == points.last()) ? points.first() : point + 1;
          vector_to_next_point = positions[next_point] - positions[point];
          mul_m3_v3(matrix, vector_to_next_point);
          normalize_v3(vector_to_next_point);
        }
        else if (points.size() == 1) {
          vector_to_next_point = {1.0f, 0.0f, 0.0f};
          mul_m3_v3(matrix, vector_to_next_point);
          normalize_v3(vector_to_next_point);
        }

        /* Apply the delta to the point position. */
        float3 position_delta = vector_to_next_point * (position_distance.varray[point] * factor);
        positions[point] += position_delta;
      }
    }
  });

  return has_distance;
}

void apply_shape_key_to_layers(GreasePencil &grease_pencil,
                               const StringRef shape_key_id,
                               const IndexMask &layer_mask,
                               const float factor)
{
  bke::AttributeAccessor layer_attributes = grease_pencil.attributes();
  const VArray<float3> shape_key_translations = *layer_attributes.lookup_or_default<float3>(
      SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_LAYER_TRANSLATION + shape_key_id,
      bke::AttrDomain::Layer,
      float3(0.0f, 0.0f, 0.0f));
  const VArray<float3> shape_key_rotations = *layer_attributes.lookup_or_default<float3>(
      SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_LAYER_ROTATION + shape_key_id,
      bke::AttrDomain::Layer,
      float3(0.0f, 0.0f, 0.0f));
  const VArray<float3> shape_key_scales = *layer_attributes.lookup_or_default<float3>(
      SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_LAYER_SCALE + shape_key_id,
      bke::AttrDomain::Layer,
      float3(0.0f, 0.0f, 0.0f));
  const VArray<float> shape_key_opacities = *layer_attributes.lookup_or_default<float>(
      SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_LAYER_OPACITY + shape_key_id,
      bke::AttrDomain::Layer,
      0.0f);

  layer_mask.foreach_index([&](const int layer_i) {
    bke::greasepencil::Layer &layer = grease_pencil.layer(layer_i);

    copy_v3_v3(layer.translation,
               float3(layer.translation) + shape_key_translations[layer_i] * factor);
    copy_v3_v3(layer.rotation, float3(layer.rotation) + shape_key_rotations[layer_i] * factor);
    copy_v3_v3(layer.scale, float3(layer.scale) + shape_key_scales[layer_i] * factor);
    layer.opacity = math::clamp(layer.opacity + shape_key_opacities[layer_i] * factor, 0.0f, 1.0f);
  });
}

void edit_get_shape_key_stroke_deltas(ShapeKeyEditData &edit_data,
                                      const Span<bke::greasepencil::Drawing *> drawings)
{
  using namespace bke;
  using namespace bke::greasepencil;

  const std::string shape_key_id = std::to_string(edit_data.edited_shape_key_index);

  threading::parallel_for(drawings.index_range(), 1, [&](const IndexRange drawing_range) {
    for (const int drawing_i : drawing_range) {
      Drawing &drawing = *drawings[drawing_i];
      if (drawing.runtime->shape_key_edit_index == 0) {
        continue;
      }

      /* Get edited and base geometry. */
      CurvesGeometry &curves = drawing.strokes_for_write();
      CurvesGeometry &base_curves =
          edit_data.base_geometry[drawing.runtime->shape_key_edit_index - 1];
      MutableAttributeAccessor attributes = curves.attributes_for_write();
      AttributeAccessor base_attributes = base_curves.attributes();
      const OffsetIndices points_by_curve = curves.points_by_curve();
      const OffsetIndices base_points_by_curve = base_curves.points_by_curve();
      VArray<int> base_stroke_indices = *attributes.lookup_or_default<int>(
          SHAPE_KEY_BASE_STROKE_INDEX, AttrDomain::Curve, 0);
      const VArray<bool> cyclic = curves.cyclic();

      Array<ColorGeometry4f> fill_color_deltas(curves.curves_num(),
                                               ColorGeometry4f(0.0f, 0.0f, 0.0f, 0.0f));
      MutableSpan<ColorGeometry4f> fill_colors = drawing.fill_colors_for_write();
      const VArray<ColorGeometry4f> base_fill_colors =
          *base_attributes.lookup_or_default<ColorGeometry4f>(
              "fill_color", AttrDomain::Curve, ColorGeometry4f(0.0f, 0.0f, 0.0f, 0.0f));
      bool fill_color_has_delta = false;

      Array<float> fill_opacity_deltas(curves.curves_num(), 0.0f);
      MutableSpan<float> fill_opacities = drawing.fill_opacities_for_write();
      const VArray<float> base_fill_opacities = *base_attributes.lookup_or_default<float>(
          "fill_opacity", AttrDomain::Curve, 1.0f);
      bool fill_opacity_has_delta = false;

      Array<math::Quaternion> quaternion_deltas(curves.points_num(), math::Quaternion::identity());
      Array<float> distance_deltas(curves.points_num(), 0.0f);
      MutableSpan<float3> positions = curves.positions_for_write();
      const Span<float3> base_positions = base_curves.positions();
      bool distance_has_delta = false;

      Array<float> radius_deltas(curves.points_num(), 0.0f);
      MutableSpan<float> radii = drawing.radii_for_write();
      const VArray<float> base_radii = *base_attributes.lookup_or_default(
          "radius", AttrDomain::Point, 0.01f);
      bool radius_has_delta = false;

      Array<float> opacity_deltas(curves.points_num(), 0.0f);
      MutableSpan<float> opacities = drawing.opacities_for_write();
      const VArray<float> base_opacities = *base_attributes.lookup_or_default(
          "opacity", AttrDomain::Point, 1.0f);
      bool opacity_has_delta = false;

      Array<ColorGeometry4f> vertex_color_deltas(curves.points_num(),
                                                 ColorGeometry4f(0.0f, 0.0f, 0.0f, 0.0f));
      MutableSpan<ColorGeometry4f> vertex_colors = drawing.vertex_colors_for_write();
      const VArray<ColorGeometry4f> base_vertex_colors =
          *base_attributes.lookup_or_default<ColorGeometry4f>(
              "vertex_color", AttrDomain::Point, ColorGeometry4f(0.0f, 0.0f, 0.0f, 0.0f));
      bool vertex_color_has_delta = false;

      /* Loop over edited strokes and look for changed shape key properties. We restore
       * shape-keyed properties to their base values, because the shape key modifier(s) will
       * take care of applying the shape key deltas on top of the base values.
       * The advantage of this approach is that the user can determine the order of modifiers
       * (e.g. a shape key applied after an armature deform). */
      for (const int stroke : curves.curves_range()) {
        const int base_stroke = base_stroke_indices[stroke] - 1;
        /* Skip strokes without base reference. */
        if (base_stroke == -1) {
          continue;
        }

        /* When the number of points in the edited stroke and the base stroke don't match, it's
         * difficult to create a shape key. For now, we ignore non-matching strokes.
         * In the future we could implement a smarter algorithm for matching the points. */
        if (points_by_curve[stroke].size() != base_points_by_curve[base_stroke].size()) {
          continue;
        }

        /* Store delta of stroke fill color. */
        float4 color_delta = float4(fill_colors[stroke]) - float4(base_fill_colors[base_stroke]);
        if (!math::is_zero(color_delta, EPSILON)) {
          fill_color_has_delta = true;
          fill_color_deltas[stroke] = ColorGeometry4f(color_delta);
        }
        /* Restore to base value. */
        fill_colors[stroke] = base_fill_colors[base_stroke];

        /* Store delta of stroke fill opacity. */
        float delta = fill_opacities[stroke] - base_fill_opacities[base_stroke];
        if (math::abs(delta) > EPSILON) {
          fill_opacity_has_delta = true;
          fill_opacity_deltas[stroke] = delta;
        }
        /* Restore to base value. */
        fill_opacities[stroke] = base_fill_opacities[base_stroke];

        /* Get stroke point deltas. */
        const IndexRange points = points_by_curve[stroke];
        float3 vector_to_next_point;
        for (const int point : points) {
          /* Get quaternion rotation and distance between base point and shape-keyed point. */
          if (positions[point] != base_positions[point]) {
            float3 shaped_point_vector = positions[point] - base_positions[point];
            const float distance = math::length(shaped_point_vector);
            if (distance > EPSILON) {
              if (point < points.last() || cyclic[stroke]) {
                const int next_point = (point == points.last()) ? points.first() : point + 1;
                vector_to_next_point = math::normalize(base_positions[next_point] -
                                                       base_positions[point]);
              }
              else if (points.size() == 1) {
                vector_to_next_point = {1.0f, 0.0f, 0.0f};
              }
              shaped_point_vector = math::normalize(shaped_point_vector);
              float4 quaternion;
              rotation_between_vecs_to_quat(quaternion, vector_to_next_point, shaped_point_vector);

              distance_has_delta = true;
              distance_deltas[point] = distance;
              quaternion_deltas[point] = math::Quaternion(quaternion);
            }
            /* Restore to base value. */
            positions[point] = base_positions[point];
          }

          /* Get radius delta. */
          delta = radii[point] - base_radii[point];
          if (math::abs(delta) > EPSILON) {
            radius_has_delta = true;
            radius_deltas[point] = delta;
          }
          /* Restore to base value. */
          radii[point] = base_radii[point];

          /* Get opacity delta. */
          delta = opacities[point] - base_opacities[point];
          if (math::abs(delta) > EPSILON) {
            opacity_has_delta = true;
            opacity_deltas[point] = delta;
          }
          /* Restore to base value. */
          opacities[point] = base_opacities[point];

          /* Get vertex color delta. */
          color_delta = float4(vertex_colors[point]) - float4(base_vertex_colors[point]);
          if (!math::is_zero(color_delta, EPSILON)) {
            vertex_color_has_delta = true;
            vertex_color_deltas[point] = ColorGeometry4f(color_delta);
          }
          /* Restore to base value. */
          vertex_colors[point] = base_vertex_colors[point];
        }
      }

      /* Store stroke and point attributes for the edited shape key. */
      if (fill_color_has_delta) {
        SpanAttributeWriter attr_deltas = attributes.lookup_or_add_for_write_span<ColorGeometry4f>(
            SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_STROKE_FILL_COLOR + shape_key_id,
            AttrDomain::Curve);
        attr_deltas.span.copy_from(fill_color_deltas);
        attr_deltas.finish();
      }
      else {
        attributes.remove(SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_STROKE_FILL_COLOR + shape_key_id);
      }
      if (fill_opacity_has_delta) {
        SpanAttributeWriter attr_deltas = attributes.lookup_or_add_for_write_span<float>(
            SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_STROKE_FILL_OPACITY + shape_key_id,
            AttrDomain::Curve);
        attr_deltas.span.copy_from(fill_opacity_deltas);
        attr_deltas.finish();
      }
      else {
        attributes.remove(SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_STROKE_FILL_OPACITY +
                          shape_key_id);
      }
      if (distance_has_delta) {
        SpanAttributeWriter attr_deltas = attributes.lookup_or_add_for_write_span<float>(
            SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_POINT_POS_DISTANCE + shape_key_id,
            AttrDomain::Point);
        attr_deltas.span.copy_from(distance_deltas);
        attr_deltas.finish();
        SpanAttributeWriter attr_deltas1 =
            attributes.lookup_or_add_for_write_span<math::Quaternion>(
                SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_POINT_POS_QUATERNION + shape_key_id,
                AttrDomain::Point);
        attr_deltas1.span.copy_from(quaternion_deltas);
        attr_deltas1.finish();

        drawing.tag_positions_changed();
      }
      else {
        attributes.remove(SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_POINT_POS_DISTANCE +
                          shape_key_id);
        attributes.remove(SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_POINT_POS_QUATERNION +
                          shape_key_id);
      }
      if (radius_has_delta) {
        SpanAttributeWriter attr_deltas = attributes.lookup_or_add_for_write_span<float>(
            SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_POINT_RADIUS + shape_key_id, AttrDomain::Point);
        attr_deltas.span.copy_from(radius_deltas);
        attr_deltas.finish();
      }
      else {
        attributes.remove(SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_POINT_RADIUS + shape_key_id);
      }
      if (opacity_has_delta) {
        SpanAttributeWriter attr_deltas = attributes.lookup_or_add_for_write_span<float>(
            SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_POINT_OPACITY + shape_key_id,
            AttrDomain::Point);
        attr_deltas.span.copy_from(opacity_deltas);
        attr_deltas.finish();
      }
      else {
        attributes.remove(SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_POINT_OPACITY + shape_key_id);
      }
      if (vertex_color_has_delta) {
        SpanAttributeWriter attr_deltas = attributes.lookup_or_add_for_write_span<ColorGeometry4f>(
            SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_POINT_VERTEX_COLOR + shape_key_id,
            AttrDomain::Point);
        attr_deltas.span.copy_from(vertex_color_deltas);
        attr_deltas.finish();
      }
      else {
        attributes.remove(SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_POINT_VERTEX_COLOR +
                          shape_key_id);
      }
    }
  });
}

static void edit_get_shape_key_deltas(const wmOperator *op)
{
  using namespace bke;
  using namespace bke::greasepencil;

  ShapeKeyEditData &edit_data = *static_cast<ShapeKeyEditData *>(op->customdata);
  const std::string shape_key_id = std::to_string(edit_data.edited_shape_key_index);
  GreasePencil &grease_pencil = *edit_data.grease_pencil;
  MutableAttributeAccessor layer_attributes = grease_pencil.attributes_for_write();

  /* Get layer deltas for the edited shape key. */
  Array<float3> translation_deltas(grease_pencil.layers().size(), float3(0.0f, 0.0f, 0.0f));
  Array<float3> rotation_deltas(grease_pencil.layers().size(), float3(0.0f, 0.0f, 0.0f));
  Array<float3> scale_deltas(grease_pencil.layers().size(), float3(0.0f, 0.0f, 0.0f));
  Array<float> opacity_deltas(grease_pencil.layers().size(), 0.0f);
  bool translation_has_delta = false;
  bool rotation_has_delta = false;
  bool scale_has_delta = false;
  bool opacity_has_delta = false;

  for (const int layer_i : grease_pencil.layers().index_range()) {
    const Layer &layer = grease_pencil.layer(layer_i);

    /* Skip when base layer is missing. */
    if (layer.runtime->shape_key_edit_index == 0) {
      continue;
    }

    /* Compare edited layer with base layer. */
    const LayerBase &base_layer = edit_data.base_layers[layer.runtime->shape_key_edit_index - 1];
    const float3 translation_delta = float3(layer.translation) - base_layer.translation;
    const float3 rotation_delta = float3(layer.rotation) - base_layer.rotation;
    const float3 scale_delta = float3(layer.scale) - base_layer.scale;
    const float opacity_delta = layer.opacity - base_layer.opactity;

    if (!math::is_zero(translation_delta, EPSILON)) {
      translation_has_delta = true;
      translation_deltas[layer_i] = translation_delta;
    }
    if (!math::is_zero(rotation_delta, EPSILON)) {
      rotation_has_delta = true;
      rotation_deltas[layer_i] = rotation_delta;
    }
    if (!math::is_zero(scale_delta, EPSILON)) {
      scale_has_delta = true;
      scale_deltas[layer_i] = scale_delta;
    }
    if (math::abs(opacity_delta) > EPSILON) {
      opacity_has_delta = true;
      opacity_deltas[layer_i] = opacity_delta;
    }
  }

  /* Store layer attributes for the edited shape key. */
  if (translation_has_delta) {
    SpanAttributeWriter attr_deltas = layer_attributes.lookup_or_add_for_write_span<float3>(
        SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_LAYER_TRANSLATION + shape_key_id,
        AttrDomain::Layer);
    attr_deltas.span.copy_from(translation_deltas);
    attr_deltas.finish();
  }
  else {
    layer_attributes.remove(SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_LAYER_TRANSLATION +
                            shape_key_id);
  }
  if (rotation_has_delta) {
    SpanAttributeWriter attr_deltas = layer_attributes.lookup_or_add_for_write_span<float3>(
        SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_LAYER_ROTATION + shape_key_id, AttrDomain::Layer);
    attr_deltas.span.copy_from(rotation_deltas);
    attr_deltas.finish();
  }
  else {
    layer_attributes.remove(SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_LAYER_ROTATION + shape_key_id);
  }
  if (scale_has_delta) {
    SpanAttributeWriter attr_deltas = layer_attributes.lookup_or_add_for_write_span<float3>(
        SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_LAYER_SCALE + shape_key_id, AttrDomain::Layer);
    attr_deltas.span.copy_from(scale_deltas);
    attr_deltas.finish();
  }
  else {
    layer_attributes.remove(SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_LAYER_SCALE + shape_key_id);
  }
  if (opacity_has_delta) {
    SpanAttributeWriter attr_deltas = layer_attributes.lookup_or_add_for_write_span<float>(
        SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_LAYER_OPACITY + shape_key_id, AttrDomain::Layer);
    attr_deltas.span.copy_from(opacity_deltas);
    attr_deltas.finish();
  }
  else {
    layer_attributes.remove(SHAPE_KEY_ATTRIBUTE_PREFIX + SHAPE_KEY_LAYER_OPACITY + shape_key_id);
  }

  /* Get deltas in geometry for the edited shape key. */
  Vector<Drawing *> drawings;
  for (const int drawing_i : grease_pencil.drawings().index_range()) {
    GreasePencilDrawingBase *drawing_base = grease_pencil.drawing(drawing_i);
    if (drawing_base->type != GP_DRAWING) {
      continue;
    }
    Drawing *drawing = &(reinterpret_cast<GreasePencilDrawing *>(drawing_base))->wrap();
    drawings.append(drawing);
  }

  edit_get_shape_key_stroke_deltas(edit_data, drawings);
}

static void edit_init(bContext *C, wmOperator *op)
{
  using namespace bke::greasepencil;

  GreasePencil &grease_pencil = *from_context(*C);

  /* Create operator data. */
  ShapeKeyEditData &edit_data = *MEM_new<ShapeKeyEditData>(__func__);
  op->customdata = &edit_data;
  edit_data.grease_pencil = &grease_pencil;
  edit_data.edited_shape_key_index = grease_pencil.active_shape_key_index;
  edit_data.edited_shape_key = BKE_grease_pencil_shape_key_active_get(&grease_pencil);

  /* Set flag now we enter edit mode. */
  in_edit_mode = true;

  /* Mark the edited shape key in the shape key modifiers. */
  Object *object = CTX_data_active_object(C);
  bool is_first = true;
  LISTBASE_FOREACH (ModifierData *, md, &object->modifiers) {
    if (md->type != eModifierType_GreasePencilShapeKey) {
      continue;
    }
    auto &skd = *reinterpret_cast<GreasePencilShapeKeyModifierData *>(md);
    skd.flag |= MOD_GREASE_PENCIL_SHAPE_KEY_IN_EDIT_MODE;
    skd.index_edited = grease_pencil.active_shape_key_index;
    skd.shape_key_edit_data = is_first ? &edit_data : nullptr;
    is_first = false;
  }
  /* Get largest 3D viewport in all windows. */
  edit_data.area = nullptr;
  edit_data.region = nullptr;
  edit_data.header_height = 0;
  edit_data.npanel_width = 0;
  int max_width = 0;
  const wmWindowManager *wm = CTX_wm_manager(C);
  LISTBASE_FOREACH (wmWindow *, window, &wm->windows) {
    bScreen *screen = WM_window_get_active_screen(window);
    LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
      if (area->spacetype == SPACE_VIEW3D) {
        int width = area->totrct.xmax - area->totrct.xmin;
        if (width > max_width) {
          edit_data.area = area;
          max_width = width;
        }
      }
    }
  }
  if (edit_data.area) {
    LISTBASE_FOREACH (ARegion *, region, &edit_data.area->regionbase) {
      if (region->regiontype == RGN_TYPE_WINDOW) {
        edit_data.region = region;
      }
      if (region->alignment == RGN_ALIGN_TOP &&
          ELEM(region->regiontype, RGN_TYPE_TOOL_HEADER, RGN_TYPE_HEADER))
      {
        edit_data.header_height += int(region->sizey * UI_SCALE_FAC + 0.5f);
      }
      if (region->alignment == RGN_ALIGN_RIGHT && region->regiontype == RGN_TYPE_UI) {
        edit_data.npanel_width = region->sizex > 0 ? 20 * UI_SCALE_FAC : 0;
      }
    }
  }

  /* Add draw handler to viewport for colored rectangle (marking 'edit mode'). */
  if (edit_data.region) {
    edit_data.draw_handle = ED_region_draw_cb_activate(
        edit_data.region->runtime->type, edit_viewport_draw, &edit_data, REGION_DRAW_POST_PIXEL);
    ED_region_tag_redraw(edit_data.region);
  }

  /* Store relevant shape key data of base layers: translation, rotation, scale and opacity. */
  edit_data.base_layers.reinitialize(grease_pencil.layers().size());
  for (const int layer_i : grease_pencil.layers().index_range()) {
    Layer &layer = grease_pencil.layer(layer_i);
    LayerBase layer_base{};
    layer_base.translation = layer.translation;
    layer_base.rotation = layer.rotation;
    layer_base.scale = layer.scale;
    layer_base.opactity = layer.opacity;

    /* Store the base layer and an index reference on the layer with the applied shape key. */
    edit_data.base_layers[layer_i] = layer_base;
    layer.runtime->shape_key_edit_index = layer_i + 1;
  }

  /* Apply the edited shape key to the layers. During edit, the shape key changes to layers must be
   * visible in the UI (layer transformation and opacity), so we apply them manually (and not by
   * the shape key modifier). */
  IndexMask all_layers(IndexRange(grease_pencil.layers().size()));
  const std::string shape_key_id = std::to_string(edit_data.edited_shape_key_index);
  apply_shape_key_to_layers(grease_pencil, shape_key_id, all_layers, 1.0f);

  /* Store shape key data of all base drawings. */
  edit_data.base_geometry.reinitialize(grease_pencil.drawings().size());
  threading::parallel_for(
      grease_pencil.drawings().index_range(), 1, [&](const IndexRange drawing_range) {
        for (const int drawing_i : drawing_range) {
          GreasePencilDrawingBase *drawing_base = grease_pencil.drawing(drawing_i);
          Drawing &drawing = (reinterpret_cast<GreasePencilDrawing *>(drawing_base))->wrap();
          if (drawing_base->type != GP_DRAWING) {
            drawing.runtime->shape_key_edit_index = 0;
            continue;
          }

          /* Store the base geometry (a full copy), so we can compute deltas when we finish
           * editing. */
          edit_data.base_geometry[drawing_i] = drawing.strokes();
          drawing.runtime->shape_key_edit_index = drawing_i + 1;

          /* Mark all strokes with an index, so we can map them to the base strokes. */
          bke::MutableAttributeAccessor attributes =
              drawing.strokes_for_write().attributes_for_write();
          IndexMask stroke_mask(IndexRange(1, drawing.strokes().curves_num()));
          Array<int> stroke_indices(drawing.strokes().curves_num());
          stroke_mask.to_indices(stroke_indices.as_mutable_span());
          bke::SpanAttributeWriter base_stroke_indices =
              attributes.lookup_or_add_for_write_span<int>(SHAPE_KEY_BASE_STROKE_INDEX,
                                                           bke::AttrDomain::Curve);
          base_stroke_indices.span.copy_from(stroke_indices);
          base_stroke_indices.finish();

          /* Apply the edited shape key to the drawing, so we can measure deltas when finishing.
           */
          IndexMask all_strokes(IndexRange(drawing.strokes().curves_num()));
          apply_shape_key_to_drawing(drawing, shape_key_id, all_strokes, 1.0f);
        }
      });

  DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
}

static int edit_modal(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  /* Operator ends when the 'in edit mode' flag is disabled by the Finish Edit operator. */
  if (!in_edit_mode) {
    edit_get_shape_key_deltas(op);
    edit_exit(C, op);
    return OPERATOR_FINISHED;
  }

  return OPERATOR_PASS_THROUGH;
}

static int edit_exec(bContext *C, wmOperator *op)
{
  /* Initialize the shape key edit mode. */
  edit_init(C, op);

  /* Add an extra undo step, otherwise the applied shape key can be undone too easily by the
   * user, resulting in the shape key being gone up in smoke. */
  ED_undo_push_op(C, op);

  /* Add a modal handler for this operator. */
  WM_event_add_modal_handler(C, op);

  return OPERATOR_RUNNING_MODAL;
}

static void GREASE_PENCIL_OT_shape_key_edit(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Edit Shape Key";
  ot->idname = "GREASE_PENCIL_OT_shape_key_edit";
  ot->description = "Edit the active shape key";
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Callbacks. */
  ot->poll = active_poll;
  ot->exec = edit_exec;
  ot->modal = edit_modal;
  ot->cancel = edit_exit;
}

static int edit_finish_exec(bContext * /*C*/, wmOperator * /*op*/)
{
  in_edit_mode = false;
  return OPERATOR_FINISHED;
}

static bool edit_finish_poll(bContext * /*C*/)
{
  return in_edit_mode;
}

static void GREASE_PENCIL_OT_shape_key_edit_finish(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Finish Edit Shape Key";
  ot->idname = "GREASE_PENCIL_OT_shape_key_edit_finish";
  ot->description = "Finish the editing of the active shape key";
  ot->flag = OPTYPE_REGISTER;

  /* Callbacks. */
  ot->poll = edit_finish_poll;
  ot->exec = edit_finish_exec;
}

// static void GREASE_PENCIL_OT_shape_key_apply_all(wmOperatorType *ot) {}

}  // namespace blender::ed::greasepencil::shape_key

bool ED_grease_pencil_shape_key_in_edit_mode()
{
  return blender::ed::greasepencil::shape_key::in_edit_mode;
}

void ED_operatortypes_grease_pencil_shape_keys()
{
  using namespace blender::ed::greasepencil::shape_key;
  WM_operatortype_append(GREASE_PENCIL_OT_shape_key_add);
  WM_operatortype_append(GREASE_PENCIL_OT_shape_key_remove);
  WM_operatortype_append(GREASE_PENCIL_OT_shape_key_move);
  WM_operatortype_append(GREASE_PENCIL_OT_shape_key_duplicate);
  WM_operatortype_append(GREASE_PENCIL_OT_shape_key_edit);
  WM_operatortype_append(GREASE_PENCIL_OT_shape_key_edit_finish);
  WM_operatortype_append(GREASE_PENCIL_OT_shape_key_remove_all);
  // TODO: WM_operatortype_append(GREASE_PENCIL_OT_shape_key_apply_all);
}
