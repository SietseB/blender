/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 */

#include <cstdlib>

#include "BLI_utildefines.h"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "rna_internal.hh"

#include "WM_types.hh"

#ifdef WITH_ONDINE
#  include "ondine_ops.hh"
#endif

#ifdef RNA_RUNTIME

#  include "DNA_brush_types.h"

#  include "BKE_paint.hh"
#  include "BKE_report.hh"

static PaletteColor *rna_Palette_unshaded_color_new(Palette *palette)
{
  if (ID_IS_LINKED(palette) || ID_IS_OVERRIDE_LIBRARY(palette)) {
    return nullptr;
  }

  PaletteColor *color = BKE_palette_unshaded_color_add(palette);
  return color;
}

static void rna_Palette_unshaded_color_remove(Palette *palette,
                                              ReportList *reports,
                                              PointerRNA *color_ptr)
{
  if (ID_IS_LINKED(palette) || ID_IS_OVERRIDE_LIBRARY(palette)) {
    return;
  }

  PaletteColor *color = static_cast<PaletteColor *>(color_ptr->data);

  if (BLI_findindex(&palette->unshaded_colors, color) == -1) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Palette '%s' does not contain unshaded color given",
                palette->id.name + 2);
    return;
  }

  BKE_palette_unshaded_color_remove(palette, color);

  RNA_POINTER_INVALIDATE(color_ptr);
}

static void rna_Palette_unshaded_color_move(Palette *palette,
                                            ReportList *reports,
                                            PointerRNA *color_ptr,
                                            const int direction)
{
  if (ID_IS_LINKED(palette) || ID_IS_OVERRIDE_LIBRARY(palette)) {
    return;
  }

  PaletteColor *color = static_cast<PaletteColor *>(color_ptr->data);

  if (BLI_findindex(&palette->unshaded_colors, color) == -1) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Palette '%s' does not contain unshaded color given",
                palette->id.name + 2);
    return;
  }

  BLI_assert(ELEM(direction, -1, 0, 1));
  BLI_listbase_link_move(&palette->unshaded_colors, color, direction);

  RNA_POINTER_INVALIDATE(color_ptr);
}

static MixingColor *rna_Palette_mixing_color_new(Palette *palette)
{
  if (ID_IS_LINKED(palette) || ID_IS_OVERRIDE_LIBRARY(palette)) {
    return nullptr;
  }

  MixingColor *color = BKE_palette_mixing_color_add(palette);
  return color;
}

static void rna_Palette_mixing_color_remove(Palette *palette,
                                            ReportList *reports,
                                            PointerRNA *color_ptr)
{
  if (ID_IS_LINKED(palette) || ID_IS_OVERRIDE_LIBRARY(palette)) {
    return;
  }

  MixingColor *color = static_cast<MixingColor *>(color_ptr->data);

  if (BLI_findindex(&palette->mixing_colors, color) == -1) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Palette '%s' does not contain mixing color given",
                palette->id.name + 2);
    return;
  }

  BKE_palette_mixing_color_remove(palette, color);

  RNA_POINTER_INVALIDATE(color_ptr);
}

static void rna_Palette_mixing_color_move(Palette *palette,
                                          ReportList *reports,
                                          PointerRNA *color_ptr,
                                          const int direction)
{
  if (ID_IS_LINKED(palette) || ID_IS_OVERRIDE_LIBRARY(palette)) {
    return;
  }

  MixingColor *color = static_cast<MixingColor *>(color_ptr->data);

  if (BLI_findindex(&palette->mixing_colors, color) == -1) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Palette '%s' does not contain mixing color given",
                palette->id.name + 2);
    return;
  }

  BLI_assert(ELEM(direction, -1, 0, 1));
  BLI_listbase_link_move(&palette->mixing_colors, color, direction);

  RNA_POINTER_INVALIDATE(color_ptr);
}

static void rna_Palette_mixing_colors_mix(Palette *palette,
                                          const float water_portion,
                                          float mixed_color[3])
{
  OD_mix_palette_colors_by_portion(palette, water_portion, mixed_color);
}

