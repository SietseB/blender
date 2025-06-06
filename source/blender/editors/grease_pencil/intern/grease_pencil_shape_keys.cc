/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edgreasepencil
 */

#include "BKE_animsys.h"
#include "BKE_context.hh"
#include "BKE_geometry_set.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_modifier.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"
#include "BKE_screen.hh"

#include "BLI_listbase.h"
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
constexpr StringRef SHAPE_KEY_LAYER_TRANSLATION = "-translation";
constexpr StringRef SHAPE_KEY_LAYER_ROTATION = "-rotation";
constexpr StringRef SHAPE_KEY_LAYER_SCALE = "-scale";
constexpr StringRef SHAPE_KEY_LAYER_OPACITY = "-opacity";
constexpr StringRef SHAPE_KEY_STROKE_FILL_COLOR = "-fill-color";
constexpr StringRef SHAPE_KEY_STROKE_FILL_OPACITY = "-fill-opacity";
constexpr StringRef SHAPE_KEY_POINT_POS_DISTANCE = "-pos-distance";
constexpr StringRef SHAPE_KEY_POINT_POS_ANGLE = "-pos-angle";
constexpr StringRef SHAPE_KEY_POINT_RADIUS = "-radius";
constexpr StringRef SHAPE_KEY_POINT_OPACITY = "-opacity";
constexpr StringRef SHAPE_KEY_POINT_VERTEX_COLOR = "-vertex-color";

/* State flag: is a shape key being edited? */
enum class ShapeKeyEditState : int8_t {
  Inactive,
  Active,
  Cancelled,
};
ShapeKeyEditState edit_state = ShapeKeyEditState::Inactive;

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
  GreasePencil *grease_pencil;
  int edited_shape_key_index;

  ARegionType *region_type;
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

/* Change shape key attribute 'sk-<n>-...' to 'sk-<n+1>-...'. */
static void attribute_increase_index(bke::MutableAttributeAccessor &attributes,
                                     const StringRef shape_key_attribute,
                                     const int index,
                                     const int max_index)
{
  for (int i = max_index; i >= index; i--) {
    std::string attribute_name = SHAPE_KEY_ATTRIBUTE_PREFIX + std::to_string(i) +
                                 shape_key_attribute;
    std::string attribute_name_new = SHAPE_KEY_ATTRIBUTE_PREFIX + std::to_string(i + 1) +
                                     shape_key_attribute;
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
    attribute_increase_index(attributes, SHAPE_KEY_POINT_POS_DISTANCE, index, max_index);
    attribute_increase_index(attributes, SHAPE_KEY_POINT_POS_ANGLE, index, max_index);
    attribute_increase_index(attributes, SHAPE_KEY_POINT_RADIUS, index, max_index);
    attribute_increase_index(attributes, SHAPE_KEY_POINT_OPACITY, index, max_index);
    attribute_increase_index(attributes, SHAPE_KEY_POINT_VERTEX_COLOR, index, max_index);
  }
}

static void add_shape_key_modifier(Object *object, Main *bmain, Scene *scene, ReportList *reports)
{
  ModifierData *md = BKE_modifiers_findby_type(object, eModifierType_GreasePencilShapeKey);
  if (md != nullptr) {
    return;
  }

  md = ed::object::modifier_add(
      reports, bmain, scene, object, "Shape Key", eModifierType_GreasePencilShapeKey);
  if (md == nullptr) {
    BKE_report(reports, RPT_WARNING, "Unable to add a Shape Key modifier to the object");
    return;
  }

  /* By default, put the shape key modifier on top of the modifier list. The user can change
   * the order afterwards for specific use cases. */
  const int index = BLI_findindex(&object->modifiers, md);
  BLI_listbase_move_index(&object->modifiers, index, 0);
}

static wmOperatorStatus add_exec(bContext *C, wmOperator *op)
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
  shape_key_new->pass_index = 0;

  /* Copy values of currently active shape key. */
  const int index = BLI_findindex(&grease_pencil->shape_keys, shape_key_new);
  if (shape_key_active != nullptr) {
    if (!name_given) {
      strcpy(name, shape_key_active->name);
    }
    shape_key_new->range_min = shape_key_active->range_min;
    shape_key_new->range_max = shape_key_active->range_max;
    shape_key_new->pass_index = shape_key_active->pass_index;

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
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  add_shape_key_modifier(object, bmain, scene, op->reports);

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
  std::string attribute_name = SHAPE_KEY_ATTRIBUTE_PREFIX + std::to_string(index) +
                               shape_key_attribute;
  attributes.remove(attribute_name);

  for (int i = index + 1; i <= max_index; i++) {
    attribute_name = SHAPE_KEY_ATTRIBUTE_PREFIX + std::to_string(i) + shape_key_attribute;
    std::string attribute_name_new = SHAPE_KEY_ATTRIBUTE_PREFIX + std::to_string(i - 1) +
                                     shape_key_attribute;
    if (attributes.contains(attribute_name)) {
      attributes.rename(attribute_name, attribute_name_new);
    }
  }
}

static wmOperatorStatus remove_exec(bContext *C, wmOperator *op)
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
    attribute_remove(attributes, SHAPE_KEY_POINT_POS_DISTANCE, index, max_index);
    attribute_remove(attributes, SHAPE_KEY_POINT_POS_ANGLE, index, max_index);
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

