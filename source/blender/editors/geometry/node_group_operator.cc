/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurves
 */

#include "BLI_listbase.h"
#include "BLI_path_utils.hh"
#include "BLI_rect.h"
#include "BLI_string.h"

#include "ED_curves.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_select_utils.hh"
#include "ED_view3d.hh"

#include "WM_api.hh"

#include "BKE_asset.hh"
#include "BKE_compute_context_cache.hh"
#include "BKE_compute_contexts.hh"
#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_customdata.hh"
#include "BKE_editmesh.hh"
#include "BKE_geometry_set.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_wrapper.hh"
#include "BKE_node_runtime.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_pointcloud.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"
#include "BKE_screen.hh"
#include "BKE_workspace.hh"

#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "ED_asset.hh"
#include "ED_asset_menu_utils.hh"
#include "ED_geometry.hh"
#include "ED_mesh.hh"
#include "ED_sculpt.hh"

#include "BLT_translation.hh"

#include "NOD_geometry_nodes_dependencies.hh"
#include "NOD_geometry_nodes_execute.hh"
#include "NOD_geometry_nodes_lazy_function.hh"
#include "NOD_socket_usage_inference.hh"

#include "AS_asset_catalog.hh"
#include "AS_asset_catalog_path.hh"
#include "AS_asset_catalog_tree.hh"
#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "geometry_intern.hh"

#include <fmt/format.h>

namespace geo_log = blender::nodes::geo_eval_log;

namespace blender::ed::geometry {

/* -------------------------------------------------------------------- */
/** \name Operator
 * \{ */

static const bNodeTree *get_asset_or_local_node_group(const bContext &C,
                                                      PointerRNA &ptr,
                                                      ReportList *reports)
{
  Main &bmain = *CTX_data_main(&C);
  if (bNodeTree *group = reinterpret_cast<bNodeTree *>(
          WM_operator_properties_id_lookup_from_name_or_session_uid(&bmain, &ptr, ID_NT)))
  {
    return group;
  }

  const asset_system::AssetRepresentation *asset =
      asset::operator_asset_reference_props_get_asset_from_all_library(C, ptr, reports);
  if (!asset) {
    return nullptr;
  }
  return reinterpret_cast<bNodeTree *>(asset::asset_local_id_ensure_imported(bmain, *asset));
}

static const bNodeTree *get_node_group(const bContext &C, PointerRNA &ptr, ReportList *reports)
{
  const bNodeTree *group = get_asset_or_local_node_group(C, ptr, reports);
  if (!group) {
    return nullptr;
  }
  if (group->type != NTREE_GEOMETRY) {
    if (reports) {
      BKE_report(reports, RPT_ERROR, "Asset is not a geometry node group");
    }
    return nullptr;
  }
  return group;
}

GeoOperatorLog::~GeoOperatorLog() = default;

/**
 * The socket value log is stored statically so it can be used in the node editor. A fancier
 * storage system shouldn't be necessary, since the goal is just to be able to debug intermediate
 * values when building a tool.
 */
static GeoOperatorLog &get_static_eval_log()
{
  static GeoOperatorLog log;
  return log;
}

const GeoOperatorLog &node_group_operator_static_eval_log()
{
  return get_static_eval_log();
}

/** Find all the visible node editors to log values for. */
static void find_socket_log_contexts(const Main &bmain,
                                     Set<ComputeContextHash> &r_socket_log_contexts)
{
  wmWindowManager *wm = static_cast<wmWindowManager *>(bmain.wm.first);
  if (wm == nullptr) {
    return;
  }
  LISTBASE_FOREACH (const wmWindow *, window, &wm->windows) {
    const bScreen *screen = BKE_workspace_active_screen_get(window->workspace_hook);
    LISTBASE_FOREACH (const ScrArea *, area, &screen->areabase) {
      const SpaceLink *sl = static_cast<SpaceLink *>(area->spacedata.first);
      if (sl->spacetype == SPACE_NODE) {
        const SpaceNode &snode = *reinterpret_cast<const SpaceNode *>(sl);
        if (snode.edittree == nullptr) {
          continue;
        }
        if (snode.geometry_nodes_type != SNODE_GEOMETRY_TOOL) {
          continue;
        }
        bke::ComputeContextCache compute_context_cache;
        const Map<const bke::bNodeTreeZone *, ComputeContextHash> hash_by_zone =
            geo_log::GeoModifierLog::get_context_hash_by_zone_for_node_editor(
                snode, compute_context_cache);
        for (const ComputeContextHash &hash : hash_by_zone.values()) {
          r_socket_log_contexts.add(hash);
        }
      }
    }
  }
}

/**
 * This class adds a user to shared mesh data, requiring modifications of the mesh to reallocate
 * the data and its sharing info. This allows tracking which data is modified without having to
 * explicitly compare it.
 */
class MeshState {
  VectorSet<const ImplicitSharingInfo *> sharing_infos_;

 public:
  MeshState(const Mesh &mesh)
  {
    if (mesh.runtime->face_offsets_sharing_info) {
      this->freeze_shared_state(*mesh.runtime->face_offsets_sharing_info);
    }
    mesh.attributes().foreach_attribute([&](const bke::AttributeIter &iter) {
      const bke::GAttributeReader attribute = iter.get();
      if (attribute.varray.size() == 0) {
        return;
      }
      this->freeze_shared_state(*attribute.sharing_info);
    });
  }

  void freeze_shared_state(const ImplicitSharingInfo &sharing_info)
  {
    if (sharing_infos_.add(&sharing_info)) {
      sharing_info.add_user();
    }
  }