static void rna_Palette_mix_two_colors(Palette * /*palette*/,
                                       const float color_a[3],
                                       const float color_b[3],
                                       const float factor_a,
                                       float mixed_color[3])
{
  OD_mix_two_colors_in_spectral_space(color_a, color_b, factor_a, mixed_color);
}

static MixingColor *rna_PaletteColor_mixed_color_new(PaletteColor *color)
{
  MixingColor *mixcolor = BKE_palettecolor_mixed_color_add(color);
  return mixcolor;
}

static void rna_PaletteColor_mixed_color_remove(PaletteColor *palcolor, PointerRNA *mixcolor_ptr)
{
  MixingColor *mixcolor = static_cast<MixingColor *>(mixcolor_ptr->data);

  if (BLI_findindex(&palcolor->mixed_colors, mixcolor) == -1) {
    return;
  }

  BKE_palettecolor_mixed_color_remove(palcolor, mixcolor);

  RNA_POINTER_INVALIDATE(mixcolor_ptr);
}

static void rna_PaletteColor_mixed_color_clear(PaletteColor *color)
{
  BKE_palettecolor_mixed_color_clear(color);
}

static PaletteColor *rna_Palette_last_used_color_new(Palette *palette, const int max_entries)
{
  if (ID_IS_LINKED(palette) || ID_IS_OVERRIDE_LIBRARY(palette)) {
    return nullptr;
  }

  PaletteColor *color = BKE_palette_last_used_color_add(palette, max_entries);
  return color;
}

static PaletteColor *rna_Palette_color_new(Palette *palette)
{
  if (!ID_IS_EDITABLE(palette) || ID_IS_OVERRIDE_LIBRARY(palette)) {
    return nullptr;
  }

  PaletteColor *color = BKE_palette_color_add(palette);
  return color;
}

static void rna_Palette_color_remove(Palette *palette, ReportList *reports, PointerRNA *color_ptr)
{
  if (!ID_IS_EDITABLE(palette) || ID_IS_OVERRIDE_LIBRARY(palette)) {
    return;
  }

  PaletteColor *color = static_cast<PaletteColor *>(color_ptr->data);

  if (BLI_findindex(&palette->colors, color) == -1) {
    BKE_reportf(
        reports, RPT_ERROR, "Palette '%s' does not contain color given", palette->id.name + 2);
    return;
  }

  BKE_palette_color_remove(palette, color);

  RNA_POINTER_INVALIDATE(color_ptr);
}

static void rna_Palette_color_clear(Palette *palette)
{
  if (!ID_IS_EDITABLE(palette) || ID_IS_OVERRIDE_LIBRARY(palette)) {
    return;
  }

  BKE_palette_clear(palette);
}

static PointerRNA rna_Palette_active_color_get(PointerRNA *ptr)
{
  Palette *palette = static_cast<Palette *>(ptr->data);
  PaletteColor *color;

  color = static_cast<PaletteColor *>(BLI_findlink(&palette->colors, palette->active_color));

  if (color) {
    return rna_pointer_inherit_refine(ptr, &RNA_PaletteColor, color);
  }

  return rna_pointer_inherit_refine(ptr, nullptr, nullptr);
}

static void rna_Palette_active_color_set(PointerRNA *ptr,
                                         PointerRNA value,
                                         ReportList * /*reports*/)
{
  Palette *palette = static_cast<Palette *>(ptr->data);
  const PaletteColor *color = static_cast<const PaletteColor *>(value.data);

  /* -1 is ok for an unset index */
  if (color == nullptr) {
    palette->active_color = -1;
  }
  else {
    palette->active_color = BLI_findindex(&palette->colors, color);
  }
}

#else

