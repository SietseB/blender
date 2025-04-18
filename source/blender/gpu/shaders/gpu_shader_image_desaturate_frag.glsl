/* SPDX-FileCopyrightText: 2018-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/gpu_shader_2D_image_desaturate_color_info.hh"

FRAGMENT_SHADER_CREATE_INFO(gpu_shader_2D_image_desaturate_color)

void main()
{
  vec4 tex = texture(image, texCoord_interp);
  tex.rgb = ((0.3333333f * factor) * vec3(tex.r + tex.g + tex.b)) + (tex.rgb * (1.0f - factor));
  fragColor = tex * color;
}