static bool active_poll(bContext *C)
{
  if (!editable_grease_pencil_poll(C)) {
    return false;
  }

  GreasePencil *grease_pencil = from_context(*C);
  return !BLI_listbase_is_empty(&grease_pencil->shape_keys) &&
         (edit_state == ShapeKeyEditState::Inactive);
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

static wmOperatorStatus remove_all_exec(bContext *C, wmOperator *op)
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
      attribute_remove(attributes, SHAPE_KEY_POINT_POS_DISTANCE, index, max_index);
      attribute_remove(attributes, SHAPE_KEY_POINT_POS_ANGLE, index, max_index);
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

static wmOperatorStatus clear_exec(bContext *C, wmOperator * /*op*/)
{
  GreasePencil *grease_pencil = from_context(*C);

  LISTBASE_FOREACH (GreasePencilShapeKey *, shape_key, &grease_pencil->shape_keys) {
    shape_key->value = math::clamp(0.0f, shape_key->range_min, shape_key->range_max);
  }

  DEG_id_tag_update(&grease_pencil->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_shape_key_clear(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Clear Shape Keys";
  ot->idname = "GREASE_PENCIL_OT_shape_key_clear";
  ot->description =
      "Reset the values of all shape keys to 0 or to the closest value within the range";
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Callbacks. */
  ot->poll = active_poll;
  ot->exec = clear_exec;
}

static void attribute_move(bke::MutableAttributeAccessor attributes,
                           const StringRef shape_key_attribute,
                           const int old_index,
                           const int new_index,
                           const int max_index)
{
  std::string attribute_old = SHAPE_KEY_ATTRIBUTE_PREFIX + std::to_string(old_index) +
                              shape_key_attribute;
  std::string attribute_new = SHAPE_KEY_ATTRIBUTE_PREFIX + std::to_string(new_index) +
                              shape_key_attribute;
  std::string attribute_temp = SHAPE_KEY_ATTRIBUTE_PREFIX + std::to_string(max_index + 1) +
                               shape_key_attribute;

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

static wmOperatorStatus move_exec(bContext *C, wmOperator *op)
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
    attribute_move(attributes, SHAPE_KEY_POINT_POS_DISTANCE, old_index, new_index, max_index);
    attribute_move(attributes, SHAPE_KEY_POINT_POS_ANGLE, old_index, new_index, max_index);
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
  std::string attribute_id_src = SHAPE_KEY_ATTRIBUTE_PREFIX + std::to_string(index_src) +
                                 shape_key_attribute;
  std::string attribute_id_dst = SHAPE_KEY_ATTRIBUTE_PREFIX + std::to_string(index_dst) +
                                 shape_key_attribute;

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

static wmOperatorStatus duplicate_exec(bContext *C, wmOperator *op)
{
  GreasePencil *grease_pencil = from_context(*C);
  const int index_src = grease_pencil->active_shape_key_index;
  GreassePencilShapeKey *shape_key_src = BKE_grease_pencil_shape_key_active_get(grease_pencil);
  const float value_src = shape_key_src->value;

  /* Create new shape key, based on the active one. */
  if (add_exec(C, op) & OPERATOR_CANCELLED) {
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
    attributes_duplicate(attributes, SHAPE_KEY_POINT_POS_DISTANCE, index_src, index_dst);
    attributes_duplicate(attributes, SHAPE_KEY_POINT_POS_ANGLE, index_src, index_dst);
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

/* Find the Grease Pencil object of the shape key that has been edited. Theoretically the
 * `edit_data.grease_pencil` pointer could be changed since the shape key editing started. */
static bool ensure_valid_grease_pencil_of_edited_shapekey(bContext *C, ShapeKeyEditData &edit_data)
{
  Main *bmain = CTX_data_main(C);
  LISTBASE_FOREACH (GreasePencil *, grease_pencil, &bmain->grease_pencils) {
    if (grease_pencil->flag & GREASE_PENCIL_SHAPE_KEY_IS_EDITED) {
      edit_data.grease_pencil = grease_pencil;
      return true;
    }
  }
  return false;
}

/* After shape key editing, restore the layer transformation and opacity to base values. */
static void restore_base_layers(ShapeKeyEditData &edit_data)
{
  using namespace bke::greasepencil;

  /* Restore layer properties possibly affected by shape key. */
  for (Layer *layer : edit_data.grease_pencil->layers_for_write()) {
    if (layer->shape_key_edit_index == 0) {
      continue;
    }
    const int base_layer_index = layer->shape_key_edit_index - 1;
    const LayerBase &base_layer = edit_data.base_layers[base_layer_index];
    copy_v3_v3(layer->translation, base_layer.translation);
    copy_v3_v3(layer->rotation, base_layer.rotation);
    copy_v3_v3(layer->scale, base_layer.scale);
    layer->opacity = base_layer.opactity;
  }
}

/* After shape key editing, remove the temporary stroke index attribute from each drawing. */
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
  edit_state = ShapeKeyEditState::Inactive;

  if (edit_data == nullptr) {
    return;
  }

  /* Make sure that the pointer to our Grease Pencil object is still valid. */
  ensure_valid_grease_pencil_of_edited_shapekey(C, *edit_data);

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

  /* Clear 'edit mode' state in 3D viewports. */
  const Main *bmain = CTX_data_main(C);
  LISTBASE_FOREACH (bScreen *, screen, &bmain->screens) {
    LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
      if (area->spacetype != SPACE_VIEW3D) {
        continue;
      }
      View3D *v3d = static_cast<View3D *>(area->spacedata.first);
      v3d->overlay.flag &= ~V3D_OVERLAY_GP_SHOW_EDIT_SHAPE_KEY;
    }
  }

  /* Remove viewport draw handler. */
  if (edit_data->draw_handle) {
    ED_region_draw_cb_exit(edit_data->region_type, edit_data->draw_handle);
  }

  /* Remove edit state flag. */
  edit_data->grease_pencil->flag &= ~GREASE_PENCIL_SHAPE_KEY_IS_EDITED;

  /* Update Grease Pencil object. */
  DEG_id_tag_update(&edit_data->grease_pencil->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, edit_data->grease_pencil);

  /* Remove operator data. */
  MEM_delete(edit_data);
  op->customdata = nullptr;
}

/* When cancelling shape key editing, revert the shape-keyed geometry to their base values. */
static void edit_cancel(bContext *C, wmOperator *op)
{
  using namespace bke;
  using namespace bke::greasepencil;

  ShapeKeyEditData &edit_data = *static_cast<ShapeKeyEditData *>(op->customdata);
  ensure_valid_grease_pencil_of_edited_shapekey(C, edit_data);

  /* Collect all drawings. */
  GreasePencil &grease_pencil = *edit_data.grease_pencil;
  Vector<Drawing *> shaped_drawings;
  for (const int drawing_i : grease_pencil.drawings().index_range()) {
    GreasePencilDrawingBase *drawing_base = grease_pencil.drawing(drawing_i);
    if (drawing_base->type != GP_DRAWING) {
      continue;
    }
    Drawing *drawing = &(reinterpret_cast<GreasePencilDrawing *>(drawing_base))->wrap();
    shaped_drawings.append(drawing);
  }

  /* Revert all shape keyed geometry attributes to their base values. */
  threading::parallel_for(shaped_drawings.index_range(), 1, [&](const IndexRange drawing_range) {
    for (const int drawing_i : drawing_range) {
      Drawing &drawing = *shaped_drawings[drawing_i];
      if (drawing.base.shape_key_edit_index == 0) {
        continue;
      }

      /* Copy the base geometry back to the drawing, cancelling all changes. */
      drawing.strokes_for_write() = edit_data.base_geometry[drawing.base.shape_key_edit_index - 1];
    }
  });

  edit_exit(C, op);
}

static void edit_viewport_draw(const bContext *C, ARegion *region, void * /*arg*/)
{
  ScrArea *area = CTX_wm_area(C);

  /* Calculate inner bounds of the viewport. */
  int header_height = 0;
  int footer_height = 0;
  int npanel_label_width = 0;
  LISTBASE_FOREACH (ARegion *, region, &area->regionbase) {
    if (!region->runtime->visible) {
      continue;
    }
    const int alignment = RGN_ALIGN_ENUM_FROM_MASK(region->alignment);
    if (alignment == RGN_ALIGN_TOP) {
      if (ELEM(region->regiontype, RGN_TYPE_TOOL_HEADER, RGN_TYPE_HEADER)) {
        header_height += region->winy;
      }
    }
    if (alignment == RGN_ALIGN_BOTTOM && region->regiontype == RGN_TYPE_ASSET_SHELF) {
      footer_height += region->winy;
    }
    if (alignment == RGN_ALIGN_RIGHT && region->regiontype == RGN_TYPE_UI) {
      npanel_label_width = region->winx > 0 ? 20 * UI_SCALE_FAC : 0;
    }
  }

  /* Draw rectangle outline. */
  const float half_line_w = 2.5f * UI_SCALE_FAC;
  rcti *outer_rect = &region->winrct;
  float alert_color[4];
  UI_GetThemeColor4fv(TH_SELECT, alert_color);
  GPUVertFormat *format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(format, "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  immUniformColor4fv(alert_color);
  GPU_line_width(2.0f * half_line_w);
  imm_draw_box_wire_2d(
      pos,
      half_line_w,
      footer_height + half_line_w,
      math::round(outer_rect->xmax - outer_rect->xmin - npanel_label_width - half_line_w),
      math::round(outer_rect->ymax - outer_rect->ymin - half_line_w));

  /* Draw text in colored box. */
  const rcti *inner_rect = ED_region_visible_rect(region);
  const int font_id = BLF_default();
  const uiStyle *style = UI_style_get();
  const float font_size = style->widget.points * UI_SCALE_FAC;
  BLF_size(font_id, font_size);
  float text_color[4] = {0.85f, 0.85f, 0.85f, 1.0f};
  BLF_color4fv(font_id, text_color);
  const char *text = IFACE_("Editing Shape Key");
  float text_width;
  float text_height;
  BLF_width_and_height(font_id, text, strlen(text), &text_width, &text_height);
  const float padding_x = 7.0f * UI_SCALE_FAC;
  const float padding_y = 4.0f * UI_SCALE_FAC;
  const float x = float(inner_rect->xmin) + (0.5f * U.widget_unit) + 2.0f;
  const float y = float(inner_rect->ymax) - (0.1f * U.widget_unit) - text_height -
                  2.0f * padding_y - 4.0f;
  GPU_line_width(text_height + 2.0f * padding_y);
  imm_draw_box_wire_2d(
      pos, x - padding_x, y + padding_y, x + text_width + padding_x, y + padding_y);
  immUnbindProgram();
  GPU_line_width(1.0f);

  BLF_position(font_id, x, y, 0);
  BLF_draw(font_id, text, strlen(text));
}

/* Data structure for a collection of shape key deltas, collected for a list of shape keys and
 * a given geometry attribute (e.g. 'fill_color' or 'radius'). */
template<typename T> struct ShapeKeysDeltas {
  /* Is the attribute shaped by any of the shape keys? */
  bool is_shaped;
  /* Per shape key: flag if there is a delta for that shape key. */
  Array<bool> has_delta;
  /* Per shape key: the delta values of the attribute for that shape key. */
  Array<VArraySpan<T>> deltas;
  /* The attribute values in the drawing. */
  MutableSpan<T> in_drawing;
};

/* Collect the shape key deltas for a list of shape keys and a given geometry attribute. */
template<typename T>
static ShapeKeysDeltas<T> collect_shape_keys_deltas(const Span<int> shape_key_indices,
                                                    const StringRef shape_key_attribute,
                                                    bke::MutableAttributeAccessor &attributes,
                                                    const bke::AttrDomain domain)
{
  ShapeKeysDeltas<T> collection;
  collection.is_shaped = false;
  collection.has_delta = Array<bool>(shape_key_indices.size(), false);
  collection.deltas = Array<VArraySpan<T>>(shape_key_indices.size());

  /* Check for each shape key if the shape key delta exists. */
  for (const int index : shape_key_indices.index_range()) {
    const std::string attribute_name = SHAPE_KEY_ATTRIBUTE_PREFIX +
                                       std::to_string(shape_key_indices[index]) +
                                       shape_key_attribute;
    if (!attributes.contains(attribute_name)) {
      continue;
    }
    collection.is_shaped = true;
    collection.has_delta[index] = true;
    collection.deltas[index] = *attributes.lookup<T>(attribute_name, domain);
  }

  return collection;
}

template<typename Fn>
void apply_shape_keys_to_geometry(const IndexRange shape_keys,
                                  const Span<bool> shape_key_has_delta,
                                  Fn &&fn)
{
  for (const int shape_key : shape_keys) {
    if (shape_key_has_delta[shape_key]) {
      fn(shape_key);
    }
  }
}

bool apply_shape_keys_to_drawing(bke::greasepencil::Drawing &drawing,
                                 const Span<int> shape_key_indices,
                                 const Span<float> shape_key_factors,
                                 const IndexMask &stroke_mask)
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
  const IndexRange shape_key_range = shape_key_indices.index_range();

  /* Collect the shape key deltas of all given shape keys. */
  ShapeKeysDeltas<ColorGeometry4f> fill_color = collect_shape_keys_deltas<ColorGeometry4f>(
      shape_key_indices, SHAPE_KEY_STROKE_FILL_COLOR, attributes, AttrDomain::Curve);

  ShapeKeysDeltas<float> fill_opacity = collect_shape_keys_deltas<float>(
      shape_key_indices, SHAPE_KEY_STROKE_FILL_OPACITY, attributes, AttrDomain::Curve);

  ShapeKeysDeltas<float> radius = collect_shape_keys_deltas<float>(
      shape_key_indices, SHAPE_KEY_POINT_RADIUS, attributes, AttrDomain::Point);

  ShapeKeysDeltas<float> opacity = collect_shape_keys_deltas<float>(
      shape_key_indices, SHAPE_KEY_POINT_OPACITY, attributes, AttrDomain::Point);

  ShapeKeysDeltas<ColorGeometry4f> vertex_color = collect_shape_keys_deltas<ColorGeometry4f>(
      shape_key_indices, SHAPE_KEY_POINT_VERTEX_COLOR, attributes, AttrDomain::Point);

  MutableSpan<float3> positions;
  ShapeKeysDeltas<float> position_distance = collect_shape_keys_deltas<float>(
      shape_key_indices, SHAPE_KEY_POINT_POS_DISTANCE, attributes, AttrDomain::Point);
  ShapeKeysDeltas<math::Quaternion> position_angle = collect_shape_keys_deltas<math::Quaternion>(
      shape_key_indices, SHAPE_KEY_POINT_POS_ANGLE, attributes, AttrDomain::Point);

  if (fill_color.is_shaped) {
    fill_color.in_drawing = drawing.fill_colors_for_write();
  }
  if (fill_opacity.is_shaped) {
    fill_opacity.in_drawing = drawing.fill_opacities_for_write();
  }
  if (radius.is_shaped) {
    radius.in_drawing = drawing.radii_for_write();
  }
  if (opacity.is_shaped) {
    opacity.in_drawing = drawing.opacities_for_write();
  }
  if (vertex_color.is_shaped) {
    vertex_color.in_drawing = drawing.vertex_colors_for_write();
  }
  if (position_distance.is_shaped) {
    positions = curves.positions_for_write();
  }

  /* Apply shape keys to strokes and points. */
  stroke_mask.foreach_index(GrainSize(512), [&](const int stroke) {
    const IndexRange points = points_by_curve[stroke];

    /* Shape key: stroke fill color. */
    if (fill_color.is_shaped) {
      apply_shape_keys_to_geometry(
          shape_key_range, fill_color.has_delta, [&](const int shape_key) {
            add_v4_v4(fill_color.in_drawing[stroke],
                      float4(fill_color.deltas[shape_key][stroke]) * shape_key_factors[shape_key]);
          });
      clamp_v4(fill_color.in_drawing[stroke], 0.0f, 1.0f);
    }

    /* Shape key: stroke fill opacity. */
    if (fill_opacity.is_shaped) {
      apply_shape_keys_to_geometry(
          shape_key_range, fill_opacity.has_delta, [&](const int shape_key) {
            fill_opacity.in_drawing[stroke] += fill_opacity.deltas[shape_key][stroke] *
                                               shape_key_factors[shape_key];
          });
      fill_opacity.in_drawing[stroke] = math::clamp(fill_opacity.in_drawing[stroke], 0.0f, 1.0f);
    }

    if (radius.is_shaped || opacity.is_shaped || vertex_color.is_shaped) {
      for (const int point : points) {
        /* Shape key: point radius. */
        if (radius.is_shaped) {
          apply_shape_keys_to_geometry(
              shape_key_range, radius.has_delta, [&](const int shape_key) {
                radius.in_drawing[point] += radius.deltas[shape_key][point] *
                                            shape_key_factors[shape_key];
              });
          radius.in_drawing[point] = math::max(radius.in_drawing[point], 0.0f);
        }
        /* Shape key: point opacity. */
        if (opacity.is_shaped) {
          apply_shape_keys_to_geometry(
              shape_key_range, opacity.has_delta, [&](const int shape_key) {
                opacity.in_drawing[point] += opacity.deltas[shape_key][point] *
                                             shape_key_factors[shape_key];
              });
          opacity.in_drawing[point] = math::clamp(opacity.in_drawing[point], 0.0f, 1.0f);
        }
        /* Shape key: vertex colors. */
        if (vertex_color.is_shaped) {
          apply_shape_keys_to_geometry(
              shape_key_range, vertex_color.has_delta, [&](const int shape_key) {
                add_v4_v4(vertex_color.in_drawing[point],
                          float4(vertex_color.deltas[shape_key][point]) *
                              shape_key_factors[shape_key]);
              });
          clamp_v4(vertex_color.in_drawing[point], 0.0f, 1.0f);
        }
      }
    }

    /* Shape key: point position. */
    if (!position_distance.is_shaped) {
      return;
    }
    float3 vector_to_next_point = {1.0f, 0.0f, 0.0f};
    const float3 position_first = positions[points.first()];
    const float3 position_one_before_last =
        positions[points.size() > 1 ? (points.last() - 1) : points.first()];

    for (const int point : points) {
      float3 position_delta = {0.0f, 0.0f, 0.0f};
      if (point == points.last()) {
        if (cyclic[stroke]) {
          vector_to_next_point = position_first - positions[point];
        }
        else if (points.size() > 1) {
          vector_to_next_point = positions[point] - position_one_before_last;
        }
      }
      else {
        vector_to_next_point = positions[point + 1] - positions[point];
      }

      for (const int shape_key : shape_key_range) {
        if (!position_distance.has_delta[shape_key] ||
            position_distance.deltas[shape_key][point] == 0.0f)
        {
          continue;
        }

        /* Convert quaternion rotation and distance to a point position delta. */
        float matrix[3][3];
        quat_to_mat3(matrix, &position_angle.deltas[shape_key][point].w);
        float3 vector_to_shaped_point = vector_to_next_point;
        mul_m3_v3(matrix, vector_to_shaped_point);
        position_delta += math::normalize(vector_to_shaped_point) *
                          (position_distance.deltas[shape_key][point] *
                           shape_key_factors[shape_key]);
      }

      /* Apply the delta to the point position. */
      positions[point] += position_delta;
    }
  });

  return position_distance.is_shaped;
}

void apply_shape_keys_to_layers(GreasePencil &grease_pencil,
                                const Span<int> shape_key_indices,
                                const Span<float> shape_key_factors,
                                const IndexMask &layer_mask)
{
  bke::AttributeAccessor layer_attributes = grease_pencil.attributes();

  for (const int shape_key : shape_key_indices.index_range()) {
    const std::string shape_key_id = std::to_string(shape_key_indices[shape_key]);
    const VArray<float3> shape_key_translations = *layer_attributes.lookup_or_default<float3>(
        SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_id + SHAPE_KEY_LAYER_TRANSLATION,
        bke::AttrDomain::Layer,
        float3(0.0f, 0.0f, 0.0f));
    const VArray<float3> shape_key_rotations = *layer_attributes.lookup_or_default<float3>(
        SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_id + SHAPE_KEY_LAYER_ROTATION,
        bke::AttrDomain::Layer,
        float3(0.0f, 0.0f, 0.0f));
    const VArray<float3> shape_key_scales = *layer_attributes.lookup_or_default<float3>(
        SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_id + SHAPE_KEY_LAYER_SCALE,
        bke::AttrDomain::Layer,
        float3(0.0f, 0.0f, 0.0f));
    const VArray<float> shape_key_opacities = *layer_attributes.lookup_or_default<float>(
        SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_id + SHAPE_KEY_LAYER_OPACITY,
        bke::AttrDomain::Layer,
        0.0f);

    layer_mask.foreach_index([&](const int layer_i) {
      bke::greasepencil::Layer &layer = grease_pencil.layer(layer_i);

      copy_v3_v3(layer.translation,
                 float3(layer.translation) +
                     shape_key_translations[layer_i] * shape_key_factors[shape_key]);
      copy_v3_v3(layer.rotation,
                 float3(layer.rotation) +
                     shape_key_rotations[layer_i] * shape_key_factors[shape_key]);
      copy_v3_v3(layer.scale,
                 float3(layer.scale) + shape_key_scales[layer_i] * shape_key_factors[shape_key]);
      layer.opacity += shape_key_opacities[layer_i] * shape_key_factors[shape_key];
    });
  }

  layer_mask.foreach_index([&](const int layer_i) {
    bke::greasepencil::Layer &layer = grease_pencil.layer(layer_i);
    layer.opacity = math::clamp(layer.opacity, 0.0f, 1.0f);
  });
}

/* Determine if an attribute may have different values in attribute collection A and B.
 * Returns true when the values may differ, returns false when the attribute values are definitely
 * the same. */
static bool attribute_may_differ(const StringRef attribute_name,
                                 const bke::AttrDomain domain,
                                 const bke::MutableAttributeAccessor &attributes_a,
                                 const bke::AttributeAccessor &attributes_b)
{
  /* Attribute values are the same when they fall back to the default value in both collections. */
  if (!attributes_a.contains(attribute_name) && !attributes_b.contains(attribute_name)) {
    return false;
  }

  /* Attribute values are the same when they have the same implicit sharing info. */
  const ImplicitSharingInfo *sharing_info_a =
      attributes_a.lookup(attribute_name, domain).sharing_info;
  const ImplicitSharingInfo *sharing_info_b =
      attributes_b.lookup(attribute_name, domain).sharing_info;
  return sharing_info_a == nullptr || sharing_info_b == nullptr ||
         sharing_info_a != sharing_info_b;
}

/* Data structure for retrieving the shape key delta between a shaped drawing and the base drawing
 * for a given geometry attribute (e.g. 'fill_color' or 'radius'). */
template<typename T> struct ShapeKeyDelta {
  bool check_for_delta;
  std::atomic<bool> has_delta = false;
  /* The geometry attribute in the shaped drawing. */
  MutableSpan<T> in_shaped_drawing;
  /* The geometry attribute in the base drawing. */
  VArray<T> in_base_drawing;
  /* The shape key delta between the shaped and the base drawing. */
  Array<T> deltas;
};

struct ShapeKeyPositionDelta {
  bool check_for_delta;
  std::atomic<bool> has_delta = false;
  MutableSpan<float3> in_shaped_drawing;
  Span<float3> in_base_drawing;
  Array<float> distance_deltas;
  Array<math::Quaternion> angle_deltas;
};

/* Get the shape key deltas of all strokes and points in the given drawings.
 * While getting the deltas, revert the shape-keyed attributes to their base values.
 * When target drawings are passed, the shape key deltas are added to the target drawings instead
 * of the shaped drawings. Note that the target drawings must have the same topology as the base
 * drawings. */
void get_shape_key_stroke_deltas(ShapeKeyEditData &edit_data,
                                 const Span<bke::greasepencil::Drawing *> shaped_drawings,
                                 const bool use_target,
                                 std::optional<Span<bke::greasepencil::Drawing *>> target_drawings)
{
  using namespace bke;
  using namespace bke::greasepencil;

  const std::string shape_key_id = std::to_string(edit_data.edited_shape_key_index);
  const bool remove_empty_delta = !use_target;

  threading::parallel_for(shaped_drawings.index_range(), 1, [&](const IndexRange drawing_range) {
    for (const int drawing_i : drawing_range) {
      Drawing &drawing = *shaped_drawings[drawing_i];
      if (drawing.base.shape_key_edit_index == 0) {
        continue;
      }
      Drawing *target_drawing;
      if (use_target) {
        target_drawing = (*target_drawings)[drawing.base.shape_key_edit_index - 1];
      }

      /* Get edited and base geometry. */
      CurvesGeometry &curves = drawing.strokes_for_write();
      CurvesGeometry &base_curves = edit_data.base_geometry[drawing.base.shape_key_edit_index - 1];
      CurvesGeometry &target_curves = use_target ? target_drawing->strokes_for_write() :
                                                   drawing.strokes_for_write();
      MutableAttributeAccessor attributes = curves.attributes_for_write();
      AttributeAccessor base_attributes = base_curves.attributes();
      MutableAttributeAccessor target_attributes = use_target ?
                                                       target_curves.attributes_for_write() :
                                                       curves.attributes_for_write();
      const OffsetIndices points_by_curve = curves.points_by_curve();
      const OffsetIndices base_points_by_curve = base_curves.points_by_curve();
      const OffsetIndices target_points_by_curve = target_curves.points_by_curve();
      VArray<int> base_stroke_indices = *attributes.lookup_or_default<int>(
          SHAPE_KEY_BASE_STROKE_INDEX, AttrDomain::Curve, 0);
      const VArray<bool> cyclic = curves.cyclic();
      const int curves_num = target_curves.curves_num();
      const int points_num = target_curves.points_num();

      /* Compare implicit sharing info of shape key attributes in the base drawing and the shape
       * key drawing. When the pointers to the sharing info match, we know that the attributes have
       * the same values and that we don't have to check for shape key deltas. */
      ShapeKeyDelta<ColorGeometry4f> fill_color{};
      fill_color.check_for_delta = attribute_may_differ(
          "fill_color", AttrDomain::Curve, attributes, base_attributes);
      if (fill_color.check_for_delta) {
        fill_color.deltas = Array<ColorGeometry4f>(curves_num,
                                                   ColorGeometry4f(0.0f, 0.0f, 0.0f, 0.0f));
        fill_color.in_shaped_drawing = drawing.fill_colors_for_write();
        fill_color.in_base_drawing = *base_attributes.lookup_or_default<ColorGeometry4f>(
            "fill_color", AttrDomain::Curve, ColorGeometry4f(0.0f, 0.0f, 0.0f, 0.0f));
      }

      ShapeKeyDelta<float> fill_opacity{};
      fill_opacity.check_for_delta = attribute_may_differ(
          "fill_opacity", AttrDomain::Curve, attributes, base_attributes);
      if (fill_opacity.check_for_delta) {
        fill_opacity.deltas = Array<float>(curves_num, 0.0f);
        fill_opacity.in_shaped_drawing = drawing.fill_opacities_for_write();
        fill_opacity.in_base_drawing = *base_attributes.lookup_or_default<float>(
            "fill_opacity", AttrDomain::Curve, 1.0f);
      }

      ShapeKeyPositionDelta position{};
      position.check_for_delta = attribute_may_differ(
          "position", AttrDomain::Point, attributes, base_attributes);
      if (position.check_for_delta) {
        position.angle_deltas = Array<math::Quaternion>(points_num, math::Quaternion::identity());
        position.distance_deltas = Array<float>(points_num, 0.0f);
        position.in_shaped_drawing = curves.positions_for_write();
        position.in_base_drawing = base_curves.positions();
      }

      ShapeKeyDelta<float> radius{};
      radius.check_for_delta = attribute_may_differ(
          "radius", AttrDomain::Point, attributes, base_attributes);
      if (radius.check_for_delta) {
        radius.deltas = Array<float>(points_num, 0.0f);
        radius.in_shaped_drawing = drawing.radii_for_write();
        radius.in_base_drawing = *base_attributes.lookup_or_default<float>(
            "radii", AttrDomain::Point, 0.01f);
      }

      ShapeKeyDelta<float> opacity{};
      opacity.check_for_delta = attribute_may_differ(
          "opacity", AttrDomain::Point, attributes, base_attributes);
      if (opacity.check_for_delta) {
        opacity.deltas = Array<float>(points_num, 0.0f);
        opacity.in_shaped_drawing = drawing.opacities_for_write();
        opacity.in_base_drawing = *base_attributes.lookup_or_default<float>(
            "opacity", AttrDomain::Point, 1.0f);
      }

      ShapeKeyDelta<ColorGeometry4f> vertex_color{};
      vertex_color.check_for_delta = attribute_may_differ(
          "vertex_color", AttrDomain::Point, attributes, base_attributes);
      if (vertex_color.check_for_delta) {
        vertex_color.deltas = Array<ColorGeometry4f>(points_num,
                                                     ColorGeometry4f(0.0f, 0.0f, 0.0f, 0.0f));
        vertex_color.in_shaped_drawing = drawing.vertex_colors_for_write();
        vertex_color.in_base_drawing = *base_attributes.lookup_or_default<ColorGeometry4f>(
            "vertex_color", AttrDomain::Point, ColorGeometry4f(0.0f, 0.0f, 0.0f, 0.0f));
      }

      /* Loop over edited strokes and look for changed shape key properties. */
      if (fill_color.check_for_delta || fill_opacity.check_for_delta || position.check_for_delta ||
          radius.check_for_delta || opacity.check_for_delta || vertex_color.check_for_delta)
      {
        threading::parallel_for(curves.curves_range(), 512, [&](const IndexRange curves_range) {
          bool fill_color_changed = false;
          bool fill_opacity_changed = false;
          bool position_changed = false;
          bool radius_changed = false;
          bool opacity_changed = false;
          bool vertex_color_changed = false;

          for (const int stroke : curves_range) {
            const int base_stroke = base_stroke_indices[stroke] - 1;
            /* Skip strokes without base reference. */
            if (base_stroke == -1) {
              continue;
            }
            const int target_stroke = use_target ? base_stroke : stroke;

            /* When the number of points in the shaped stroke and the base stroke don't match, it's
             * difficult to create a shape key. For now, we ignore non-matching strokes.
             * In the future we could implement a smarter algorithm for matching the points. */
            if (points_by_curve[stroke].size() != base_points_by_curve[base_stroke].size()) {
              continue;
            }

            /* Store delta of stroke fill color. */
            if (fill_color.check_for_delta) {
              const float4 color_delta = float4(fill_color.in_shaped_drawing[stroke]) -
                                         float4(fill_color.in_base_drawing[base_stroke]);
              if (!math::is_zero(color_delta, EPSILON)) {
                fill_color_changed = true;
                fill_color.deltas[target_stroke] = ColorGeometry4f(color_delta);
              }
              /* Restore to base value. */
              fill_color.in_shaped_drawing[stroke] = fill_color.in_base_drawing[base_stroke];
            }

            /* Store delta of stroke fill opacity. */
            if (fill_opacity.check_for_delta) {
              const float delta = fill_opacity.in_shaped_drawing[stroke] -
                                  fill_opacity.in_base_drawing[base_stroke];
              if (math::abs(delta) > EPSILON) {
                fill_opacity_changed = true;
                fill_opacity.deltas[target_stroke] = delta;
              }
              /* Restore to base value. */
              fill_opacity.in_shaped_drawing[stroke] = fill_opacity.in_base_drawing[base_stroke];
            }

            /* Get stroke point deltas. */
            if (!(position.check_for_delta || radius.check_for_delta || opacity.check_for_delta ||
                  vertex_color.check_for_delta))
            {
              continue;
            }
            const IndexRange points = points_by_curve[stroke];
            const int target_point_delta = points.first() -
                                           target_points_by_curve[target_stroke].first();
            float3 vector_to_next_point = {1.0f, 0.0f, 0.0f};
            for (const int point : points) {
              /* NOTE: This assumes that the number of points in the shaped stroke and the base
               * stroke are equal. */
              const int target_point = point - target_point_delta;

              if (position.check_for_delta) {
                /* Get angle and distance between base point and shape-keyed point. */
                if (position.in_shaped_drawing[point] != position.in_base_drawing[point]) {
                  float3 vector_to_shaped_point = position.in_shaped_drawing[point] -
                                                  position.in_base_drawing[point];
                  const float distance = math::length(vector_to_shaped_point);
                  if (distance > EPSILON) {
                    if (point == points.last()) {
                      if (cyclic[stroke]) {
                        vector_to_next_point = position.in_base_drawing[points.first()] -
                                               position.in_base_drawing[point];
                      }
                      else if (points.size() > 1) {
                        vector_to_next_point = position.in_base_drawing[point] -
                                               position.in_base_drawing[points.last() - 1];
                      }
                    }
                    else {
                      vector_to_next_point = position.in_base_drawing[point + 1] -
                                             position.in_base_drawing[point];
                    }
                    vector_to_shaped_point = math::normalize(vector_to_shaped_point);
                    vector_to_next_point = math::normalize(vector_to_next_point);
                    float4 angle;
                    rotation_between_vecs_to_quat(
                        angle, vector_to_next_point, vector_to_shaped_point);

                    position_changed = true;
                    position.distance_deltas[target_point] = distance;
                    position.angle_deltas[target_point] = math::Quaternion(angle);
                  }
                  /* Restore to base value. */
                  position.in_shaped_drawing[point] = position.in_base_drawing[point];
                }
              }

              /* Get radius delta. */
              if (radius.check_for_delta) {
                const float delta = radius.in_shaped_drawing[point] -
                                    radius.in_base_drawing[point];
                if (math::abs(delta) > EPSILON) {
                  radius_changed = true;
                  radius.deltas[target_point] = delta;
                }
                /* Restore to base value. */
                radius.in_shaped_drawing[point] = radius.in_base_drawing[point];
              }

              /* Get opacity delta. */
              if (opacity.check_for_delta) {
                const float delta = opacity.in_shaped_drawing[point] -
                                    opacity.in_base_drawing[point];
                if (math::abs(delta) > EPSILON) {
                  opacity_changed = true;
                  opacity.deltas[target_point] = delta;
                }
                /* Restore to base value. */
                opacity.in_shaped_drawing[point] = opacity.in_base_drawing[point];
              }

              /* Get vertex color delta. */
              if (vertex_color.check_for_delta) {
                const float4 color_delta = float4(vertex_color.in_shaped_drawing[point]) -
                                           float4(vertex_color.in_base_drawing[point]);
                if (!math::is_zero(color_delta, EPSILON)) {
                  vertex_color_changed = true;
                  vertex_color.deltas[target_point] = ColorGeometry4f(color_delta);
                }
                /* Restore to base value. */
                vertex_color.in_shaped_drawing[point] = vertex_color.in_base_drawing[point];
              }
            }
          }

          if (fill_color_changed) {
            fill_color.has_delta.store(true, std::memory_order_relaxed);
          }
          if (fill_opacity_changed) {
            fill_opacity.has_delta.store(true, std::memory_order_relaxed);
          }
          if (position_changed) {
            position.has_delta.store(true, std::memory_order_relaxed);
          }
          if (radius_changed) {
            radius.has_delta.store(true, std::memory_order_relaxed);
          }
          if (opacity_changed) {
            opacity.has_delta.store(true, std::memory_order_relaxed);
          }
          if (vertex_color_changed) {
            vertex_color.has_delta.store(true, std::memory_order_relaxed);
          }
        });
      }

      /* Store stroke and point deltas for the edited shape key. Or remove them when there is no
       * delta for the geometry attribute. */
      if (fill_color.has_delta) {
        SpanAttributeWriter attr_deltas =
            target_attributes.lookup_or_add_for_write_span<ColorGeometry4f>(
                SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_id + SHAPE_KEY_STROKE_FILL_COLOR,
                AttrDomain::Curve);
        attr_deltas.span.copy_from(fill_color.deltas);
        attr_deltas.finish();
      }
      else if (remove_empty_delta) {
        attributes.remove(SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_id + SHAPE_KEY_STROKE_FILL_COLOR);
      }
      if (fill_opacity.has_delta) {
        SpanAttributeWriter attr_deltas = target_attributes.lookup_or_add_for_write_span<float>(
            SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_id + SHAPE_KEY_STROKE_FILL_OPACITY,
            AttrDomain::Curve);
        attr_deltas.span.copy_from(fill_opacity.deltas);
        attr_deltas.finish();
      }
      else if (remove_empty_delta) {
        attributes.remove(SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_id +
                          SHAPE_KEY_STROKE_FILL_OPACITY);
      }
      if (position.has_delta) {
        SpanAttributeWriter attr_deltas = target_attributes.lookup_or_add_for_write_span<float>(
            SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_id + SHAPE_KEY_POINT_POS_DISTANCE,
            AttrDomain::Point);
        attr_deltas.span.copy_from(position.distance_deltas);
        attr_deltas.finish();
        SpanAttributeWriter attr_deltas1 =
            target_attributes.lookup_or_add_for_write_span<math::Quaternion>(
                SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_id + SHAPE_KEY_POINT_POS_ANGLE,
                AttrDomain::Point);
        attr_deltas1.span.copy_from(position.angle_deltas);
        attr_deltas1.finish();

        drawing.tag_positions_changed();
        if (use_target) {
          target_drawing->tag_positions_changed();
        }
      }
      else if (remove_empty_delta) {
        attributes.remove(SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_id +
                          SHAPE_KEY_POINT_POS_DISTANCE);
        attributes.remove(SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_id + SHAPE_KEY_POINT_POS_ANGLE);
      }
      if (radius.has_delta) {
        SpanAttributeWriter attr_deltas = target_attributes.lookup_or_add_for_write_span<float>(
            SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_id + SHAPE_KEY_POINT_RADIUS, AttrDomain::Point);
        attr_deltas.span.copy_from(radius.deltas);
        attr_deltas.finish();
      }
      else if (remove_empty_delta) {
        attributes.remove(SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_id + SHAPE_KEY_POINT_RADIUS);
      }
      if (opacity.has_delta) {
        SpanAttributeWriter attr_deltas = target_attributes.lookup_or_add_for_write_span<float>(
            SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_id + SHAPE_KEY_POINT_OPACITY,
            AttrDomain::Point);
        attr_deltas.span.copy_from(opacity.deltas);
        attr_deltas.finish();
      }
      else if (remove_empty_delta) {
        attributes.remove(SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_id + SHAPE_KEY_POINT_OPACITY);
      }
      if (vertex_color.has_delta) {
        SpanAttributeWriter attr_deltas =
            target_attributes.lookup_or_add_for_write_span<ColorGeometry4f>(
                SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_id + SHAPE_KEY_POINT_VERTEX_COLOR,
                AttrDomain::Point);
        attr_deltas.span.copy_from(vertex_color.deltas);
        attr_deltas.finish();
      }
      else if (remove_empty_delta) {
        attributes.remove(SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_id +
                          SHAPE_KEY_POINT_VERTEX_COLOR);
      }
    }
  });
}

static void get_shape_key_layer_deltas(ShapeKeyEditData &edit_data,
                                       GreasePencil *target_grease_pencil = nullptr)
{
  using namespace bke;
  using namespace bke::greasepencil;

  const bool use_target = target_grease_pencil != nullptr;
  const bool remove_empty_delta = !use_target;

  const std::string shape_key_id = std::to_string(edit_data.edited_shape_key_index);
  GreasePencil &grease_pencil = *edit_data.grease_pencil;
  MutableAttributeAccessor layer_attributes = use_target ?
                                                  target_grease_pencil->attributes_for_write() :
                                                  grease_pencil.attributes_for_write();
  const int layers_num = use_target ? target_grease_pencil->layers().size() :
                                      grease_pencil.layers().size();

  /* Get layer deltas for the edited shape key. */
  Array<float3> translation_deltas(layers_num, float3(0.0f, 0.0f, 0.0f));
  Array<float3> rotation_deltas(layers_num, float3(0.0f, 0.0f, 0.0f));
  Array<float3> scale_deltas(layers_num, float3(0.0f, 0.0f, 0.0f));
  Array<float> opacity_deltas(layers_num, 0.0f);
  bool translation_has_delta = false;
  bool rotation_has_delta = false;
  bool scale_has_delta = false;
  bool opacity_has_delta = false;

  for (const int layer_i : grease_pencil.layers().index_range()) {
    const Layer &layer = grease_pencil.layer(layer_i);

    /* Skip when base layer is missing. */
    if (layer.shape_key_edit_index == 0) {
      continue;
    }
    const int base_layer_index = layer.shape_key_edit_index - 1;
    const int target_layer_index = use_target ? base_layer_index : layer_i;

    /* Compare edited layer with base layer. */
    const LayerBase &base_layer = edit_data.base_layers[base_layer_index];
    const float3 translation_delta = float3(layer.translation) - base_layer.translation;
    const float3 rotation_delta = float3(layer.rotation) - base_layer.rotation;
    const float3 scale_delta = float3(layer.scale) - base_layer.scale;
    const float opacity_delta = layer.opacity - base_layer.opactity;

    if (!math::is_zero(translation_delta, EPSILON)) {
      translation_has_delta = true;
      translation_deltas[target_layer_index] = translation_delta;
    }
    if (!math::is_zero(rotation_delta, EPSILON)) {
      rotation_has_delta = true;
      rotation_deltas[target_layer_index] = rotation_delta;
    }
    if (!math::is_zero(scale_delta, EPSILON)) {
      scale_has_delta = true;
      scale_deltas[target_layer_index] = scale_delta;
    }
    if (math::abs(opacity_delta) > EPSILON) {
      opacity_has_delta = true;
      opacity_deltas[target_layer_index] = opacity_delta;
    }
  }

  /* Store layer attributes for the edited shape key. */
  if (translation_has_delta) {
    SpanAttributeWriter attr_deltas = layer_attributes.lookup_or_add_for_write_span<float3>(
        SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_id + SHAPE_KEY_LAYER_TRANSLATION,
        AttrDomain::Layer);
    attr_deltas.span.copy_from(translation_deltas);
    attr_deltas.finish();
  }
  else if (remove_empty_delta) {
    layer_attributes.remove(SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_id +
                            SHAPE_KEY_LAYER_TRANSLATION);
  }
  if (rotation_has_delta) {
    SpanAttributeWriter attr_deltas = layer_attributes.lookup_or_add_for_write_span<float3>(
        SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_id + SHAPE_KEY_LAYER_ROTATION, AttrDomain::Layer);
    attr_deltas.span.copy_from(rotation_deltas);
    attr_deltas.finish();
  }
  else if (remove_empty_delta) {
    layer_attributes.remove(SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_id + SHAPE_KEY_LAYER_ROTATION);
  }
  if (scale_has_delta) {
    SpanAttributeWriter attr_deltas = layer_attributes.lookup_or_add_for_write_span<float3>(
        SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_id + SHAPE_KEY_LAYER_SCALE, AttrDomain::Layer);
    attr_deltas.span.copy_from(scale_deltas);
    attr_deltas.finish();
  }
  else if (remove_empty_delta) {
    layer_attributes.remove(SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_id + SHAPE_KEY_LAYER_SCALE);
  }
  if (opacity_has_delta) {
    SpanAttributeWriter attr_deltas = layer_attributes.lookup_or_add_for_write_span<float>(
        SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_id + SHAPE_KEY_LAYER_OPACITY, AttrDomain::Layer);
    attr_deltas.span.copy_from(opacity_deltas);
    attr_deltas.finish();
  }
  else if (remove_empty_delta) {
    layer_attributes.remove(SHAPE_KEY_ATTRIBUTE_PREFIX + shape_key_id + SHAPE_KEY_LAYER_OPACITY);
  }
}

/* Get the shape key deltas for layers and drawings by comparing the edited shape key values
 * with the base values. This also reverts the shape-keyed drawings to their base versions. */
static void get_shape_key_deltas(ShapeKeyEditData &edit_data)
{
  using namespace bke::greasepencil;

  /* Get layer deltas. */
  get_shape_key_layer_deltas(edit_data);

  /* Get deltas in geometry for the edited shape key. */
  GreasePencil &grease_pencil = *edit_data.grease_pencil;
  Vector<Drawing *> drawings;
  for (const int drawing_i : grease_pencil.drawings().index_range()) {
    GreasePencilDrawingBase *drawing_base = grease_pencil.drawing(drawing_i);
    if (drawing_base->type != GP_DRAWING) {
      continue;
    }
    Drawing *drawing = &(reinterpret_cast<GreasePencilDrawing *>(drawing_base))->wrap();
    drawings.append(drawing);
  }

  get_shape_key_stroke_deltas(edit_data, drawings);
}

static void store_base_layers(ShapeKeyEditData &edit_data)
{
  GreasePencil &grease_pencil = *edit_data.grease_pencil;

  /* Store relevant shape key data of base layers: translation, rotation, scale and opacity. */
  edit_data.base_layers.reinitialize(grease_pencil.layers().size());
  for (const int layer_i : grease_pencil.layers().index_range()) {
    bke::greasepencil::Layer &layer = grease_pencil.layer(layer_i);
    LayerBase layer_base{};
    layer_base.translation = layer.translation;
    layer_base.rotation = layer.rotation;
    layer_base.scale = layer.scale;
    layer_base.opactity = layer.opacity;

    /* Store the base layer and an index reference on the layer with the applied shape key. */
    edit_data.base_layers[layer_i] = layer_base;
    layer.shape_key_edit_index = layer_i + 1;
  }
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
  grease_pencil.flag |= GREASE_PENCIL_SHAPE_KEY_IS_EDITED;

  /* Set flag now we enter edit mode. */
  edit_state = ShapeKeyEditState::Active;

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

  /* Add draw handler to the viewport for a colored rectangle marking shape key 'edit mode'. */
  const SpaceType *space_type = BKE_spacetype_from_id(SPACE_VIEW3D);
  edit_data.region_type = BKE_regiontype_from_id(space_type, RGN_TYPE_WINDOW);
  edit_data.draw_handle = ED_region_draw_cb_activate(
      edit_data.region_type, edit_viewport_draw, nullptr, REGION_DRAW_POST_PIXEL);

  /* Set 'edit mode' state in 3D viewports. */
  const Main *bmain = CTX_data_main(C);
  LISTBASE_FOREACH (bScreen *, screen, &bmain->screens) {
    LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
      if (area->spacetype != SPACE_VIEW3D) {
        continue;
      }
      View3D *v3d = static_cast<View3D *>(area->spacedata.first);
      v3d->overlay.flag |= V3D_OVERLAY_GP_SHOW_EDIT_SHAPE_KEY;
    }
  }

  /* Store relevant shape key data of base layers: translation, rotation, scale and opacity. */
  store_base_layers(edit_data);

  /* Apply the edited shape key to the layers. During edit, the shape key changes to layers must be
   * visible in the UI (layer transformation and opacity), so we apply them manually (and not by
   * the shape key modifier). */
  Array<int> edited_shape_key(1, edit_data.edited_shape_key_index);
  Array<float> factor(1, 1.0f);
  IndexMask all_layers(IndexRange(grease_pencil.layers().size()));
  const std::string shape_key_id = std::to_string(edit_data.edited_shape_key_index);
  apply_shape_keys_to_layers(grease_pencil, edited_shape_key, factor, all_layers);

  /* Store the base drawings. */
  edit_data.base_geometry.reinitialize(grease_pencil.drawings().size());
  threading::parallel_for(
      grease_pencil.drawings().index_range(), 1, [&](const IndexRange drawing_range) {
        for (const int drawing_i : drawing_range) {
          GreasePencilDrawingBase *drawing_base = grease_pencil.drawing(drawing_i);
          Drawing &drawing = (reinterpret_cast<GreasePencilDrawing *>(drawing_base))->wrap();
          if (drawing_base->type != GP_DRAWING) {
            drawing.base.shape_key_edit_index = 0;
            continue;
          }

          /* Store the base geometry by copying the #CurvesGeometry object. (Note that this uses
           * implicit sharing, so the copying is delayed until a geometry attribute changes.)
           * The base geometry is used to compute the deltas when we finish shape key editing. */
          edit_data.base_geometry[drawing_i] = drawing.strokes();
          drawing.base.shape_key_edit_index = drawing_i + 1;

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

          /* Apply the edited shape key to the drawing, so we can measure deltas when we finish
           * editing. */
          IndexMask all_strokes(IndexRange(drawing.strokes().curves_num()));
          apply_shape_keys_to_drawing(drawing, edited_shape_key, factor, all_strokes);
        }
      });

  /* Add an undo step, allowing the user to undo the first action while editing without leaving
   * edit mode immediately. */
  ED_undo_push(C, "Start Edit Shape Key");

  DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
}

static wmOperatorStatus edit_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  /* Check for a Grease Pencil object with the #GREASE_PENCIL_SHAPE_KEY_IS_EDITED flag enabled.
   * When this flag isn't found, it means the user undoed 'out of' shape key editing. In that case
   * we cancel the editing. */
  if (!ELEM(event->type, MOUSEMOVE, INBETWEEN_MOUSEMOVE)) {
    ShapeKeyEditData &edit_data = *static_cast<ShapeKeyEditData *>(op->customdata);
    if (!ensure_valid_grease_pencil_of_edited_shapekey(C, edit_data)) {
      edit_cancel(C, op);
      return OPERATOR_FINISHED;
    }
  }

  /* Operator will end when the shape key 'edit state' is changed by the 'Finish Edit' or
   * 'Cancel Edit' operator. */
  if (edit_state == ShapeKeyEditState::Cancelled) {
    edit_cancel(C, op);
    return OPERATOR_FINISHED;
  }
  if (edit_state == ShapeKeyEditState::Inactive) {
    /* Grab all the shape key deltas and wrap up shape key edit mode. */
    ShapeKeyEditData &edit_data = *static_cast<ShapeKeyEditData *>(op->customdata);
    ensure_valid_grease_pencil_of_edited_shapekey(C, edit_data);
    get_shape_key_deltas(edit_data);
    edit_exit(C, op);
    return OPERATOR_FINISHED;
  }

  return OPERATOR_PASS_THROUGH;
}

static wmOperatorStatus edit_exec(bContext *C, wmOperator *op)
{
  /* Initialize the shape key edit mode. */
  edit_init(C, op);

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
  ot->cancel = edit_cancel;
}

static wmOperatorStatus edit_finish_exec(bContext * /*C*/, wmOperator * /*op*/)
{
  edit_state = ShapeKeyEditState::Inactive;
  return OPERATOR_FINISHED;
}

static bool edit_finish_poll(bContext * /*C*/)
{
  return edit_state == ShapeKeyEditState::Active;
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

static wmOperatorStatus edit_cancel_exec(bContext * /*C*/, wmOperator * /*op*/)
{
  edit_state = ShapeKeyEditState::Cancelled;
  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_shape_key_edit_cancel(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Cancel Edit Shape Key";
  ot->idname = "GREASE_PENCIL_OT_shape_key_edit_cancel";
  ot->description =
      "Cancel the editing of the active shape key, reverting all changes made to the shape key";
  ot->flag = OPTYPE_REGISTER;

  /* Callbacks. */
  ot->poll = edit_finish_poll;
  ot->exec = edit_cancel_exec;
}

static wmOperatorStatus new_from_mix_exec(bContext *C, wmOperator *op)
{
  using namespace bke::greasepencil;

  /* Create a new shape key, based on the active one. */
  if (add_exec(C, op) & OPERATOR_CANCELLED) {
    return OPERATOR_CANCELLED;
  }

  GreasePencil &grease_pencil = *from_context(*C);
  GreassePencilShapeKey *shape_key = BKE_grease_pencil_shape_key_active_get(&grease_pencil);
  shape_key->value = 0.0f;

  ShapeKeyEditData edit_data = {&grease_pencil, grease_pencil.active_shape_key_index};

  /* Store the base layers. */
  store_base_layers(edit_data);

  /* Apply all active shape keys to the layers. */
  Vector<int> shape_key_indices;
  Vector<float> shape_key_factors;
  int shape_key_index;
  LISTBASE_FOREACH_INDEX (
      GreasePencilShapeKey *, shape_key, &grease_pencil.shape_keys, shape_key_index)
  {
    if ((shape_key->value == 0.0f) || (shape_key->flag & GREASE_PENCIL_SHAPE_KEY_MUTED) != 0) {
      continue;
    }
    shape_key_indices.append(shape_key_index);
    shape_key_factors.append(shape_key->value);
  }
  if (!shape_key_indices.is_empty()) {
    IndexMask all_layers(IndexRange(grease_pencil.layers().size()));
    apply_shape_keys_to_layers(grease_pencil, shape_key_indices, shape_key_factors, all_layers);
  }

  /* Store the base drawings and apply the active shape keys. */
  edit_data.base_geometry.reinitialize(grease_pencil.drawings().size());
  threading::parallel_for(
      grease_pencil.drawings().index_range(), 1, [&](const IndexRange drawing_range) {
        for (const int drawing_i : drawing_range) {
          GreasePencilDrawingBase *drawing_base = grease_pencil.drawing(drawing_i);
          Drawing &drawing = (reinterpret_cast<GreasePencilDrawing *>(drawing_base))->wrap();
          if (drawing_base->type != GP_DRAWING) {
            drawing.base.shape_key_edit_index = 0;
            continue;
          }

          /* Store the base geometry (a full copy). */
          edit_data.base_geometry[drawing_i] = drawing.strokes();
          drawing.base.shape_key_edit_index = drawing_i + 1;

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

          /* Apply all active shape keys to the drawing. */
          if (!shape_key_indices.is_empty()) {
            IndexMask all_strokes(IndexRange(drawing.strokes().curves_num()));
            apply_shape_keys_to_drawing(
                drawing, shape_key_indices, shape_key_factors, all_strokes);
          }
        }
      });

  /* Store layer and drawing deltas. This also restores drawings to their base values. */
  get_shape_key_deltas(edit_data);

  /* Restore base layers. */
  restore_base_layers(edit_data);

  /* Remove temporary stroke index attributes. */
  remove_stroke_index_attributes(edit_data);

  DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_shape_key_new_from_mix(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "New Shape Key from Mix";
  ot->idname = "GREASE_PENCIL_OT_shape_key_new_from_mix";
  ot->description = "Create a new shape key based on the current mix of active shape keys";
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Callbacks. */
  ot->poll = active_poll;
  ot->exec = new_from_mix_exec;

  /* Properties. */
  PropertyRNA *prop;
  prop = RNA_def_string(ot->srna, "name", nullptr, MAX_NAME, "Name", "Name of the new shape key");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  ot->prop = prop;
}

bool new_from_modifier(Object *object,
                       ModifierData *md_eval_in,
                       Main *bmain,
                       Scene *scene,
                       Depsgraph *depsgraph,
                       ReportList *reports)
{
  using namespace bke;
  using namespace bke::greasepencil;

  WM_cursor_wait(true);

  /* Use the original modifier data, as the evaluated one will become invalid when the scene graph
   * is updated for the next keyframe. */
  ModifierData *md = BKE_modifier_get_original(object, md_eval_in);

  /* Add new shape key. */
  GreasePencil *grease_pencil_orig = static_cast<GreasePencil *>(object->data);
  GreasePencilShapeKey *shape_key = static_cast<GreasePencilShapeKey *>(
      MEM_callocN(sizeof(GreasePencilShapeKey), __func__));
  BLI_addtail(&grease_pencil_orig->shape_keys, shape_key);
  shape_key->range_min = 0.0f;
  shape_key->range_max = 1.0f;
  shape_key->value = 0.0f;
  shape_key->pass_index = 0;
  BLI_strncpy(shape_key->name, DATA_(md->name), sizeof(shape_key->name));
  BLI_uniquename(&grease_pencil_orig->shape_keys,
                 shape_key,
                 DATA_("ShapeKey"),
                 '.',
                 offsetof(GreasePencilShapeKey, name),
                 sizeof(shape_key->name));
  const int shape_key_index = BLI_findindex(&grease_pencil_orig->shape_keys, shape_key);
  BKE_grease_pencil_shape_key_active_set(grease_pencil_orig, shape_key_index);

  /* Add a shape key modifier automatically when there isn't one. */
  add_shape_key_modifier(object, bmain, scene, reports);

  /* Collect and sort all keyframes. */
  VectorSet<int> frame_numbers;
  for (const int layer_i : grease_pencil_orig->layers().index_range()) {
    const Layer &layer = grease_pencil_orig->layer(layer_i);
    for (const auto [frame_number, frame] : layer.frames().items()) {
      frame_numbers.add(frame_number);
    }
  }
  Array<int> sorted_frame_times(frame_numbers.size());
  int i = 0;
  for (const int key : frame_numbers.as_span()) {
    sorted_frame_times[i++] = key;
  }
  std::sort(sorted_frame_times.begin(), sorted_frame_times.end());

  /* Loop over all keyframes. */
  const int start_frame = int(DEG_get_ctime(depsgraph));
  bool changed = false;
  for (const int eval_frame : sorted_frame_times) {
    scene->r.cfra = eval_frame;
    BKE_scene_graph_update_for_newframe(depsgraph);

    /* Create a temporary Grease Pencil object. */
    Object *ob_eval = DEG_get_evaluated(depsgraph, object);
    GreasePencil *grease_pencil_eval = ob_eval->runtime->data_orig ?
                                           reinterpret_cast<GreasePencil *>(
                                               ob_eval->runtime->data_orig) :
                                           grease_pencil_orig;
    const int eval_frame = int(DEG_get_ctime(depsgraph));
    GreasePencil *grease_pencil_temp = reinterpret_cast<GreasePencil *>(
        BKE_id_copy_ex(nullptr, &grease_pencil_eval->id, nullptr, LIB_ID_COPY_LOCALIZE));
    grease_pencil_temp->runtime->eval_frame = eval_frame;

    /* Get the drawings at this frame. */
    for (GreasePencilDrawingBase *drawing_base : grease_pencil_temp->drawings()) {
      drawing_base->shape_key_edit_index = 0;
    }
    const Vector<Drawing *> drawings = ed::greasepencil::retrieve_visible_drawings_at_frame(
        *scene, *grease_pencil_temp, eval_frame);

    /* Store the base layers and drawings. */
    ShapeKeyEditData edit_data = {grease_pencil_temp, shape_key_index};
    store_base_layers(edit_data);
    edit_data.base_geometry.reinitialize(drawings.size());
    threading::parallel_for(drawings.index_range(), 1, [&](const IndexRange drawing_range) {
      for (const int drawing_i : drawing_range) {
        Drawing &drawing = *drawings[drawing_i];

        /* Store the base geometry (a full copy). */
        edit_data.base_geometry[drawing_i] = drawing.strokes();
        drawing.base.shape_key_edit_index = drawing_i + 1;

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
      }
    });

    /* Apply the modifier. */
    GeometrySet eval_geometry_set = GeometrySet::from_grease_pencil(grease_pencil_temp,
                                                                    GeometryOwnershipType::Owned);
    const ModifierTypeInfo *mti = BKE_modifier_get_info(ModifierType(md->type));
    ModifierData *md_eval = BKE_modifier_get_evaluated(depsgraph, object, md);
    ModifierEvalContext mectx = {depsgraph, ob_eval, MOD_APPLY_TO_ORIGINAL};
    mti->modify_geometry_set(md_eval, &mectx, &eval_geometry_set);
    if (!eval_geometry_set.has_grease_pencil()) {
      continue;
    }
    GreasePencil &grease_pencil_modified =
        *eval_geometry_set.get_component_for_write<GreasePencilComponent>().get_for_write();

    /* Get the shape key deltas from the modified Grease Pencil object. */
    edit_data.grease_pencil = &grease_pencil_modified;
    const Vector<Drawing *> drawings_modified =
        ed::greasepencil::retrieve_visible_drawings_at_frame(
            *scene, grease_pencil_modified, eval_frame);
    const Vector<Drawing *> drawings_orig = ed::greasepencil::retrieve_visible_drawings_at_frame(
        *scene, *grease_pencil_orig, eval_frame);
    get_shape_key_layer_deltas(edit_data, grease_pencil_orig);
    get_shape_key_stroke_deltas(edit_data, drawings_modified, true, drawings_orig);
    changed = true;

    scene->r.cfra = start_frame;
    BKE_scene_graph_update_for_newframe(depsgraph);
  }

  WM_cursor_wait(false);

  return changed;
}

}  // namespace blender::ed::greasepencil::shape_key

bool ED_grease_pencil_shape_key_in_edit_mode()
{
  return blender::ed::greasepencil::shape_key::edit_state ==
         blender::ed::greasepencil::shape_key::ShapeKeyEditState::Active;
}

void ED_operatortypes_grease_pencil_shape_keys()
{
  using namespace blender::ed::greasepencil::shape_key;
  WM_operatortype_append(GREASE_PENCIL_OT_shape_key_add);
  WM_operatortype_append(GREASE_PENCIL_OT_shape_key_move);
  WM_operatortype_append(GREASE_PENCIL_OT_shape_key_duplicate);
  WM_operatortype_append(GREASE_PENCIL_OT_shape_key_new_from_mix);
  WM_operatortype_append(GREASE_PENCIL_OT_shape_key_edit);
  WM_operatortype_append(GREASE_PENCIL_OT_shape_key_edit_finish);
  WM_operatortype_append(GREASE_PENCIL_OT_shape_key_edit_cancel);
  WM_operatortype_append(GREASE_PENCIL_OT_shape_key_remove);
  WM_operatortype_append(GREASE_PENCIL_OT_shape_key_remove_all);
  WM_operatortype_append(GREASE_PENCIL_OT_shape_key_clear);
}
