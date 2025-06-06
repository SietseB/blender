/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_euler.hh"

#include "NOD_socket_search_link.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "node_function_util.hh"

namespace blender::nodes::node_fn_input_rotation_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Rotation>("Rotation");
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiLayout *col = &layout->column(true);
  uiItemR(col, ptr, "rotation_euler", UI_ITEM_R_EXPAND, "", ICON_NONE);
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  const bNode &bnode = builder.node();
  const NodeInputRotation &node_storage = *static_cast<const NodeInputRotation *>(bnode.storage);
  const math::EulerXYZ euler_rotation(node_storage.rotation_euler[0],
                                      node_storage.rotation_euler[1],
                                      node_storage.rotation_euler[2]);
  builder.construct_and_set_matching_fn<mf::CustomMF_Constant<math::Quaternion>>(
      math::to_quaternion(euler_rotation));
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeInputRotation *data = MEM_callocN<NodeInputRotation>(__func__);
  node->storage = data;
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  fn_node_type_base(&ntype, "FunctionNodeInputRotation", FN_NODE_INPUT_ROTATION);
  ntype.ui_name = "Rotation";
  ntype.enum_name_legacy = "INPUT_ROTATION";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  blender::bke::node_type_storage(
      ntype, "NodeInputRotation", node_free_standard_storage, node_copy_standard_storage);
  ntype.build_multi_function = node_build_multi_function;
  ntype.draw_buttons = node_layout;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_input_rotation_cc