/* palette.colors */
static void rna_def_palettecolors(BlenderRNA *brna, PropertyRNA *cprop)
{
  StructRNA *srna;
  PropertyRNA *prop;

  FunctionRNA *func;
  PropertyRNA *parm;

  RNA_def_property_srna(cprop, "PaletteColors");
  srna = RNA_def_struct(brna, "PaletteColors", nullptr);
  RNA_def_struct_sdna(srna, "Palette");
  RNA_def_struct_ui_text(srna, "Palette Splines", "Collection of palette colors");

  func = RNA_def_function(srna, "new", "rna_Palette_color_new");
  RNA_def_function_ui_description(func, "Add a new color to the palette");
  parm = RNA_def_pointer(func, "color", "PaletteColor", "", "The newly created color");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "remove", "rna_Palette_color_remove");
  RNA_def_function_ui_description(func, "Remove a color from the palette");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_pointer(func, "color", "PaletteColor", "", "The color to remove");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, ParameterFlag(0));

  func = RNA_def_function(srna, "clear", "rna_Palette_color_clear");
  RNA_def_function_ui_description(func, "Remove all colors from the palette");

  prop = RNA_def_property(srna, "active", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "PaletteColor");
  RNA_def_property_pointer_funcs(
      prop, "rna_Palette_active_color_get", "rna_Palette_active_color_set", nullptr, nullptr);
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Active Palette Color", "");
}

/* palette.last_used_colors */
static void rna_def_palettecolors_last_used(BlenderRNA *brna, PropertyRNA *cprop)
{
  StructRNA *srna;

  FunctionRNA *func;
  PropertyRNA *parm;

  RNA_def_property_srna(cprop, "PaletteColorsLastUsed");
  srna = RNA_def_struct(brna, "PaletteColorsLastUsed", nullptr);
  RNA_def_struct_sdna(srna, "Palette");
  RNA_def_struct_ui_text(srna, "Last Used Splines", "Collection of last used colors");

  func = RNA_def_function(srna, "new", "rna_Palette_last_used_color_new");
  RNA_def_function_ui_description(func, "Add a new color to the palette");
  parm = RNA_def_int(
      func, "max_entries", 10, 1, 20, "", "Maximum number of last used colors", 1, 20);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "color", "PaletteColor", "", "The newly created color");
  RNA_def_function_return(func, parm);
}

/* palette.unshaded_colors */
static void rna_def_palettecolors_unshaded(BlenderRNA *brna, PropertyRNA *cprop)
{
  StructRNA *srna;

  FunctionRNA *func;
  PropertyRNA *parm;

  RNA_def_property_srna(cprop, "PaletteColorsUnshaded");
  srna = RNA_def_struct(brna, "PaletteColorsUnshaded", nullptr);
  RNA_def_struct_sdna(srna, "Palette");
  RNA_def_struct_ui_text(srna, "Unshaded Colors", "Collection of unshaded colors");

  func = RNA_def_function(srna, "new", "rna_Palette_unshaded_color_new");
  RNA_def_function_ui_description(func, "Add a new unshaded color to the palette");
  parm = RNA_def_pointer(func, "color", "PaletteColor", "", "The newly created mixing color");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "remove", "rna_Palette_unshaded_color_remove");
  RNA_def_function_ui_description(func, "Remove a unshaded color from the palette");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_pointer(func, "color", "PaletteColor", "", "The unshaded color to remove");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, ParameterFlag(0));

  func = RNA_def_function(srna, "move", "rna_Palette_unshaded_color_move");
  RNA_def_function_ui_description(func, "Move a unshaded color in the palette (change order)");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_pointer(func, "color", "PaletteColor", "", "The unshaded color to move");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  parm = RNA_def_int(
      func, "direction", 0, -1, 1, "", "Direction to move in order: -1 or 1", -1, 1);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
}

