/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#include "BLI_hash.h"
#include "BLI_math_color.h"
#include "BLI_math_color.hh"
#include "BLI_math_vector.hh"

#include "BLT_translation.hh"

#include "BLO_read_write.hh"

#include "DNA_defaults.h"
#include "DNA_gpencil_modifier_types.h"
#include "DNA_material_types.h"

#include "BKE_colortools.hh"
#include "BKE_curves.hh"
#include "BKE_geometry_set.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_material.h"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "MOD_grease_pencil_util.hh"
#include "MOD_ui_common.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.h"

namespace blender {

static void init_data(ModifierData *md)
{
  GreasePencilNoiseModifierData *gpmd = reinterpret_cast<GreasePencilNoiseModifierData *>(md);

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(gpmd, modifier));

  MEMCPY_STRUCT_AFTER(gpmd, DNA_struct_default_get(GreasePencilNoiseModifierData), modifier);
  modifier::greasepencil::init_influence_data(&gpmd->influence, true);
}

static void free_data(ModifierData *md)
{
  GreasePencilNoiseModifierData *mmd = reinterpret_cast<GreasePencilNoiseModifierData *>(md);

  modifier::greasepencil::free_influence_data(&mmd->influence);
}

static void copy_data(const ModifierData *md, ModifierData *target, int flag)
{
  const GreasePencilNoiseModifierData *gmd =
      reinterpret_cast<const GreasePencilNoiseModifierData *>(md);
  GreasePencilNoiseModifierData *tgmd = reinterpret_cast<GreasePencilNoiseModifierData *>(target);

  BKE_modifier_copydata_generic(md, target, flag);
  modifier::greasepencil::copy_influence_data(&gmd->influence, &tgmd->influence, flag);
}

static void blend_write(BlendWriter *writer, const ID * /*id_owner*/, const ModifierData *md)
{
  const GreasePencilNoiseModifierData *mmd =
      reinterpret_cast<const GreasePencilNoiseModifierData *>(md);

  BLO_write_struct(writer, GreasePencilNoiseModifierData, mmd);
  modifier::greasepencil::write_influence_data(writer, &mmd->influence);
}

static void blend_read(BlendDataReader *reader, ModifierData *md)
{
  GreasePencilNoiseModifierData *mmd = reinterpret_cast<GreasePencilNoiseModifierData *>(md);
  modifier::greasepencil::read_influence_data(reader, &mmd->influence);
}

static bool depends_on_time(Scene * /*scene*/, ModifierData *md)
{
  GreasePencilNoiseModifierData *mmd = reinterpret_cast<GreasePencilNoiseModifierData *>(md);
  return (mmd->flag & GP_NOISE_USE_RANDOM) != 0;
}

static Array<float> noise_table(const int len, const int offset, const int seed)
{
  Array<float> table(len);
  for (int i = 0; i < len; i++) {
    table[i] = BLI_hash_int_01(BLI_hash_int_2d(seed, i + offset + 1));
  }
  return table;
}

/**
 * Apply noise effect based on stroke direction.
 */
