/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_glsl_cpp_stubs.hh"

/**
 * Convert from a cube-map vector to an octahedron UV coordinate.
 */
vec2 octahedral_uv_from_direction(vec3 co)
{
  /* Projection onto octahedron. */
  co /= dot(vec3(1.0f), abs(co));

  /* Out-folding of the downward faces. */
  if (co.z < 0.0f) {
    vec2 sign = step(0.0f, co.xy) * 2.0f - 1.0f;
    co.xy = (1.0f - abs(co.yx)) * sign;
  }

  /* Mapping to [0;1]^2 texture space. */
  vec2 uvs = co.xy * (0.5f) + 0.5f;

  return uvs;
}

vec3 octahedral_uv_to_direction(vec2 co)
{
  /* Change range to between [-1..1] */
  co = co * 2.0f - 1.0f;

  vec2 abs_co = abs(co);
  vec3 v = vec3(co, 1.0f - (abs_co.x + abs_co.y));

  if (abs_co.x + abs_co.y > 1.0f) {
    v.xy = (abs(co.yx) - 1.0f) * -sign(co.xy);
  }

  return v;
}

/* Mirror the UV if they are not on the diagonal or unit UV squares.
 * Doesn't extend outside of [-1..2] range. But this is fine since we use it only for borders. */
vec2 octahedral_mirror_repeat_uv(vec2 uv)
{
  vec2 m = abs(uv - 0.5f) + 0.5f;
  vec2 f = floor(m);
  float x = f.x - f.y;
  if (x != 0.0f) {
    uv.xy = 1.0f - uv.xy;
  }
  return fract(uv);
}