/* palette.mixing_colors */
static void rna_def_palettecolors_mixing(BlenderRNA *brna, PropertyRNA *cprop)
{
  StructRNA *srna;

  FunctionRNA *func;
  PropertyRNA *parm;

  RNA_def_property_srna(cprop, "PaletteColorsMixing");
  srna = RNA_def_struct(brna, "PaletteColorsMixing", nullptr);
  RNA_def_struct_sdna(srna, "Palette");
  RNA_def_struct_ui_text(srna, "Mixing Colors", "Collection of mixing colors");

  func = RNA_def_function(srna, "new", "rna_Palette_mixing_color_new");
  RNA_def_function_ui_description(func, "Add a new mixing color to the palette");
  parm = RNA_def_pointer(func, "color", "MixingColor", "", "The newly created mixing color");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "remove", "rna_Palette_mixing_color_remove");
  RNA_def_function_ui_description(func, "Remove a mixing color from the palette");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_pointer(func, "color", "MixingColor", "", "The mixing color to remove");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, ParameterFlag(0));

  func = RNA_def_function(srna, "move", "rna_Palette_mixing_color_move");
  RNA_def_function_ui_description(func, "Move a mixing color in the palette (change order)");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_pointer(func, "color", "MixingColor", "", "The mixing color to move");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  parm = RNA_def_int(
      func, "direction", 0, -1, 1, "", "Direction to move in order: -1 or 1", -1, 1);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);

#  ifdef WITH_ONDINE
  func = RNA_def_function(srna, "mix", "rna_Palette_mixing_colors_mix");
  RNA_def_function_ui_description(func,
                                  "Get the mixed color based on the portioned mixing colors");
  parm = RNA_def_float(func,
                       "water_portion",
                       0.0f,
                       0.0f,
                       10.0f,
                       "Water Portion",
                       "Portion of water to the mix",
                       0.0f,
                       10.0f);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_float_color(
      func, "mixed_color", 3, nullptr, -FLT_MAX, FLT_MAX, "Mixed Color", "", -FLT_MAX, FLT_MAX);
  RNA_def_parameter_flags(parm, PROP_THICK_WRAP, ParameterFlag(0));
  RNA_def_function_output(func, parm);
#  endif  // WITH_ONDINE
}

/* palettecolor.mixed_colors */
static void rna_def_palettecolor_mixed(BlenderRNA *brna, PropertyRNA *cprop)
{
  StructRNA *srna;

  FunctionRNA *func;
  PropertyRNA *parm;

  RNA_def_property_srna(cprop, "PaletteColorMixed");
  srna = RNA_def_struct(brna, "PaletteColorMixed", nullptr);
  RNA_def_struct_sdna(srna, "PaletteColor");
  RNA_def_struct_ui_text(srna, "Mixed Colors", "Collection of mixed colors");

  func = RNA_def_function(srna, "new", "rna_PaletteColor_mixed_color_new");
  RNA_def_function_ui_description(func, "Add a new mix color to the palette color");
  parm = RNA_def_pointer(func, "color", "MixingColor", "", "The newly created mix color");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "remove", "rna_PaletteColor_mixed_color_remove");
  RNA_def_function_ui_description(func, "Remove a mix color from the palette color");
  parm = RNA_def_pointer(func, "color", "MixingColor", "", "The mix color to remove");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, ParameterFlag(0));

  func = RNA_def_function(srna, "clear", "rna_PaletteColor_mixed_color_clear");
  RNA_def_function_ui_description(func, "Remove all mixed colors from the palette color");
}

static void rna_def_palettecolor(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "PaletteColor", nullptr);
  RNA_def_struct_ui_text(srna, "Palette Color", "");

  prop = RNA_def_property(srna, "mixed_colors", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "MixingColor");
  rna_def_palettecolor_mixed(brna, prop);

  prop = RNA_def_property(srna, "color", PROP_FLOAT, PROP_COLOR_GAMMA);
  RNA_def_property_range(prop, 0.0, 1.0);
  RNA_def_property_float_sdna(prop, nullptr, "rgb");
  RNA_def_property_flag(prop, PROP_LIB_EXCEPTION);
  RNA_def_property_array(prop, 3);
  RNA_def_property_ui_text(prop, "Color", "");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "strength", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 1.0);
  RNA_def_property_float_sdna(prop, nullptr, "value");
  RNA_def_property_ui_text(prop, "Value", "");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "weight", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 1.0);
  RNA_def_property_float_sdna(prop, nullptr, "value");
  RNA_def_property_ui_text(prop, "Weight", "");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "shading_factor", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "shading_factor");
  RNA_def_property_range(prop, -1.0f, 1.0f);
  RNA_def_property_float_default(prop, 0.0f);
  RNA_def_property_ui_text(prop, "Shading Factor", "");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "water_portion", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "water_portion");
  RNA_def_property_range(prop, 0.0f, 10.0f);
  RNA_def_property_float_default(prop, 0.0f);
  RNA_def_property_ui_text(prop, "Portion of Water in Mix", "");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);
}