  ~MeshState()
  {
    for (const ImplicitSharingInfo *sharing_info : sharing_infos_) {
      sharing_info->remove_user_and_delete_if_last();
    }
  }
};

/**
 * Geometry nodes currently requires working on "evaluated" data-blocks (rather than "original"
 * data-blocks that are part of a #Main data-base). This could change in the future, but for now,
 * we need to create evaluated copies of geometry before passing it to geometry nodes. Implicit
 * sharing lets us avoid copying attribute data though.
 */
static bke::GeometrySet get_original_geometry_eval_copy(Depsgraph &depsgraph,
                                                        Object &object,
                                                        nodes::GeoNodesOperatorData &operator_data,
                                                        Vector<MeshState> &orig_mesh_states)
{
  switch (object.type) {
    case OB_CURVES: {
      Curves *curves = BKE_curves_copy_for_eval(static_cast<const Curves *>(object.data));
      return bke::GeometrySet::from_curves(curves);
    }
    case OB_POINTCLOUD: {
      PointCloud *points = BKE_pointcloud_copy_for_eval(
          static_cast<const PointCloud *>(object.data));
      return bke::GeometrySet::from_pointcloud(points);
    }
    case OB_MESH: {
      const Mesh *mesh = static_cast<const Mesh *>(object.data);
      if (std::shared_ptr<BMEditMesh> &em = mesh->runtime->edit_mesh) {
        operator_data.active_point_index = BM_mesh_active_vert_index_get(em->bm);
        operator_data.active_edge_index = BM_mesh_active_edge_index_get(em->bm);
        operator_data.active_face_index = BM_mesh_active_face_index_get(em->bm, false, true);
        Mesh *mesh_copy = BKE_mesh_wrapper_from_editmesh(em, nullptr, mesh);
        BKE_mesh_wrapper_ensure_mdata(mesh_copy);
        Mesh *final_copy = BKE_mesh_copy_for_eval(*mesh_copy);
        BKE_id_free(nullptr, mesh_copy);
        return bke::GeometrySet::from_mesh(final_copy);
      }
      if (bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object)) {
        /* Currently many sculpt mode operations do not tag normals dirty (see use of
         * #Mesh::tag_positions_changed_no_normals()), so access within geometry nodes cannot
         * know that normals are out of date and recalculate them. Update them here instead. */
        bke::pbvh::update_normals(depsgraph, object, *pbvh);
      }
      Mesh *mesh_copy = BKE_mesh_copy_for_eval(*mesh);
      orig_mesh_states.append_as(*mesh_copy);
      return bke::GeometrySet::from_mesh(mesh_copy);
    }
    default:
      return {};
  }
}

static void store_result_geometry(const wmOperator &op,
                                  const Depsgraph &depsgraph,
                                  Main &bmain,
                                  Scene &scene,
                                  Object &object,
                                  const RegionView3D *rv3d,
                                  bke::GeometrySet geometry)
{
  geometry.ensure_owns_direct_data();
  switch (object.type) {
    case OB_CURVES: {
      Curves &curves = *static_cast<Curves *>(object.data);
      Curves *new_curves = geometry.get_curves_for_write();
      if (!new_curves) {
        curves.geometry.wrap() = {};
        break;
      }

      /* Anonymous attributes shouldn't be available on the applied geometry. */
      new_curves->geometry.wrap().attributes_for_write().remove_anonymous();

      curves.geometry.wrap() = std::move(new_curves->geometry.wrap());
      BKE_object_material_from_eval_data(&bmain, &object, &new_curves->id);
      DEG_id_tag_update(&curves.id, ID_RECALC_GEOMETRY);
      break;
    }
    case OB_POINTCLOUD: {
      PointCloud &points = *static_cast<PointCloud *>(object.data);
      PointCloud *new_points =
          geometry.get_component_for_write<bke::PointCloudComponent>().release();
      if (!new_points) {
        CustomData_free(&points.pdata);
        points.totpoint = 0;
        break;
      }

      /* Anonymous attributes shouldn't be available on the applied geometry. */
      new_points->attributes_for_write().remove_anonymous();

      BKE_object_material_from_eval_data(&bmain, &object, &new_points->id);
      BKE_pointcloud_nomain_to_pointcloud(new_points, &points);
      DEG_id_tag_update(&points.id, ID_RECALC_GEOMETRY);
      break;
    }
    case OB_MESH: {
      Mesh &mesh = *static_cast<Mesh *>(object.data);

      const bool has_shape_keys = mesh.key != nullptr;

      Mesh *new_mesh = geometry.get_component_for_write<bke::MeshComponent>().release();
      if (new_mesh) {
        /* Anonymous attributes shouldn't be available on the applied geometry. */
        new_mesh->attributes_for_write().remove_anonymous();
        BKE_object_material_from_eval_data(&bmain, &object, &new_mesh->id);
      }
      else {
        new_mesh = BKE_mesh_new_nomain(0, 0, 0, 0);
      }

      if (object.mode == OB_MODE_SCULPT) {
        sculpt_paint::store_mesh_from_eval(op, scene, depsgraph, rv3d, object, new_mesh);
      }
      else if (object.mode == OB_MODE_EDIT) {
        EDBM_mesh_make_from_mesh(&object, new_mesh, scene.toolsettings->selectmode, true);
        BKE_editmesh_looptris_and_normals_calc(mesh.runtime->edit_mesh.get());
        BKE_id_free(nullptr, new_mesh);
        DEG_id_tag_update(&mesh.id, ID_RECALC_GEOMETRY);
      }
      else {
        BKE_mesh_nomain_to_mesh(new_mesh, &mesh, &object);
        DEG_id_tag_update(&mesh.id, ID_RECALC_GEOMETRY);
      }

      if (has_shape_keys && !mesh.key) {
        BKE_report(op.reports, RPT_WARNING, "Mesh shape key data removed");
      }
      break;
    }
  }
}

/**
 * Gather IDs used by the node group, and the node group itself if there are any. We need to use
 * *all* IDs because the only mechanism we have to replace the socket ID pointers with their
 * evaluated counterparts is evaluating the node group data-block itself.
 */
static void gather_node_group_ids(const bNodeTree &node_tree, Set<ID *> &ids)
{
  const int orig_size = ids.size();
  BLI_assert(node_tree.runtime->geometry_nodes_eval_dependencies);
  for (ID *id : node_tree.runtime->geometry_nodes_eval_dependencies->ids.values()) {
    ids.add(id);
  }
  if (ids.size() != orig_size) {
    /* Only evaluate the node group if it references data-blocks. In that case it needs to be
     * evaluated so that ID pointers are switched to point to evaluated data-blocks. */
    ids.add(const_cast<ID *>(&node_tree.id));
  }
}

static const bNodeTreeInterfaceSocket *find_group_input_by_identifier(const bNodeTree &node_group,
                                                                      const StringRef identifier)
{
  for (const bNodeTreeInterfaceSocket *input : node_group.interface_inputs()) {
    if (input->identifier == identifier) {
      return input;
    }
  }
  return nullptr;
}

static std::optional<ID_Type> socket_type_to_id_type(const eNodeSocketDatatype socket_type)
{
  switch (socket_type) {
    case SOCK_CUSTOM:
    case SOCK_FLOAT:
    case SOCK_VECTOR:
    case SOCK_RGBA:
    case SOCK_SHADER:
    case SOCK_BOOLEAN:
    case SOCK_INT:
    case SOCK_STRING:
    case SOCK_GEOMETRY:
    case SOCK_ROTATION:
    case SOCK_MENU:
    case SOCK_MATRIX:
    case SOCK_BUNDLE:
    case SOCK_CLOSURE:
      return std::nullopt;
    case SOCK_OBJECT:
      return ID_OB;
    case SOCK_IMAGE:
      return ID_IM;
    case SOCK_COLLECTION:
      return ID_GR;
    case SOCK_TEXTURE:
      return ID_TE;
    case SOCK_MATERIAL:
      return ID_MA;
  }
  return std::nullopt;
}