static void deform_drawing(const GreasePencilNoiseModifierData &mmd,
                           Object &ob,
                           const int ctime,
                           const int start_frame_number,
                           bke::greasepencil::Drawing &drawing)
{
  bke::CurvesGeometry &strokes = drawing.strokes_for_write();
  bke::MutableAttributeAccessor attributes = strokes.attributes_for_write();
  if (strokes.points_num() == 0) {
    return;
  }

  IndexMaskMemory memory;
  const IndexMask filtered_strokes = modifier::greasepencil::get_filtered_stroke_mask(
      &ob, strokes, mmd.influence, memory);

  const bool use_curve = (mmd.influence.flag & GREASE_PENCIL_INFLUENCE_USE_CUSTOM_CURVE) != 0;
  const bool is_keyframe = (mmd.noise_mode == GP_NOISE_RANDOM_KEYFRAME);
  const bool use_random = (mmd.flag & GP_NOISE_USE_RANDOM) != 0;
  const bool use_random_smooth = use_random && !is_keyframe &&
                                 ((mmd.flag & GP_NOISE_USE_RANDOM_SMOOTH) != 0);
  const bool use_color = (mmd.flag & GP_NOISE_USE_COLOR) != 0;

  /* Sanitize as it can create out of bound reads. */
  const float noise_scale = math::clamp(mmd.noise_scale, 0.0f, 1.0f);
  const float noise_offset = math::fract(mmd.noise_offset);
  const int floored_noise_offset = int(math::floor(mmd.noise_offset));

  if (filtered_strokes.is_empty()) {
    return;
  }

  int seed = mmd.seed;
  int seed_next;
  float smooth_factor;
  /* Make sure different modifiers get different seeds. */
  seed += BLI_hash_string(ob.id.name + 2);
  seed += BLI_hash_string(mmd.modifier.name);
  if (use_random) {
    if (!is_keyframe) {
      seed += math::floor(ctime / mmd.step);
      seed_next = seed + 1;
      smooth_factor = float(ctime % mmd.step) / mmd.step;
    }
    else {
      /* If change every keyframe, use the last keyframe. */
      seed += start_frame_number;
    }
  }

  const OffsetIndices<int> points_by_curve = strokes.points_by_curve();
  const VArray<float> vgroup_weights = modifier::greasepencil::get_influence_vertex_weights(
      strokes, mmd.influence);

  auto get_weight = [&](const IndexRange points, const int point_i) {
    const float vertex_weight = vgroup_weights[points[point_i]];
    if (!use_curve) {
      return vertex_weight;
    }
    const float value = float(point_i) / float(points.size() - 1);
    return vertex_weight * BKE_curvemapping_evaluateF(mmd.influence.custom_curve, 0, value);
  };

  auto get_noise = [](const Span<float> noise_table, const float value) {
    return math::interpolate(noise_table[int(math::ceil(value))],
                             noise_table[int(math::floor(value))],
                             math::fract(value));
  };

  if (mmd.factor > 0.0f) {
    const Span<float3> curve_plane_normals = drawing.curve_plane_normals();
    const Span<float3> tangents = strokes.evaluated_tangents();
    MutableSpan<float3> positions = strokes.positions_for_write();

    filtered_strokes.foreach_index(GrainSize(512), [&](const int stroke_i) {
      const IndexRange points = points_by_curve[stroke_i];
      const int noise_len = math::ceil(points.size() * noise_scale) + 2;
      const Array<float> table = noise_table(noise_len, floored_noise_offset, seed + 2 + stroke_i);
      Array<float> table_next;
      if (use_random_smooth) {
        table_next = noise_table(noise_len, floored_noise_offset, seed_next + 2 + stroke_i);
      }
      for (const int i : points.index_range()) {
        const int point = points[i];
        float weight = get_weight(points, i);
        /* Vector orthogonal to normal. */
        const float3 bi_normal = math::normalize(
            math::cross(tangents[point], curve_plane_normals[stroke_i]));
        const float noise = get_noise(table, i * noise_scale + noise_offset);
        float3 pos_next = positions[point];
        positions[point] += bi_normal * (noise * 2.0f - 1.0f) * weight * mmd.factor * 0.1f;
        if (use_random_smooth) {
          const float noise = get_noise(table_next, i * noise_scale + noise_offset);
          pos_next += bi_normal * (noise * 2.0f - 1.0f) * weight * mmd.factor * 0.1f;
          positions[point] = math::interpolate(positions[point], pos_next, smooth_factor);
        }
      }
    });
    drawing.tag_positions_changed();
  }

  if (mmd.factor_thickness > 0.0f) {
    MutableSpan<float> radii = drawing.radii_for_write();

    filtered_strokes.foreach_index(GrainSize(512), [&](const int stroke_i) {
      const IndexRange points = points_by_curve[stroke_i];
      const int noise_len = math::ceil(points.size() * noise_scale) + 2;
      const Array<float> table = noise_table(noise_len, floored_noise_offset, seed + stroke_i);
      Array<float> table_next;
      if (use_random_smooth) {
        table_next = noise_table(noise_len, floored_noise_offset, seed_next + stroke_i);
      }
      for (const int i : points.index_range()) {
        const int point = points[i];
        const float weight = get_weight(points, i);
        const float noise = get_noise(table, i * noise_scale + noise_offset);
        float radius_next = radii[point];
        radii[point] *= math::max(1.0f + (noise * 2.0f - 1.0f) * weight * mmd.factor_thickness,
                                  0.0f);
        if (use_random_smooth) {
          const float noise = get_noise(table_next, i * noise_scale + noise_offset);
          radius_next *= math::max(1.0f + (noise * 2.0f - 1.0f) * weight * mmd.factor_thickness,
                                   0.0f);
          radii[point] = math::interpolate(radii[point], radius_next, smooth_factor);
        }
      }
    });
  }

  if (mmd.factor_strength > 0.0f) {
    MutableSpan<float> opacities = drawing.opacities_for_write();

    filtered_strokes.foreach_index(GrainSize(512), [&](const int stroke_i) {
      const IndexRange points = points_by_curve[stroke_i];
      const int noise_len = math::ceil(points.size() * noise_scale) + 2;
      const Array<float> table = noise_table(noise_len, floored_noise_offset, seed + 3 + stroke_i);
      Array<float> table_next;
      if (use_random_smooth) {
        table_next = noise_table(noise_len, floored_noise_offset, seed_next + 3 + stroke_i);
      }
      for (const int i : points.index_range()) {
        const int point = points[i];
        const float weight = get_weight(points, i);
        const float noise = get_noise(table, i * noise_scale + noise_offset);
        float opacity_next = opacities[point];
        opacities[point] *= math::max(1.0f - noise * weight * mmd.factor_strength, 0.0f);
        if (use_random_smooth) {
          const float noise = get_noise(table_next, i * noise_scale + noise_offset);
          opacity_next *= math::max(1.0f - noise * weight * mmd.factor_strength, 0.0f);
          opacities[point] = math::interpolate(opacities[point], opacity_next, smooth_factor);
        }
      }
    });
  }

  if (mmd.factor_uvs > 0.0f) {
    bke::SpanAttributeWriter<float> rotations = attributes.lookup_or_add_for_write_span<float>(
        "rotation", bke::AttrDomain::Point);

    filtered_strokes.foreach_index(GrainSize(512), [&](const int stroke_i) {
      const IndexRange points = points_by_curve[stroke_i];
      const int noise_len = math::ceil(points.size() * noise_scale) + 2;
      const Array<float> table = noise_table(noise_len, floored_noise_offset, seed + 4 + stroke_i);
      Array<float> table_next;
      if (use_random_smooth) {
        table_next = noise_table(noise_len, floored_noise_offset, seed_next + 4 + stroke_i);
      }
      for (const int i : points.index_range()) {
        const int point = points[i];
        const float weight = get_weight(points, i);
        const float noise = get_noise(table, i * noise_scale + noise_offset);
        const float delta_rot = (noise * 2.0f - 1.0f) * weight * mmd.factor_uvs * M_PI_2;
        float rotation_next = rotations.span[point];
        rotations.span[point] = math::clamp(
            rotations.span[point] + delta_rot, float(-M_PI_2), float(M_PI_2));
        if (use_random_smooth) {
          const float noise = get_noise(table_next, i * noise_scale + noise_offset);
          const float delta_rot = (noise * 2.0f - 1.0f) * weight * mmd.factor_uvs * M_PI_2;
          rotation_next = math::clamp(rotation_next + delta_rot, float(-M_PI_2), float(M_PI_2));
          rotations.span[point] = math::interpolate(
              rotations.span[point], rotation_next, smooth_factor);
        }
      }
    });
    rotations.finish();
  }

  if (use_color) {
    const VArray<int> stroke_materials = *attributes.lookup_or_default<int>(
        "material_index", bke::AttrDomain::Curve, 0);
    MutableSpan<ColorGeometry4f> fill_colors = drawing.fill_colors_for_write();
    MutableSpan<ColorGeometry4f> vertex_colors = drawing.vertex_colors_for_write();

    filtered_strokes.foreach_index(GrainSize(512), [&](const int stroke_i) {
      const IndexRange points = points_by_curve[stroke_i];
      const int noise_len = math::ceil(points.size() * noise_scale) + 2;
      const Array<float> table_h = noise_table(
          noise_len, floored_noise_offset, seed + 5 + stroke_i);
      const Array<float> table_s = noise_table(
          noise_len, floored_noise_offset, seed + 6 + stroke_i);
      const Array<float> table_v = noise_table(
          noise_len, floored_noise_offset, seed + 7 + stroke_i);
      Array<float> table_next_h;
      Array<float> table_next_s;
      Array<float> table_next_v;
      if (use_random_smooth) {
        table_next_h = noise_table(noise_len, floored_noise_offset, seed_next + 5 + stroke_i);
        table_next_s = noise_table(noise_len, floored_noise_offset, seed_next + 6 + stroke_i);
        table_next_v = noise_table(noise_len, floored_noise_offset, seed_next + 7 + stroke_i);
      }

      const Material *ma = BKE_object_material_get(&ob, stroke_materials[stroke_i] + 1);
      const MaterialGPencilStyle *gp_style = ma->gp_style;

      /* Fill color. */
      if (mmd.modify_color != MOD_GREASE_PENCIL_COLOR_STROKE) {
        /* If not using fill color, use the material color. */
        if (gp_style && fill_colors[stroke_i].a == 0.0f && gp_style->fill_rgba[3] > 0.0f) {
          fill_colors[stroke_i] = gp_style->fill_rgba;
          fill_colors[stroke_i].a = 1.0f;
        }

        float3 noise, hsv, hsv_next;
        rgb_to_hsv_v(fill_colors[stroke_i], hsv);
        hsv_next = hsv;
        noise[0] = get_noise(table_h, noise_offset);
        hsv[0] = math::fract(hsv[0] + (mmd.hsv[0] - 1.0f) * 0.5f * noise[0]);
        noise[1] = -1.0f + 2.0f * get_noise(table_s, noise_offset);
        hsv[1] = math::clamp(hsv[1] * (1.0f + (mmd.hsv[1] - 1.0f) * noise[1]), 0.0f, 1.0f);
        noise[2] = -1.0f + 2.0f * get_noise(table_v, noise_offset);
        hsv[2] = math::clamp(hsv[2] * (1.0f + (mmd.hsv[2] - 1.0f) * noise[2]), 0.0f, 1.0f);
        hsv_to_rgb_v(hsv, fill_colors[stroke_i]);

        if (use_random_smooth) {
          float noise_next;
          noise_next = get_noise(table_next_h, noise_offset);
          hsv_next[0] = math::fract(hsv_next[0] + (mmd.hsv[0] - 1.0f) * 0.5f * noise_next);
          noise_next = -1.0f + 2.0f * get_noise(table_next_s, noise_offset);
          hsv_next[1] = math::clamp(
              hsv_next[1] * (1.0f + (mmd.hsv[1] - 1.0f) * noise_next), 0.0f, 1.0f);
          noise_next = -1.0f + 2.0f * get_noise(table_next_v, noise_offset);
          hsv_next[2] = math::clamp(
              hsv_next[2] * (1.0f + (mmd.hsv[2] - 1.0f) * noise_next), 0.0f, 1.0f);
          hsv_next = math::interpolate(hsv, hsv_next, smooth_factor);
          hsv_to_rgb_v(hsv_next, fill_colors[stroke_i]);
        }
      }

      /* Stroke color. */
      if (mmd.modify_color != MOD_GREASE_PENCIL_COLOR_FILL) {
        for (const int i : points.index_range()) {
          const int point = points[i];
          const float weight = get_weight(points, i);

          /* If not using vertex color, use the material color. */
          if (gp_style && vertex_colors[point].a == 0.0f && gp_style->stroke_rgba[3] > 0.0f) {
            vertex_colors[point] = gp_style->stroke_rgba;
            vertex_colors[point].a = 1.0f;
          }

          float3 noise, hsv, hsv_next;
          rgb_to_hsv_v(vertex_colors[point], hsv);
          hsv_next = hsv;
          noise[0] = get_noise(table_h, i * noise_scale + noise_offset);
          hsv[0] = math::fract(hsv[0] + (mmd.hsv[0] - 1.0f) * 0.5f * noise[0] * weight);
          noise[1] = -1.0f + 2.0f * get_noise(table_s, i * noise_scale + noise_offset);
          hsv[1] = math::clamp(
              hsv[1] * (1.0f + (mmd.hsv[1] - 1.0f) * noise[1] * weight), 0.0f, 1.0f);
          noise[2] = -1.0f + 2.0f * get_noise(table_v, i * noise_scale + noise_offset);
          hsv[2] = math::clamp(
              hsv[2] * (1.0f + (mmd.hsv[2] - 1.0f) * noise[2] * weight), 0.0f, 1.0f);
          hsv_to_rgb_v(hsv, vertex_colors[point]);

          if (use_random_smooth) {
            float noise_next;
            noise_next = get_noise(table_next_h, i * noise_scale + noise_offset);
            hsv_next[0] = math::fract(hsv_next[0] +
                                      (mmd.hsv[0] - 1.0f) * 0.5f * noise_next * weight);
            noise_next = -1.0f + 2.0f * get_noise(table_next_s, i * noise_scale + noise_offset);
            hsv_next[1] = math::clamp(
                hsv_next[1] * (1.0f + (mmd.hsv[1] - 1.0f) * noise_next * weight), 0.0f, 1.0f);
            noise_next = -1.0f + 2.0f * get_noise(table_next_v, i * noise_scale + noise_offset);
            hsv_next[2] = math::clamp(
                hsv_next[2] * (1.0f + (mmd.hsv[2] - 1.0f) * noise_next * weight), 0.0f, 1.0f);
            hsv_next = math::interpolate(hsv, hsv_next, smooth_factor);
            hsv_to_rgb_v(hsv_next, vertex_colors[point]);
          }
        }
      }
    });
  }
}