static void rna_def_mixingcolor(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "MixingColor", nullptr);
  RNA_def_struct_ui_text(srna, "Mixing Color", "");

  prop = RNA_def_property(srna, "color", PROP_FLOAT, PROP_COLOR_GAMMA);
  RNA_def_property_range(prop, 0.0, 1.0);
  RNA_def_property_float_sdna(prop, nullptr, "rgb");
  RNA_def_property_flag(prop, PROP_LIB_EXCEPTION);
  RNA_def_property_array(prop, 3);
  RNA_def_property_ui_text(prop, "Color", "");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "paint_id", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, nullptr, "paint_id");
  RNA_def_property_ui_text(prop, "Paint ID", "");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "portion", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 10.0);
  RNA_def_property_float_sdna(prop, nullptr, "portion");
  RNA_def_property_ui_text(prop, "Portion", "");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);
}

static void rna_def_palette(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "Palette", "ID");
  RNA_def_struct_ui_text(srna, "Palette", "");
  RNA_def_struct_ui_icon(srna, ICON_COLOR);

  prop = RNA_def_property(srna, "colors", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "PaletteColor");
  rna_def_palettecolors(brna, prop);

  prop = RNA_def_property(srna, "last_used_colors", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "PaletteColor");
  rna_def_palettecolors_last_used(brna, prop);

  prop = RNA_def_property(srna, "unshaded_colors", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "PaletteColor");
  rna_def_palettecolors_unshaded(brna, prop);

  prop = RNA_def_property(srna, "mixing_colors", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "MixingColor");
  rna_def_palettecolors_mixing(brna, prop);

  prop = RNA_def_property(srna, "shader_count", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_int_sdna(prop, nullptr, "shader_count");
  prop = RNA_def_property(srna, "darker_shades", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_int_sdna(prop, nullptr, "darker_shades");
  prop = RNA_def_property(srna, "lighter_shades", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_int_sdna(prop, nullptr, "lighter_shades");

#  ifdef WITH_ONDINE
  FunctionRNA *func;
  PropertyRNA *parm;

  func = RNA_def_function(srna, "mix", "rna_Palette_mix_two_colors");
  RNA_def_function_ui_description(func, "Mix two linear srgb colors in spectral color space");
  parm = RNA_def_float_color(
      func, "color_a", 3, nullptr, -FLT_MAX, FLT_MAX, "Color A", "", -FLT_MAX, FLT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_float_color(
      func, "color_b", 3, nullptr, -FLT_MAX, FLT_MAX, "Color B", "", -FLT_MAX, FLT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_float(func,
                       "factor",
                       0.0f,
                       0.0f,
                       1.0f,
                       "Mix Factor",
                       "Mix factor, lower means more color A",
                       0.0f,
                       1.0f);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_float_color(
      func, "mixed_color", 3, nullptr, -FLT_MAX, FLT_MAX, "Mixed Color", "", -FLT_MAX, FLT_MAX);
  RNA_def_parameter_flags(parm, PROP_THICK_WRAP, ParameterFlag(0));
  RNA_def_function_output(func, parm);
#  endif  // WITH_ONDINE
}

void RNA_def_palette(BlenderRNA *brna)
{
  /* *** Non-Animated *** */
  RNA_define_animate_sdna(false);
  rna_def_palettecolor(brna);
  rna_def_mixingcolor(brna);
  rna_def_palette(brna);
  RNA_define_animate_sdna(true);
}

#endif
