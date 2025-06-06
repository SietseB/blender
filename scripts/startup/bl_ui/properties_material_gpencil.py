# SPDX-FileCopyrightText: 2018-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import Menu, Panel, UIList
from rna_prop_ui import PropertyPanel
from bl_ui.utils import PresetPanel
from .space_properties import PropertiesAnimationMixin

from bl_ui.properties_grease_pencil_common import (
    GreasePencilMaterialsPanel,
)


class GPENCIL_MT_material_context_menu(Menu):
    bl_label = "Material Specials"

    def draw(self, _context):
        layout = self.layout
        layout.operator("grease_pencil.material_reveal", icon='RESTRICT_VIEW_OFF', text="Show All")
        layout.operator("grease_pencil.material_hide", icon='RESTRICT_VIEW_ON', text="Hide Others").invert = True

        layout.separator()

        layout.operator("grease_pencil.material_lock_all", icon='LOCKED', text="Lock All")
        layout.operator("grease_pencil.material_unlock_all", icon='UNLOCKED', text="Unlock All")
        layout.operator("grease_pencil.material_lock_unselected", text="Lock Unselected")
        layout.operator("grease_pencil.material_lock_unused", text="Lock Unused")

        layout.separator()

        layout.operator(
            "grease_pencil.material_copy_to_object",
            text="Copy Material to Selected",
        ).only_active = True
        layout.operator(
            "grease_pencil.material_copy_to_object",
            text="Copy All Materials to Selected",
        ).only_active = False

        layout.operator("object.material_slot_remove_unused")


class GPENCIL_UL_matslots(UIList):
    def draw_item(self, _context, layout, _data, item, icon, _active_data, _active_propname, _index):
        slot = item
        ma = slot.material

        if self.layout_type in {'DEFAULT', 'COMPACT'}:
            row = layout.row(align=True)
            row.label(text="", icon_value=icon)

            if ma is None:
                return

            if (gpcolor := ma.grease_pencil) is None:
                return

            row = layout.row(align=True)
            row.enabled = not gpcolor.lock
            row.prop(ma, "name", text="", emboss=False, icon='NONE')

            row = layout.row(align=True)

            if gpcolor.ghost is True:
                icon = 'ONIONSKIN_OFF'
            else:
                icon = 'ONIONSKIN_ON'
            row.prop(gpcolor, "ghost", text="", icon=icon, emboss=False)
            row.prop(gpcolor, "hide", text="", emboss=False)
            row.prop(gpcolor, "lock", text="", emboss=False)

        elif self.layout_type == 'GRID':
            layout.alignment = 'CENTER'
            layout.label(text="", icon_value=icon)


class GPMaterialButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "material"

    @classmethod
    def poll(cls, context):
        ma = context.material
        return ma and ma.grease_pencil


class MATERIAL_PT_gpencil_slots(GreasePencilMaterialsPanel, Panel):
    bl_label = "Grease Pencil Material Slots"
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "material"
    bl_options = {'HIDE_HEADER'}

    @classmethod
    def poll(cls, context):
        ma = context.material
        found_material = ma and ma.grease_pencil
        found_object = context.object and context.object.type == 'GREASEPENCIL'
        return found_material or found_object


# Used as parent for "Stroke" and "Fill" panels
class MATERIAL_PT_gpencil_surface(GPMaterialButtonsPanel, Panel):
    bl_label = "Surface"

    def draw_header_preset(self, _context):
        MATERIAL_PT_gpencil_material_presets.draw_panel_header(self.layout)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        ma = context.material
        if ma is None or ma.grease_pencil is None:
            return

        layout.prop(ma.grease_pencil, "stroke_wetness")


class GPMaterialColorPalettePanel():
    bl_label = "Color Palette"
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = ".material"
    bl_ui_units_x = 10

    def set_shaded_color_palette_width(self, layout, paint):
        palette = paint.palette
        if palette is None:
            return
        if palette.shader_count == 0:
            return
        layout.ui_units_x = palette.shader_count if palette.shader_count >= 10 else 2 * palette.shader_count


class MATERIAL_PT_gpencil_strokecolor_palette(GPMaterialColorPalettePanel, Panel):
    def draw(self, context):
        layout = self.layout
        paint = context.scene.tool_settings.gpencil_paint
        gpcolor = context.material.grease_pencil
        self.set_shaded_color_palette_width(layout, paint)

        layout.template_ID(paint, "palette", new="palette.new")
        if paint.palette:
            layout.template_palette(paint, "palette", target=gpcolor, target_property="color")


