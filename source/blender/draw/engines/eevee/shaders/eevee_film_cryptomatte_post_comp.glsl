/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_film_info.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_film_cryptomatte_post)

#include "gpu_shader_math_base_lib.glsl"

#define CRYPTOMATTE_LEVELS_MAX 16

void cryptomatte_load_samples(ivec2 texel, int layer, out vec2 samples[CRYPTOMATTE_LEVELS_MAX])
{
  int pass_len = divide_ceil(cryptomatte_samples_per_layer, 2);
  int layer_id = layer * pass_len;

  /* Read all samples from the cryptomatte layer. */
  for (int p = 0; p < pass_len; p++) {
    vec4 pass_sample = imageLoadFast(cryptomatte_img, ivec3(texel, p + layer_id));
    samples[p * 2] = pass_sample.xy;
    samples[p * 2 + 1] = pass_sample.zw;
  }
  for (int i = pass_len * 2; i < CRYPTOMATTE_LEVELS_MAX; i++) {
    samples[i] = vec2(0.0f);
  }
}

void cryptomatte_sort_samples(inout vec2 samples[CRYPTOMATTE_LEVELS_MAX])
{
  /* Sort samples. Lame implementation, can be replaced with a more efficient algorithm. */
  for (int i = 0; i < cryptomatte_samples_per_layer - 1 && samples[i].y != 0.0f; i++) {
    int highest_index = i;
    float highest_weight = samples[i].y;
    for (int j = i + 1; j < cryptomatte_samples_per_layer && samples[j].y != 0.0f; j++) {
      if (samples[j].y > highest_weight) {
        highest_index = j;
        highest_weight = samples[j].y;
      }
    };

    if (highest_index != i) {
      vec2 tmp = samples[i];
      samples[i] = samples[highest_index];
      samples[highest_index] = tmp;
    }
  }
}

void cryptomatte_store_samples(ivec2 texel, int layer, vec2 samples[CRYPTOMATTE_LEVELS_MAX])
{
  int pass_len = divide_ceil(cryptomatte_samples_per_layer, 2);
  int layer_id = layer * pass_len;

  /* Store samples back to the cryptomatte layer. */
  for (int p = 0; p < pass_len; p++) {
    vec4 pass_sample;
    pass_sample.xy = samples[p * 2];
    pass_sample.zw = samples[p * 2 + 1];
    imageStoreFast(cryptomatte_img, ivec3(texel, p + layer_id), pass_sample);
  }
  /* Ensure stores are visible to later reads. */
  imageFence(cryptomatte_img);
}

void main()
{
  ivec2 texel = ivec2(gl_GlobalInvocationID.xy);

  if (any(greaterThanEqual(texel, uniform_buf.film.extent))) {
    return;
  }

  for (int layer = 0; layer < cryptomatte_layer_len; layer++) {
    vec2 samples[CRYPTOMATTE_LEVELS_MAX];
    cryptomatte_load_samples(texel, layer, samples);
    cryptomatte_sort_samples(samples);
    cryptomatte_store_samples(texel, layer, samples);
  }
}