/**
 * Gather IDs referenced from node group input properties (the redo panel). In the end, the group
 * input properties will be copied to contain evaluated data-blocks from the active and/or an extra
 * depsgraph.
 */
static Map<StringRef, ID *> gather_input_ids(const Main &bmain,
                                             const bNodeTree &node_group,
                                             const IDProperty &properties)
{
  Map<StringRef, ID *> ids;
  IDP_foreach_property(
      &const_cast<IDProperty &>(properties), IDP_TYPE_FILTER_STRING, [&](IDProperty *prop) {
        const bNodeTreeInterfaceSocket *input = find_group_input_by_identifier(node_group,
                                                                               prop->name);
        if (!input) {
          return;
        }
        const std::optional<ID_Type> id_type = socket_type_to_id_type(
            eNodeSocketDatatype(input->socket_typeinfo()->type));
        if (!id_type) {
          return;
        }
        const char *id_name = IDP_String(prop);
        ID *id = BKE_libblock_find_name(&const_cast<Main &>(bmain), *id_type, id_name);
        if (!id) {
          return;
        }
        ids.add(prop->name, id);
      });
  return ids;
}

static Depsgraph *build_extra_depsgraph(const Depsgraph &depsgraph_active, const Set<ID *> &ids)
{
  Depsgraph *depsgraph = DEG_graph_new(DEG_get_bmain(&depsgraph_active),
                                       DEG_get_input_scene(&depsgraph_active),
                                       DEG_get_input_view_layer(&depsgraph_active),
                                       DEG_get_mode(&depsgraph_active));
  DEG_graph_build_from_ids(depsgraph, Vector<ID *>(ids.begin(), ids.end()));
  DEG_evaluate_on_refresh(depsgraph);
  return depsgraph;
}

static IDProperty *replace_strings_with_id_pointers(const IDProperty &op_properties,
                                                    const Map<StringRef, ID *> &input_ids)
{
  IDProperty *properties = bke::idprop::create_group("Exec Properties").release();
  IDP_foreach_property(&const_cast<IDProperty &>(op_properties), 0, [&](IDProperty *prop) {
    if (ID *id = input_ids.lookup_default(prop->name, nullptr)) {
      IDP_AddToGroup(properties, bke::idprop::create(prop->name, id).release());
    }
    else {
      IDP_AddToGroup(properties, IDP_CopyProperty(prop));
    }
  });
  return properties;
}

static void replace_inputs_evaluated_data_blocks(
    IDProperty &properties, const nodes::GeoNodesOperatorDepsgraphs &depsgraphs)
{
  IDP_foreach_property(&properties, IDP_TYPE_FILTER_ID, [&](IDProperty *property) {
    if (ID *id = IDP_Id(property)) {
      if (ID_TYPE_USE_COPY_ON_EVAL(GS(id->name))) {
        property->data.pointer = const_cast<ID *>(depsgraphs.get_evaluated_id(*id));
      }
    }
  });
}

static bool object_has_editable_data(const Main &bmain, const Object &object)
{
  if (!ELEM(object.type, OB_CURVES, OB_POINTCLOUD, OB_MESH)) {
    return false;
  }
  if (!BKE_id_is_editable(&bmain, static_cast<const ID *>(object.data))) {
    return false;
  }
  return true;
}

static Vector<Object *> gather_supported_objects(const bContext &C,
                                                 const Main &bmain,
                                                 const eObjectMode mode)
{
  Vector<Object *> objects;
  Set<const ID *> unique_object_data;

  auto handle_object = [&](Object *object) {
    if (object->mode != mode) {
      return;
    }
    if (!unique_object_data.add(static_cast<const ID *>(object->data))) {
      return;
    }
    if (!object_has_editable_data(bmain, *object)) {
      return;
    }
    objects.append(object);
  };

  if (mode == OB_MODE_OBJECT) {
    CTX_DATA_BEGIN (&C, Object *, object, selected_objects) {
      handle_object(object);
    }
    CTX_DATA_END;
  }
  else {
    Scene *scene = CTX_data_scene(&C);
    ViewLayer *view_layer = CTX_data_view_layer(&C);
    View3D *v3d = CTX_wm_view3d(&C);
    Object *active_object = CTX_data_active_object(&C);
    if (v3d && active_object) {
      FOREACH_OBJECT_IN_MODE_BEGIN (scene, view_layer, v3d, active_object->type, mode, ob) {
        handle_object(ob);
      }
      FOREACH_OBJECT_IN_MODE_END;
    }
  }
  return objects;
}