class MATERIAL_PT_gpencil_strokecolor(GPMaterialButtonsPanel, Panel):
    bl_label = "Stroke"
    bl_parent_id = "MATERIAL_PT_gpencil_surface"

    def draw_header(self, context):
        ma = context.material
        if ma is not None and ma.grease_pencil is not None:
            gpcolor = ma.grease_pencil
            self.layout.prop(gpcolor, "show_stroke", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        ma = context.material
        if ma is not None and ma.grease_pencil is not None:
            gpcolor = ma.grease_pencil

            col = layout.column()

            col.prop(gpcolor, "mode")

            col.prop(gpcolor, "stroke_style", text="Style")

            row = col.row(heading="Base Color", align=True)
            row.popover(panel="MATERIAL_PT_gpencil_strokecolor_palette", icon="COLOR", text="")
            row2 = row.row(align=True)
            row2.prop(gpcolor, "color", text="")
            row2.prop_decorator(gpcolor, "color")

            if gpcolor.stroke_style == 'TEXTURE':
                layout.separator()
                col = layout.column()
                row = col.row()
                col = row.column(align=True)
                col.template_ID(gpcolor, "stroke_image", open="image.open")

                col = layout.column()
                col.enabled = not gpcolor.lock
                col.prop(gpcolor, "mix_stroke_factor", text="Blend", slider=True)
                col.prop(gpcolor, "texture_density", slider=True)

                col.separator()
                col.prop(gpcolor, "alignment_rotation", text="Angle")
                col.prop(gpcolor, "texture_angle_variation")
                col.prop(gpcolor, "smooth_texture")


class MATERIAL_PT_gpencil_strokecolor_default(GPMaterialButtonsPanel, Panel):
    bl_label = "Default Render"
    bl_parent_id = "MATERIAL_PT_gpencil_strokecolor"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        ma = context.material
        if ma is None or ma.grease_pencil is None:
            return

        gpcolor = ma.grease_pencil

        col = layout.column()
        col.enabled = not gpcolor.lock

        if gpcolor.mode in {'DOTS', 'BOX'}:
            col.prop(gpcolor, "alignment_mode")

        if gpcolor.stroke_style == 'TEXTURE' and gpcolor.mode == 'LINE':
            col.prop(gpcolor, "pixel_size", text="UV Factor")

        col.prop(gpcolor, "use_stroke_holdout")

        if gpcolor.mode == 'LINE':
            col.prop(gpcolor, "use_overlap_strokes")


class MATERIAL_PT_gpencil_fillcolor_palette(GPMaterialColorPalettePanel, Panel):
    def draw(self, context):
        layout = self.layout
        paint = context.scene.tool_settings.gpencil_paint
        gpcolor = context.material.grease_pencil
        self.set_shaded_color_palette_width(layout, paint)

        layout.template_ID(paint, "palette", new="palette.new")
        if paint.palette:
            layout.template_palette(paint, "palette", target=gpcolor, target_property="fill_color")


class MATERIAL_PT_gpencil_mixcolor_palette(GPMaterialColorPalettePanel, Panel):
    def draw(self, context):
        layout = self.layout
        paint = context.scene.tool_settings.gpencil_paint
        gpcolor = context.material.grease_pencil
        self.set_shaded_color_palette_width(layout, paint)

        layout.template_ID(paint, "palette", new="palette.new")
        if paint.palette:
            layout.template_palette(paint, "palette", target=gpcolor, target_property="mix_color")


class MATERIAL_PT_gpencil_fillcolor(GPMaterialButtonsPanel, Panel):
    bl_label = "Fill"
    bl_parent_id = "MATERIAL_PT_gpencil_surface"

    def draw_header(self, context):
        ma = context.material
        gpcolor = ma.grease_pencil
        self.layout.prop(gpcolor, "show_fill", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        ma = context.material
        gpcolor = ma.grease_pencil

        # color settings
        col = layout.column()
        col.prop(gpcolor, "fill_style", text="Style")

        if gpcolor.fill_style == 'GRADIENT':
            col.prop(gpcolor, "gradient_type")

        if gpcolor.fill_style == 'GRADIENT' or gpcolor.fill_style == 'SOLID':
            row = col.row(heading="Base Color", align=True)
            row.popover(panel="MATERIAL_PT_gpencil_fillcolor_palette", icon="COLOR", text="")
            row2 = row.row(align=True)
            row2.prop(gpcolor, "fill_color", text="")
            row2.prop_decorator(gpcolor, "fill_color")

        if gpcolor.fill_style == 'GRADIENT':
            row = col.row(heading="Secondary Color", align=True)
            row.popover(panel="MATERIAL_PT_gpencil_mixcolor_palette", icon="COLOR", text="")
            row2 = row.row(align=True)
            row2.prop(gpcolor, "mix_color", text="")
            row2.prop_decorator(gpcolor, "mix_color")

            col.prop(gpcolor, "mix_factor", text="Blend", slider=True)
            col.prop(gpcolor, "flip", text="Flip Colors")


class MATERIAL_PT_gpencil_fillcolor_ondine(GPMaterialButtonsPanel, Panel):
    bl_label = "Ondine Render"
    bl_parent_id = "MATERIAL_PT_gpencil_fillcolor"

    @classmethod
    def poll(cls, context):
        ma = context.material
        if ma is None or ma.grease_pencil is None:
            return False
        gpcolor = ma.grease_pencil
        return gpcolor.fill_style == 'GRADIENT'

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        ma = context.material
        gpcolor = ma.grease_pencil

        col = layout.column()
        col.enabled = not gpcolor.lock

        if gpcolor.fill_style == 'GRADIENT':
            if gpcolor.gradient_type == 'LINEAR':
                col.prop(gpcolor, "texture_angle", text="Rotation")

                col = layout.column(align=True)
                col.enabled = not gpcolor.lock
                col.prop(gpcolor, "ondine_gradient_offset", text="Base Offset", index=0)
                col.prop(gpcolor, "ondine_gradient_offset", text="Secondary", index=1)
            else:
                col.prop(gpcolor, "ondine_gradient_offset", text="Offset")
                col.prop(gpcolor, "ondine_gradient_scale", text="Scale")


class MATERIAL_PT_gpencil_fillcolor_default(GPMaterialButtonsPanel, Panel):
    bl_label = "Default Render"
    bl_parent_id = "MATERIAL_PT_gpencil_fillcolor"

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        ma = context.material
        if ma is None or ma.grease_pencil is None:
            return

        gpcolor = ma.grease_pencil

        col = layout.column()
        col.enabled = not gpcolor.lock

        if gpcolor.fill_style == 'GRADIENT':
            col.prop(gpcolor, "texture_offset", text="Location")

            row = col.row()
            row.enabled = gpcolor.gradient_type == 'LINEAR'
            row.prop(gpcolor, "texture_angle", text="Rotation")
            col.prop(gpcolor, "texture_scale", text="Scale")
            col.prop(gpcolor, "use_fill_holdout")

        elif gpcolor.fill_style == 'SOLID':
            col.prop(gpcolor, "use_fill_holdout")

        elif gpcolor.fill_style == 'TEXTURE':
            row = col.row(heading="Base Color", align=True)
            row.popover(panel="MATERIAL_PT_gpencil_fillcolor_palette", icon="COLOR", text="")
            row2 = row.row(align=True)
            row2.prop(gpcolor, "fill_color", text="")
            row2.prop_decorator(gpcolor, "fill_color")

            col.prop(gpcolor, "use_fill_holdout")

            col.separator()
            col.template_ID(gpcolor, "fill_image", open="image.open")

            col.prop(gpcolor, "mix_factor", text="Blend", slider=True)

            col.prop(gpcolor, "texture_offset", text="Location")
            col.prop(gpcolor, "texture_angle", text="Rotation")
            col.prop(gpcolor, "texture_scale", text="Scale")
            col.prop(gpcolor, "texture_clamp", text="Clip Image")


class MATERIAL_PT_gpencil_animation(GPMaterialButtonsPanel, PropertiesAnimationMixin, PropertyPanel, Panel):
    _animated_id_context_property = "material"


class MATERIAL_PT_gpencil_preview(GPMaterialButtonsPanel, Panel):
    bl_label = "Preview"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        ma = context.material
        self.layout.template_preview(ma)


class MATERIAL_PT_gpencil_custom_props(GPMaterialButtonsPanel, PropertyPanel, Panel):
    COMPAT_ENGINES = {'BLENDER_WORKBENCH'}
    _context_path = "object.active_material"
    _property_type = bpy.types.Material


class MATERIAL_PT_gpencil_settings(GPMaterialButtonsPanel, Panel):
    bl_label = "Settings"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        ma = context.material
        gpcolor = ma.grease_pencil
        layout.prop(gpcolor, "pass_index")


class MATERIAL_PT_gpencil_material_presets(PresetPanel, Panel):
    """Material settings"""
    bl_label = "Material Presets"
    preset_subdir = "gpencil_material"
    preset_operator = "script.execute_preset"
    preset_add_operator = "scene.gpencil_material_preset_add"


classes = (
    GPENCIL_UL_matslots,
    GPENCIL_MT_material_context_menu,
    MATERIAL_PT_gpencil_slots,
    MATERIAL_PT_gpencil_preview,
    MATERIAL_PT_gpencil_material_presets,
    MATERIAL_PT_gpencil_surface,
    MATERIAL_PT_gpencil_strokecolor,
    MATERIAL_PT_gpencil_strokecolor_default,
    MATERIAL_PT_gpencil_strokecolor_palette,
    MATERIAL_PT_gpencil_fillcolor,
    MATERIAL_PT_gpencil_fillcolor_ondine,
    MATERIAL_PT_gpencil_fillcolor_default,
    MATERIAL_PT_gpencil_fillcolor_palette,
    MATERIAL_PT_gpencil_mixcolor_palette,
    MATERIAL_PT_gpencil_settings,
    MATERIAL_PT_gpencil_animation,
    MATERIAL_PT_gpencil_custom_props,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class

    for cls in classes:
        register_class(cls)