static void modify_geometry_set(ModifierData *md,
                                const ModifierEvalContext *ctx,
                                bke::GeometrySet *geometry_set)
{
  const auto *mmd = reinterpret_cast<GreasePencilNoiseModifierData *>(md);

  if (!geometry_set->has_grease_pencil()) {
    return;
  }

  if (!mmd->factor && !mmd->factor_strength && !mmd->factor_thickness && !mmd->factor_uvs &&
      (mmd->flag & GP_NOISE_USE_COLOR) == 0)
  {
    return;
  }

  GreasePencil &grease_pencil = *geometry_set->get_grease_pencil_for_write();
  const int current_frame = grease_pencil.runtime->eval_frame;

  IndexMaskMemory mask_memory;
  const IndexMask layer_mask = modifier::greasepencil::get_filtered_layer_mask(
      grease_pencil, mmd->influence, mask_memory);
  const Vector<modifier::greasepencil::FrameDrawingInfo> drawing_infos =
      modifier::greasepencil::get_drawing_infos_by_frame(grease_pencil, layer_mask, current_frame);

  threading::parallel_for_each(
      drawing_infos, [&](const modifier::greasepencil::FrameDrawingInfo &info) {
        deform_drawing(*mmd, *ctx->object, current_frame, info.start_frame_number, *info.drawing);
      });
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  GreasePencilNoiseModifierData *mmd = reinterpret_cast<GreasePencilNoiseModifierData *>(md);

  modifier::greasepencil::foreach_influence_ID_link(&mmd->influence, ob, walk, user_data);
}