static wmOperatorStatus run_node_group_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  Object *active_object = CTX_data_active_object(C);
  if (!active_object) {
    return OPERATOR_CANCELLED;
  }
  const eObjectMode mode = eObjectMode(active_object->mode);

  const bNodeTree *node_tree_orig = get_node_group(*C, *op->ptr, op->reports);
  if (!node_tree_orig) {
    return OPERATOR_CANCELLED;
  }

  const Vector<Object *> objects = gather_supported_objects(*C, *bmain, mode);

  Depsgraph *depsgraph_active = CTX_data_ensure_evaluated_depsgraph(C);
  Set<ID *> extra_ids;
  gather_node_group_ids(*node_tree_orig, extra_ids);
  const Map<StringRef, ID *> input_ids = gather_input_ids(
      *bmain, *node_tree_orig, *op->properties);
  for (ID *id : input_ids.values()) {
    /* Skip IDs that are already fully evaluated in the active depsgraph. */
    if (!DEG_id_is_fully_evaluated(depsgraph_active, id)) {
      extra_ids.add(id);
    }
  }

  const nodes::GeoNodesOperatorDepsgraphs depsgraphs{
      depsgraph_active,
      extra_ids.is_empty() ? nullptr : build_extra_depsgraph(*depsgraph_active, extra_ids),
  };

  IDProperty *properties = replace_strings_with_id_pointers(*op->properties, input_ids);
  BLI_SCOPED_DEFER([&]() { IDP_FreeProperty_ex(properties, false); });

  replace_inputs_evaluated_data_blocks(*properties, depsgraphs);

  const bNodeTree *node_tree = nullptr;
  if (depsgraphs.extra) {
    node_tree = DEG_get_evaluated(depsgraphs.extra, node_tree_orig);
  }
  else {
    node_tree = node_tree_orig;
  }

  const nodes::GeometryNodesLazyFunctionGraphInfo *lf_graph_info =
      nodes::ensure_geometry_nodes_lazy_function_graph(*node_tree);
  if (lf_graph_info == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Cannot evaluate node group");
    return OPERATOR_CANCELLED;
  }

  if (!node_tree->group_output_node()) {
    BKE_report(op->reports, RPT_ERROR, "Node group must have a group output node");
    return OPERATOR_CANCELLED;
  }
  if (node_tree->interface_outputs().is_empty() ||
      !STREQ(node_tree->interface_outputs()[0]->socket_type, "NodeSocketGeometry"))
  {
    BKE_report(op->reports, RPT_ERROR, "Node group's first output must be a geometry");
    return OPERATOR_CANCELLED;
  }

  bke::OperatorComputeContext compute_context;
  Set<ComputeContextHash> socket_log_contexts;
  GeoOperatorLog &eval_log = get_static_eval_log();
  eval_log.log = std::make_unique<geo_log::GeoModifierLog>();
  eval_log.node_group_name = node_tree->id.name + 2;
  find_socket_log_contexts(*bmain, socket_log_contexts);

  /* May be null if operator called from outside 3D view context. */
  const RegionView3D *rv3d = CTX_wm_region_view3d(C);
  Vector<MeshState> orig_mesh_states;

  for (Object *object : objects) {
    nodes::GeoNodesOperatorData operator_eval_data{};
    operator_eval_data.mode = mode;
    operator_eval_data.depsgraphs = &depsgraphs;
    operator_eval_data.self_object_orig = object;
    operator_eval_data.scene_orig = scene;
    RNA_int_get_array(op->ptr, "mouse_position", operator_eval_data.mouse_position);
    RNA_int_get_array(op->ptr, "region_size", operator_eval_data.region_size);
    RNA_float_get_array(op->ptr, "cursor_position", operator_eval_data.cursor_position);
    RNA_float_get_array(op->ptr, "cursor_rotation", &operator_eval_data.cursor_rotation.w);
    RNA_float_get_array(
        op->ptr, "viewport_projection_matrix", operator_eval_data.viewport_winmat.base_ptr());
    RNA_float_get_array(
        op->ptr, "viewport_view_matrix", operator_eval_data.viewport_viewmat.base_ptr());
    operator_eval_data.viewport_is_perspective = RNA_boolean_get(op->ptr,
                                                                 "viewport_is_perspective");

    nodes::GeoNodesCallData call_data{};
    call_data.operator_data = &operator_eval_data;
    call_data.eval_log = eval_log.log.get();
    if (object == active_object) {
      /* Only log values from the active object. */
      call_data.socket_log_contexts = &socket_log_contexts;
    }

    bke::GeometrySet geometry_orig = get_original_geometry_eval_copy(
        *depsgraph_active, *object, operator_eval_data, orig_mesh_states);

    bke::GeometrySet new_geometry = nodes::execute_geometry_nodes_on_geometry(
        *node_tree,
        nodes::build_properties_vector_set(properties),
        compute_context,
        call_data,
        std::move(geometry_orig));

    store_result_geometry(
        *op, *depsgraph_active, *bmain, *scene, *object, rv3d, std::move(new_geometry));
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, object->data);
  }

  geo_log::GeoTreeLog &tree_log = eval_log.log->get_tree_log(compute_context.hash());
  tree_log.ensure_node_warnings(*bmain);
  for (const geo_log::NodeWarning &warning : tree_log.all_warnings) {
    if (warning.type == geo_log::NodeWarningType::Info) {
      BKE_report(op->reports, RPT_INFO, warning.message.c_str());
    }
    else {
      BKE_report(op->reports, RPT_WARNING, warning.message.c_str());
    }
  }

  return OPERATOR_FINISHED;
}

/**
 * Input node values are stored as operator properties in order to support redoing from the redo
 * panel for a few reasons:
 *  1. Some data (like the mouse position) cannot be retrieved from the `exec` callback used for
 *     operator redo. Redo is meant to just call the operator again with the exact same properties.
 *  2. While adjusting an input in the redo panel, the user doesn't expect anything else to change.
 *     If we retrieve other data like the viewport transform on every execution, that won't be the
 *     case.
 * We use operator RNA properties instead of operator custom data because the custom data struct
 * isn't maintained for the redo `exec` call.
 */
static void store_input_node_values_rna_props(const bContext &C,
                                              wmOperator &op,
                                              const wmEvent &event)
{
  Scene *scene = CTX_data_scene(&C);
  /* NOTE: `region` and `rv3d` may be null when called from a script. */
  const ARegion *region = CTX_wm_region(&C);
  const RegionView3D *rv3d = CTX_wm_region_view3d(&C);

  /* Mouse position node inputs. */
  RNA_int_set_array(op.ptr, "mouse_position", event.mval);
  RNA_int_set_array(
      op.ptr,
      "region_size",
      region ? int2(BLI_rcti_size_x(&region->winrct), BLI_rcti_size_y(&region->winrct)) : int2(0));

  /* 3D cursor node inputs. */
  const View3DCursor &cursor = scene->cursor;
  RNA_float_set_array(op.ptr, "cursor_position", cursor.location);
  math::Quaternion cursor_rotation = cursor.rotation();
  RNA_float_set_array(op.ptr, "cursor_rotation", &cursor_rotation.w);

  /* Viewport transform node inputs. */
  RNA_float_set_array(op.ptr,
                      "viewport_projection_matrix",
                      rv3d ? float4x4(rv3d->winmat).base_ptr() : float4x4::identity().base_ptr());
  RNA_float_set_array(op.ptr,
                      "viewport_view_matrix",
                      rv3d ? float4x4(rv3d->viewmat).base_ptr() : float4x4::identity().base_ptr());
  RNA_boolean_set(op.ptr, "viewport_is_perspective", rv3d ? bool(rv3d->is_persp) : true);
}

static wmOperatorStatus run_node_group_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  const bNodeTree *node_tree = get_node_group(*C, *op->ptr, op->reports);
  if (!node_tree) {
    return OPERATOR_CANCELLED;
  }

  store_input_node_values_rna_props(*C, *op, *event);

  nodes ::update_input_properties_from_node_tree(
      *node_tree, op->properties, *op->properties, true);
  nodes::update_output_properties_from_node_tree(*node_tree, op->properties, *op->properties);

  return run_node_group_exec(C, op);
}

static std::string run_node_group_get_description(bContext *C,
                                                  wmOperatorType * /*ot*/,
                                                  PointerRNA *ptr)
{
  const asset_system::AssetRepresentation *asset =
      asset::operator_asset_reference_props_get_asset_from_all_library(*C, *ptr, nullptr);
  if (!asset) {
    return "";
  }
  if (!asset->get_metadata().description) {
    return "";
  }
  return asset->get_metadata().description;
}

