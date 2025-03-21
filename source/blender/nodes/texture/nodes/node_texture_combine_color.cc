/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup texnodes
 */

#include "BLI_math_color.h"
#include "node_texture_util.hh"
#include "node_util.hh"

static blender::bke::bNodeSocketTemplate inputs[] = {
    {SOCK_FLOAT, N_("Red"), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, PROP_FACTOR},
    {SOCK_FLOAT, N_("Green"), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, PROP_FACTOR},
    {SOCK_FLOAT, N_("Blue"), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, PROP_FACTOR},
    {SOCK_FLOAT, N_("Alpha"), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, PROP_FACTOR},
    {-1, ""},
};
static blender::bke::bNodeSocketTemplate outputs[] = {
    {SOCK_RGBA, N_("Color")},
    {-1, ""},
};

static void colorfn(float *out, TexParams *p, bNode *node, bNodeStack **in, short thread)
{
  int i;
  for (i = 0; i < 4; i++) {
    out[i] = tex_input_value(in[i], p, thread);
  }
  /* Apply color space if required. */
  switch (node->custom1) {
    case NODE_COMBSEP_COLOR_RGB: {
      /* Pass */
      break;
    }
    case NODE_COMBSEP_COLOR_HSV: {
      hsv_to_rgb_v(out, out);
      break;
    }
    case NODE_COMBSEP_COLOR_HSL: {
      hsl_to_rgb_v(out, out);
      break;
    }
    default: {
      BLI_assert_unreachable();
      break;
    }
  }
}

static void update(bNodeTree * /*ntree*/, bNode *node)
{
  node_combsep_color_label(&node->inputs, (NodeCombSepColorMode)node->custom1);
}

static void exec(void *data,
                 int /*thread*/,
                 bNode *node,
                 bNodeExecData *execdata,
                 bNodeStack **in,
                 bNodeStack **out)
{
  tex_output(node, execdata, in, out[0], &colorfn, static_cast<TexCallData *>(data));
}

void register_node_type_tex_combine_color()
{
  static blender::bke::bNodeType ntype;

  tex_node_type_base(&ntype, "TextureNodeCombineColor", TEX_NODE_COMBINE_COLOR);
  ntype.ui_name = "Combine Color";
  ntype.enum_name_legacy = "COMBINE_COLOR";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  blender::bke::node_type_socket_templates(&ntype, inputs, outputs);
  ntype.exec_fn = exec;
  ntype.updatefunc = update;

  blender::bke::node_register_type(ntype);
}