static void panel_draw(const bContext *C, Panel *panel)
{
  uiLayout *col;
  uiLayout *layout = panel->layout;

  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  uiLayoutSetPropSep(layout, true);

  col = uiLayoutColumn(layout, false);
  uiItemR(col, ptr, "factor", UI_ITEM_NONE, IFACE_("Position"), ICON_NONE);
  uiItemR(col, ptr, "factor_strength", UI_ITEM_NONE, IFACE_("Strength"), ICON_NONE);
  uiItemR(col, ptr, "factor_thickness", UI_ITEM_NONE, IFACE_("Thickness"), ICON_NONE);
  uiItemR(col, ptr, "factor_uvs", UI_ITEM_NONE, IFACE_("UV"), ICON_NONE);
  uiItemR(col, ptr, "noise_scale", UI_ITEM_NONE, nullptr, ICON_NONE);
  uiItemR(col, ptr, "noise_offset", UI_ITEM_NONE, nullptr, ICON_NONE);
  uiItemR(col, ptr, "seed", UI_ITEM_NONE, nullptr, ICON_NONE);

  if (uiLayout *color_layout = uiLayoutPanelProp(C, layout, ptr, "open_color_panel", "Color")) {
    uiItemR(color_layout, ptr, "use_color", UI_ITEM_NONE, IFACE_("Color"), ICON_NONE);

    uiLayout *color_col = uiLayoutColumn(color_layout, false);
    uiLayoutSetPropSep(color_col, true);
    uiLayoutSetActive(color_col, RNA_boolean_get(ptr, "use_color"));

    uiItemR(color_col, ptr, "modify_color", UI_ITEM_NONE, nullptr, ICON_NONE);
    uiItemR(color_col, ptr, "hue", UI_ITEM_R_SLIDER, nullptr, ICON_NONE);
    uiItemR(color_col, ptr, "saturation", UI_ITEM_R_SLIDER, nullptr, ICON_NONE);
    uiItemR(color_col, ptr, "value", UI_ITEM_R_SLIDER, nullptr, ICON_NONE);
  }

  if (uiLayout *random_layout = uiLayoutPanelProp(C, layout, ptr, "open_random_panel", "Random")) {
    uiItemR(random_layout, ptr, "use_random", UI_ITEM_NONE, IFACE_("Randomize"), ICON_NONE);

    uiLayout *random_col = uiLayoutColumn(random_layout, false);

    uiLayoutSetPropSep(random_col, true);
    uiLayoutSetActive(random_col, RNA_boolean_get(ptr, "use_random"));

    uiItemR(random_col, ptr, "random_mode", UI_ITEM_NONE, nullptr, ICON_NONE);
    const int mode = RNA_enum_get(ptr, "random_mode");
    if (mode != GP_NOISE_RANDOM_KEYFRAME) {
      uiItemR(random_col, ptr, "step", UI_ITEM_NONE, nullptr, ICON_NONE);
      uiItemR(layout, ptr, "use_random_smooth", UI_ITEM_NONE, NULL, ICON_NONE);
    }
  }

  if (uiLayout *influence_panel = uiLayoutPanelProp(
          C, layout, ptr, "open_influence_panel", "Influence"))
  {
    modifier::greasepencil::draw_layer_filter_settings(C, influence_panel, ptr);
    modifier::greasepencil::draw_material_filter_settings(C, influence_panel, ptr);
    modifier::greasepencil::draw_vertex_group_settings(C, influence_panel, ptr);
    modifier::greasepencil::draw_custom_curve_settings(C, influence_panel, ptr);
  }

  modifier_panel_end(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_GreasePencilNoise, panel_draw);
}

}  // namespace blender

ModifierTypeInfo modifierType_GreasePencilNoise = {
    /*idname*/ "GreasePencilNoiseModifier",
    /*name*/ N_("Noise"),
    /*struct_name*/ "GreasePencilNoiseModifierData",
    /*struct_size*/ sizeof(GreasePencilNoiseModifierData),
    /*srna*/ &RNA_GreasePencilNoiseModifier,
    /*type*/ ModifierTypeType::OnlyDeform,
    /*flags*/
    (eModifierTypeFlag_AcceptsGreasePencil | eModifierTypeFlag_SupportsEditmode |
     eModifierTypeFlag_EnableInEditmode),
    /*icon*/ ICON_GREASEPENCIL,

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
    /*update_depsgraph*/ nullptr,
    /*depends_on_time*/ blender::depends_on_time,
    /*depends_on_normals*/ nullptr,
    /*foreach_ID_link*/ blender::foreach_ID_link,
    /*foreach_tex_link*/ nullptr,
    /*free_runtime_data*/ nullptr,
    /*panel_register*/ blender::panel_register,
    /*blend_write*/ blender::blend_write,
    /*blend_read*/ blender::blend_read,
};