static void add_attribute_search_or_value_buttons(uiLayout *layout,
                                                  PointerRNA *md_ptr,
                                                  const StringRef socket_id_esc,
                                                  const StringRefNull rna_path,
                                                  const bNodeTreeInterfaceSocket &socket)
{
  bke::bNodeSocketType *typeinfo = bke::node_socket_type_find(socket.socket_type);
  const eNodeSocketDatatype socket_type = eNodeSocketDatatype(typeinfo->type);

  const std::string rna_path_use_attribute = fmt::format(
      "[\"{}{}\"]", socket_id_esc, nodes::input_use_attribute_suffix);
  const std::string rna_path_attribute_name = fmt::format(
      "[\"{}{}\"]", socket_id_esc, nodes::input_attribute_name_suffix);

  /* We're handling this manually in this case. */
  uiLayoutSetPropDecorate(layout, false);

  uiLayout *split = uiLayoutSplit(layout, 0.4f, false);
  uiLayout *name_row = &split->row(false);
  uiLayoutSetAlignment(name_row, UI_LAYOUT_ALIGN_RIGHT);

  const bool use_attribute = RNA_boolean_get(md_ptr, rna_path_use_attribute.c_str());
  if (socket_type == SOCK_BOOLEAN && !use_attribute) {
    uiItemL(name_row, "", ICON_NONE);
  }
  else {
    uiItemL(name_row, socket.name ? socket.name : "", ICON_NONE);
  }

  uiLayout *prop_row = &split->row(true);
  if (socket_type == SOCK_BOOLEAN) {
    uiLayoutSetPropSep(prop_row, false);
    uiLayoutSetAlignment(prop_row, UI_LAYOUT_ALIGN_EXPAND);
  }

  if (use_attribute) {
    /* TODO: Add attribute search. */
    uiItemR(prop_row, md_ptr, rna_path_attribute_name, UI_ITEM_NONE, "", ICON_NONE);
  }
  else {
    const char *name = socket_type == SOCK_BOOLEAN ? (socket.name ? socket.name : "") : "";
    uiItemR(prop_row, md_ptr, rna_path, UI_ITEM_NONE, name, ICON_NONE);
  }

  uiItemR(prop_row, md_ptr, rna_path_use_attribute, UI_ITEM_R_ICON_ONLY, "", ICON_SPREADSHEET);
}

static void draw_property_for_socket(const bNodeTree &node_tree,
                                     uiLayout *layout,
                                     const nodes::PropertiesVectorSet &properties_set,
                                     PointerRNA *bmain_ptr,
                                     PointerRNA *op_ptr,
                                     const bNodeTreeInterfaceSocket &socket,
                                     const int socket_index,
                                     const bool affects_output)
{
  bke::bNodeSocketType *typeinfo = bke::node_socket_type_find(socket.socket_type);
  const eNodeSocketDatatype socket_type = eNodeSocketDatatype(typeinfo->type);

  /* The property should be created in #MOD_nodes_update_interface with the correct type. */
  const IDProperty *property = properties_set.lookup_key_default_as(socket.identifier, nullptr);

  /* IDProperties can be removed with python, so there could be a situation where
   * there isn't a property for a socket or it doesn't have the correct type. */
  if (!property || !nodes::id_property_type_matches_socket(socket, *property, true)) {
    return;
  }

  char socket_id_esc[MAX_NAME * 2];
  BLI_str_escape(socket_id_esc, socket.identifier, sizeof(socket_id_esc));

  char rna_path[sizeof(socket_id_esc) + 4];
  SNPRINTF(rna_path, "[\"%s\"]", socket_id_esc);

  uiLayout *row = &layout->row(true);
  uiLayoutSetActive(row, affects_output);
  uiLayoutSetPropDecorate(row, false);

  /* Use #uiItemPointerR to draw pointer properties because #uiItemR would not have enough
   * information about what type of ID to select for editing the values. This is because
   * pointer IDProperties contain no information about their type. */
  const char *name = socket.name ? socket.name : "";
  switch (socket_type) {
    case SOCK_OBJECT:
      uiItemPointerR(row, op_ptr, rna_path, bmain_ptr, "objects", name, ICON_OBJECT_DATA);
      break;
    case SOCK_COLLECTION:
      uiItemPointerR(
          row, op_ptr, rna_path, bmain_ptr, "collections", name, ICON_OUTLINER_COLLECTION);
      break;
    case SOCK_MATERIAL:
      uiItemPointerR(row, op_ptr, rna_path, bmain_ptr, "materials", name, ICON_MATERIAL);
      break;
    case SOCK_TEXTURE:
      uiItemPointerR(row, op_ptr, rna_path, bmain_ptr, "textures", name, ICON_TEXTURE);
      break;
    case SOCK_IMAGE:
      uiItemPointerR(row, op_ptr, rna_path, bmain_ptr, "images", name, ICON_IMAGE);
      break;
    default:
      if (nodes::input_has_attribute_toggle(node_tree, socket_index)) {
        add_attribute_search_or_value_buttons(row, op_ptr, socket_id_esc, rna_path, socket);
      }
      else {
        uiItemR(row, op_ptr, rna_path, UI_ITEM_NONE, name, ICON_NONE);
      }
  }
  if (!nodes::input_has_attribute_toggle(node_tree, socket_index)) {
    uiItemL(row, "", ICON_BLANK1);
  }
}

static void run_node_group_ui(bContext *C, wmOperator *op)
{
  uiLayout *layout = op->layout;
  uiLayoutSetPropSep(layout, true);
  uiLayoutSetPropDecorate(layout, false);
  Main *bmain = CTX_data_main(C);
  PointerRNA bmain_ptr = RNA_main_pointer_create(bmain);

  const bNodeTree *node_tree = get_node_group(*C, *op->ptr, nullptr);
  if (!node_tree) {
    return;
  }
  const nodes::PropertiesVectorSet properties_set = nodes::build_properties_vector_set(
      op->properties);

  node_tree->ensure_interface_cache();

  Array<bool> input_usages(node_tree->interface_inputs().size());
  nodes::socket_usage_inference::infer_group_interface_inputs_usage(
      *node_tree, properties_set, input_usages);

  int input_index = 0;
  for (const bNodeTreeInterfaceSocket *io_socket : node_tree->interface_inputs()) {
    draw_property_for_socket(*node_tree,
                             layout,
                             properties_set,
                             &bmain_ptr,
                             op->ptr,
                             *io_socket,
                             input_index,
                             input_usages[input_index]);
    ++input_index;
  }
}

static bool run_node_ui_poll(wmOperatorType * /*ot*/, PointerRNA *ptr)
{
  bool result = false;
  RNA_STRUCT_BEGIN (ptr, prop) {
    int flag = RNA_property_flag(prop);
    if ((flag & PROP_HIDDEN) == 0) {
      result = true;
      break;
    }
  }
  RNA_STRUCT_END;
  return result;
}

static std::string run_node_group_get_name(wmOperatorType * /*ot*/, PointerRNA *ptr)
{
  int len;
  char *local_name = RNA_string_get_alloc(ptr, "name", nullptr, 0, &len);
  BLI_SCOPED_DEFER([&]() { MEM_SAFE_FREE(local_name); })
  if (len > 0) {
    return std::string(local_name, len);
  }
  char *library_asset_identifier = RNA_string_get_alloc(
      ptr, "relative_asset_identifier", nullptr, 0, &len);
  BLI_SCOPED_DEFER([&]() { MEM_SAFE_FREE(library_asset_identifier); })
  StringRef ref(library_asset_identifier, len);
  return ref.drop_prefix(ref.find_last_of(SEP_STR) + 1);
}

static bool run_node_group_depends_on_cursor(bContext &C, wmOperatorType & /*ot*/, PointerRNA *ptr)
{
  if (!ptr) {
    return false;
  }
  Main &bmain = *CTX_data_main(&C);
  if (bNodeTree *group = reinterpret_cast<bNodeTree *>(
          WM_operator_properties_id_lookup_from_name_or_session_uid(&bmain, ptr, ID_NT)))
  {
    return group->geometry_node_asset_traits &&
           (group->geometry_node_asset_traits->flag & GEO_NODE_ASSET_WAIT_FOR_CURSOR) != 0;
  }

  const asset_system::AssetRepresentation *asset =
      asset::operator_asset_reference_props_get_asset_from_all_library(C, *ptr, nullptr);
  if (!asset) {
    return false;
  }
  const IDProperty *traits_flag = BKE_asset_metadata_idprop_find(
      &asset->get_metadata(), "geometry_node_asset_traits_flag");
  if (traits_flag == nullptr || !(IDP_Int(traits_flag) & GEO_NODE_ASSET_WAIT_FOR_CURSOR)) {
    return false;
  }
  return true;
}

void GEOMETRY_OT_execute_node_group(wmOperatorType *ot)
{
  PropertyRNA *prop;
  ot->name = "Run Node Group";
  ot->idname = __func__;
  ot->description = "Execute a node group on geometry";

  /* A proper poll is not possible, since it doesn't have access to the operator's properties. */
  ot->invoke = run_node_group_invoke;
  ot->exec = run_node_group_exec;
  ot->get_description = run_node_group_get_description;
  ot->ui = run_node_group_ui;
  ot->ui_poll = run_node_ui_poll;
  ot->get_name = run_node_group_get_name;
  ot->depends_on_cursor = run_node_group_depends_on_cursor;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  asset::operator_asset_reference_props_register(*ot->srna);
  WM_operator_properties_id_lookup(ot, true);

  /* See comment for #store_input_node_values_rna_props. */
  prop = RNA_def_int_array(ot->srna,
                           "mouse_position",
                           2,
                           nullptr,
                           INT_MIN,
                           INT_MAX,
                           "Mouse Position",
                           "Mouse coordinates in region space",
                           INT_MIN,
                           INT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_int_array(
      ot->srna, "region_size", 2, nullptr, 0, INT_MAX, "Region Size", "", 0, INT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_float_array(ot->srna,
                             "cursor_position",
                             3,
                             nullptr,
                             FLT_MIN,
                             FLT_MAX,
                             "3D Cursor Position",
                             "",
                             FLT_MIN,
                             FLT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_float_array(ot->srna,
                             "cursor_rotation",
                             4,
                             nullptr,
                             FLT_MIN,
                             FLT_MAX,
                             "3D Cursor Rotation",
                             "",
                             FLT_MIN,
                             FLT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_float_array(ot->srna,
                             "viewport_projection_matrix",
                             16,
                             nullptr,
                             FLT_MIN,
                             FLT_MAX,
                             "Viewport Projection Transform",
                             "",
                             FLT_MIN,
                             FLT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_float_array(ot->srna,
                             "viewport_view_matrix",
                             16,
                             nullptr,
                             FLT_MIN,
                             FLT_MAX,
                             "Viewport View Transform",
                             "",
                             FLT_MIN,
                             FLT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_boolean(
      ot->srna, "viewport_is_perspective", false, "Viewport Is Perspective", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Menu
 * \{ */

static bool asset_menu_poll(const bContext *C, MenuType * /*mt*/)
{
  return CTX_wm_view3d(C);
}

static GeometryNodeAssetTraitFlag asset_flag_for_context(const ObjectType type,
                                                         const eObjectMode mode)
{
  switch (type) {
    case OB_MESH: {
      switch (mode) {
        case OB_MODE_OBJECT:
          return (GEO_NODE_ASSET_TOOL | GEO_NODE_ASSET_OBJECT | GEO_NODE_ASSET_MESH);
        case OB_MODE_EDIT:
          return (GEO_NODE_ASSET_TOOL | GEO_NODE_ASSET_EDIT | GEO_NODE_ASSET_MESH);
        case OB_MODE_SCULPT:
          return (GEO_NODE_ASSET_TOOL | GEO_NODE_ASSET_SCULPT | GEO_NODE_ASSET_MESH);
        default:
          break;
      }
      break;
    }
    case OB_CURVES: {
      switch (mode) {
        case OB_MODE_OBJECT:
          return (GEO_NODE_ASSET_TOOL | GEO_NODE_ASSET_OBJECT | GEO_NODE_ASSET_CURVE);
        case OB_MODE_EDIT:
          return (GEO_NODE_ASSET_TOOL | GEO_NODE_ASSET_EDIT | GEO_NODE_ASSET_CURVE);
        case OB_MODE_SCULPT_CURVES:
          return (GEO_NODE_ASSET_TOOL | GEO_NODE_ASSET_SCULPT | GEO_NODE_ASSET_CURVE);
        default:
          break;
      }
      break;
    }
    case OB_POINTCLOUD: {
      switch (mode) {
        case OB_MODE_OBJECT:
          return (GEO_NODE_ASSET_TOOL | GEO_NODE_ASSET_OBJECT | GEO_NODE_ASSET_POINTCLOUD);
        case OB_MODE_EDIT:
          return (GEO_NODE_ASSET_TOOL | GEO_NODE_ASSET_EDIT | GEO_NODE_ASSET_POINTCLOUD);
        default:
          break;
      }
      break;
    }
    default:
      break;
  }
  BLI_assert_unreachable();
  return GeometryNodeAssetTraitFlag(0);
}

static GeometryNodeAssetTraitFlag asset_flag_for_context(const Object &active_object)
{
  return asset_flag_for_context(ObjectType(active_object.type), eObjectMode(active_object.mode));
}

static asset::AssetItemTree *get_static_item_tree(const ObjectType type, const eObjectMode mode)
{
  switch (type) {
    case OB_MESH: {
      switch (mode) {
        case OB_MODE_OBJECT: {
          static asset::AssetItemTree tree;
          return &tree;
        }
        case OB_MODE_EDIT: {
          static asset::AssetItemTree tree;
          return &tree;
        }
        case OB_MODE_SCULPT: {
          static asset::AssetItemTree tree;
          return &tree;
        }
        default:
          return nullptr;
      }
    }
    case OB_CURVES: {
      switch (mode) {
        case OB_MODE_OBJECT: {
          static asset::AssetItemTree tree;
          return &tree;
        }
        case OB_MODE_EDIT: {
          static asset::AssetItemTree tree;
          return &tree;
        }
        case OB_MODE_SCULPT_CURVES: {
          static asset::AssetItemTree tree;
          return &tree;
        }
        default:
          return nullptr;
      }
    }
    case OB_POINTCLOUD: {
      switch (mode) {
        case OB_MODE_OBJECT: {
          static asset::AssetItemTree tree;
          return &tree;
        }
        case OB_MODE_EDIT: {
          static asset::AssetItemTree tree;
          return &tree;
        }
        default:
          return nullptr;
      }
    }
    default:
      return nullptr;
  }
}

static asset::AssetItemTree *get_static_item_tree(const Object &active_object)
{
  return get_static_item_tree(ObjectType(active_object.type), eObjectMode(active_object.mode));
}

void clear_operator_asset_trees()
{
  for (const ObjectType type : {OB_MESH, OB_CURVES, OB_POINTCLOUD}) {
    for (const eObjectMode mode :
         {OB_MODE_OBJECT, OB_MODE_EDIT, OB_MODE_SCULPT, OB_MODE_SCULPT_CURVES})
    {
      if (asset::AssetItemTree *tree = get_static_item_tree(type, mode)) {
        tree->dirty = true;
      }
    }
  }
}

static asset::AssetItemTree build_catalog_tree(const bContext &C, const Object &active_object)
{
  asset::AssetFilterSettings type_filter{};
  type_filter.id_types = FILTER_ID_NT;
  const GeometryNodeAssetTraitFlag flag = asset_flag_for_context(active_object);
  auto meta_data_filter = [&](const AssetMetaData &meta_data) {
    const IDProperty *tree_type = BKE_asset_metadata_idprop_find(&meta_data, "type");
    if (tree_type == nullptr || IDP_Int(tree_type) != NTREE_GEOMETRY) {
      return false;
    }
    const IDProperty *traits_flag = BKE_asset_metadata_idprop_find(
        &meta_data, "geometry_node_asset_traits_flag");
    if (traits_flag == nullptr || (IDP_Int(traits_flag) & flag) != flag) {
      return false;
    }
    return true;
  };
  const AssetLibraryReference library = asset_system::all_library_reference();
  asset_system::all_library_reload_catalogs_if_dirty();
  return asset::build_filtered_all_catalog_tree(library, C, type_filter, meta_data_filter);
}

/**
 * Avoid adding a separate root catalog when the assets have already been added to one of the
 * builtin menus. The need to define the builtin menu labels here is non-ideal. We don't have
 * any UI introspection that can do this though.
 */
static Set<std::string> get_builtin_menus(const ObjectType object_type, const eObjectMode mode)
{
  Set<std::string> menus;
  switch (object_type) {
    case OB_CURVES:
      menus.add_new("View");
      menus.add_new("Select");
      menus.add_new("Curves");
      break;
    case OB_POINTCLOUD:
      menus.add_new("View");
      menus.add_new("Select");
      menus.add_new("Point Cloud");
      break;
    case OB_MESH:
      switch (mode) {
        case OB_MODE_OBJECT:
          menus.add_new("View");
          menus.add_new("Select");
          menus.add_new("Add");
          menus.add_new("Object");
          menus.add_new("Object/Apply");
          menus.add_new("Object/Convert");
          menus.add_new("Object/Quick Effects");
          break;
        case OB_MODE_EDIT:
          menus.add_new("View");
          menus.add_new("Select");
          menus.add_new("Add");
          menus.add_new("Mesh");
          menus.add_new("Mesh/Extrude");
          menus.add_new("Mesh/Clean Up");
          menus.add_new("Mesh/Delete");
          menus.add_new("Mesh/Merge");
          menus.add_new("Mesh/Normals");
          menus.add_new("Mesh/Shading");
          menus.add_new("Mesh/Split");
          menus.add_new("Mesh/Weights");
          menus.add_new("Vertex");
          menus.add_new("Edge");
          menus.add_new("Face");
          menus.add_new("Face/Face Data");
          menus.add_new("UV");
          menus.add_new("UV/Unwrap");
          break;
        case OB_MODE_SCULPT:
          menus.add_new("View");
          menus.add_new("Sculpt");
          menus.add_new("Mask");
          menus.add_new("Face Sets");
          break;
        case OB_MODE_VERTEX_PAINT:
          menus.add_new("View");
          menus.add_new("Paint");
          break;
        case OB_MODE_WEIGHT_PAINT:
          menus.add_new("View");
          menus.add_new("Weights");
          break;
        default:
          break;
      }
    default:
      break;
  }
  return menus;
}

static void catalog_assets_draw(const bContext *C, Menu *menu)
{
  const Object *active_object = CTX_data_active_object(C);
  if (!active_object) {
    return;
  }
  asset::AssetItemTree *tree = get_static_item_tree(*active_object);
  if (!tree) {
    return;
  }
  const std::optional<StringRefNull> menu_path = CTX_data_string_get(C, "asset_catalog_path");
  if (!menu_path) {
    return;
  }
  const Span<asset_system::AssetRepresentation *> assets = tree->assets_per_path.lookup(
      menu_path->data());
  const asset_system::AssetCatalogTreeItem *catalog_item = tree->catalogs.find_item(
      menu_path->data());
  BLI_assert(catalog_item != nullptr);

  uiLayout *layout = menu->layout;
  bool add_separator = true;

  wmOperatorType *ot = WM_operatortype_find("GEOMETRY_OT_execute_node_group", true);
  for (const asset_system::AssetRepresentation *asset : assets) {
    if (add_separator) {
      uiItemS(layout);
      add_separator = false;
    }
    PointerRNA props_ptr;
    uiItemFullO_ptr(layout,
                    ot,
                    IFACE_(asset->get_name()),
                    ICON_NONE,
                    nullptr,
                    WM_OP_INVOKE_REGION_WIN,
                    UI_ITEM_NONE,
                    &props_ptr);
    asset::operator_asset_reference_props_set(*asset, props_ptr);
  }

  const Set<std::string> builtin_menus = get_builtin_menus(ObjectType(active_object->type),
                                                           eObjectMode(active_object->mode));

  asset_system::AssetLibrary *all_library = asset::list::library_get_once_available(
      asset_system::all_library_reference());
  if (!all_library) {
    return;
  }

  catalog_item->foreach_child([&](const asset_system::AssetCatalogTreeItem &item) {
    if (builtin_menus.contains_as(item.catalog_path().str())) {
      return;
    }
    if (add_separator) {
      uiItemS(layout);
      add_separator = false;
    }
    asset::draw_menu_for_catalog(item, "GEO_MT_node_operator_catalog_assets", *layout);
  });
}

MenuType node_group_operator_assets_menu()
{
  MenuType type{};
  STRNCPY(type.idname, "GEO_MT_node_operator_catalog_assets");
  type.poll = asset_menu_poll;
  type.draw = catalog_assets_draw;
  type.listener = asset::list::asset_reading_region_listen_fn;
  type.flag = MenuTypeFlag::ContextDependent;
  return type;
}

static bool unassigned_local_poll(const bContext &C)
{
  Main &bmain = *CTX_data_main(&C);
  const Object *active_object = CTX_data_active_object(&C);
  if (!active_object) {
    return false;
  }
  const GeometryNodeAssetTraitFlag flag = asset_flag_for_context(*active_object);
  LISTBASE_FOREACH (const bNodeTree *, group, &bmain.nodetrees) {
    /* Assets are displayed in other menus, and non-local data-blocks aren't added to this menu. */
    if (group->id.library_weak_reference || group->id.asset_data) {
      continue;
    }
    if (!group->geometry_node_asset_traits ||
        (group->geometry_node_asset_traits->flag & flag) != flag)
    {
      continue;
    }
    return true;
  }
  return false;
}

static void catalog_assets_draw_unassigned(const bContext *C, Menu *menu)
{
  const Object *active_object = CTX_data_active_object(C);
  if (!active_object) {
    return;
  }
  asset::AssetItemTree *tree = get_static_item_tree(*active_object);
  if (!tree) {
    return;
  }
  uiLayout *layout = menu->layout;
  wmOperatorType *ot = WM_operatortype_find("GEOMETRY_OT_execute_node_group", true);
  for (const asset_system::AssetRepresentation *asset : tree->unassigned_assets) {
    PointerRNA props_ptr;
    uiItemFullO_ptr(layout,
                    ot,
                    IFACE_(asset->get_name()),
                    ICON_NONE,
                    nullptr,
                    WM_OP_INVOKE_REGION_WIN,
                    UI_ITEM_NONE,
                    &props_ptr);
    asset::operator_asset_reference_props_set(*asset, props_ptr);
  }

  const GeometryNodeAssetTraitFlag flag = asset_flag_for_context(*active_object);

  bool first = true;
  bool add_separator = !tree->unassigned_assets.is_empty();
  Main &bmain = *CTX_data_main(C);
  LISTBASE_FOREACH (const bNodeTree *, group, &bmain.nodetrees) {
    /* Assets are displayed in other menus, and non-local data-blocks aren't added to this menu. */
    if (group->id.library_weak_reference || group->id.asset_data) {
      continue;
    }
    if (!group->geometry_node_asset_traits ||
        (group->geometry_node_asset_traits->flag & flag) != flag)
    {
      continue;
    }

    if (add_separator) {
      uiItemS(layout);
      add_separator = false;
    }
    if (first) {
      uiItemL(layout, IFACE_("Non-Assets"), ICON_NONE);
      first = false;
    }

    PointerRNA props_ptr;
    uiItemFullO_ptr(layout,
                    ot,
                    group->id.name + 2,
                    ICON_NONE,
                    nullptr,
                    WM_OP_INVOKE_REGION_WIN,
                    UI_ITEM_NONE,
                    &props_ptr);
    WM_operator_properties_id_lookup_set_from_id(&props_ptr, &group->id);
    /* Also set the name so it can be used for #run_node_group_get_name. */
    RNA_string_set(&props_ptr, "name", group->id.name + 2);
  }
}

MenuType node_group_operator_assets_menu_unassigned()
{
  MenuType type{};
  STRNCPY(type.label, "Unassigned Node Tools");
  STRNCPY(type.idname, "GEO_MT_node_operator_unassigned");
  type.poll = asset_menu_poll;
  type.draw = catalog_assets_draw_unassigned;
  type.listener = asset::list::asset_reading_region_listen_fn;
  type.flag = MenuTypeFlag::ContextDependent;
  type.description = N_(
      "Tool node group assets not assigned to a catalog.\n"
      "Catalogs can be assigned in the Asset Browser");
  return type;
}

void ui_template_node_operator_asset_menu_items(uiLayout &layout,
                                                const bContext &C,
                                                const StringRef catalog_path)
{
  const Object *active_object = CTX_data_active_object(&C);
  if (!active_object) {
    return;
  }
  asset::AssetItemTree *tree = get_static_item_tree(*active_object);
  if (!tree) {
    return;
  }
  const asset_system::AssetCatalogTreeItem *item = tree->catalogs.find_item(catalog_path);
  if (!item) {
    return;
  }
  asset_system::AssetLibrary *all_library = asset::list::library_get_once_available(
      asset_system::all_library_reference());
  if (!all_library) {
    return;
  }
  uiLayout *col = &layout.column(false);
  uiLayoutSetContextString(col, "asset_catalog_path", item->catalog_path().str());
  uiItemMContents(col, "GEO_MT_node_operator_catalog_assets");
}

void ui_template_node_operator_asset_root_items(uiLayout &layout, const bContext &C)
{
  const Object *active_object = CTX_data_active_object(&C);
  if (!active_object) {
    return;
  }
  asset::AssetItemTree *tree = get_static_item_tree(*active_object);
  if (!tree) {
    return;
  }
  if (tree->dirty) {
    *tree = build_catalog_tree(C, *active_object);
  }

  const Set<std::string> builtin_menus = get_builtin_menus(ObjectType(active_object->type),
                                                           eObjectMode(active_object->mode));

  tree->catalogs.foreach_root_item([&](const asset_system::AssetCatalogTreeItem &item) {
    if (!builtin_menus.contains_as(item.catalog_path().str())) {
      asset::draw_menu_for_catalog(item, "GEO_MT_node_operator_catalog_assets", layout);
    }
  });

  if (!tree->unassigned_assets.is_empty() || unassigned_local_poll(C)) {
    uiItemM(&layout, "GEO_MT_node_operator_unassigned", "", ICON_FILE_HIDDEN);
  }
}

/** \} */

}  // namespace blender::ed::geometry
