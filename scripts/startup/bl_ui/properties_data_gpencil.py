# SPDX-FileCopyrightText: 2018-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import Menu, Panel, UIList
from rna_prop_ui import PropertyPanel

from bl_ui.properties_grease_pencil_common import (
    GreasePencilLayerMasksPanel,
    GreasePencilLayerTransformPanel,
    GreasePencilLayerAdjustmentsPanel,
    GreasePencilLayerRelationsPanel,
    GreasePencilLayerDisplayPanel,
)

###############################
# Base-Classes (for shared stuff - e.g. poll, attributes, etc.)


class DataButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "data"

    @classmethod
    def poll(cls, context):
        return context.gpencil


class ObjectButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "data"

    @classmethod
    def poll(cls, context):
        ob = context.object
        return ob and ob.type == 'GPENCIL'


class LayerDataButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "data"

    @classmethod
    def poll(cls, context):
        gpencil = context.gpencil
        return gpencil and gpencil.layers.active


###############################
# GP Object Properties Panels and Helper Classes

class DATA_PT_context_gpencil(DataButtonsPanel, Panel):
    bl_label = ""
    bl_options = {'HIDE_HEADER'}

    def draw(self, context):
        layout = self.layout

        ob = context.object
        space = context.space_data

        if ob:
            layout.template_ID(ob, "data")
        else:
            layout.template_ID(space, "pin_id")


class GPENCIL_MT_layer_context_menu(Menu):
    bl_label = "Layer Specials"

    def draw(self, context):
        layout = self.layout
        ob = context.object
        gpd = ob.data
        gpl = gpd.layers.active

        layout.operator("gpencil.layer_duplicate", text="Duplicate", icon='DUPLICATE').mode = 'ALL'
        layout.operator("gpencil.layer_duplicate", text="Duplicate Empty Keyframes").mode = 'EMPTY'

        layout.separator()

        layout.operator("gpencil.reveal", icon='RESTRICT_VIEW_OFF', text="Show All")
        layout.operator("gpencil.hide", icon='RESTRICT_VIEW_ON', text="Hide Others").unselected = True

        layout.separator()

        layout.operator("gpencil.lock_all", icon='LOCKED', text="Lock All")
        layout.operator("gpencil.unlock_all", icon='UNLOCKED', text="Unlock All")
        layout.prop(gpd, "use_autolock_layers", text="Autolock Inactive Layers")
        layout.prop(gpl, "lock_material")

        layout.separator()

        layout.operator("gpencil.layer_merge", icon='SORT_ASC', text="Merge Down").mode = 'ACTIVE'
        layout.operator("gpencil.layer_merge", text="Merge All").mode = 'ALL'

        layout.separator()
        layout.operator("gpencil.layer_duplicate_object", text="Copy Layer to Selected").only_active = True
        layout.operator("gpencil.layer_duplicate_object", text="Copy All Layers to Selected").only_active = False


class DATA_PT_gpencil_layers(DataButtonsPanel, Panel):
    bl_label = "Layers"

    def draw(self, context):
        layout = self.layout
        layout.use_property_decorate = False

        gpd = context.gpencil

        # Grease Pencil data...
        if (gpd is None) or (not gpd.layers):
            layout.operator("gpencil.layer_add", text="New Layer")
        else:
            self.draw_layers(context, layout, gpd)

    def draw_layers(self, _context, layout, gpd):

        gpl = gpd.layers.active

        row = layout.row()
        layer_rows = 7

        col = row.column()
        col.template_list(
            "GPENCIL_UL_layer", "", gpd, "layers", gpd.layers, "active_index",
            rows=layer_rows, sort_reverse=True, sort_lock=True,
        )

        col = row.column()
        sub = col.column(align=True)
        sub.operator("gpencil.layer_add", icon='ADD', text="")
        sub.operator("gpencil.layer_remove", icon='REMOVE', text="")

        sub.separator()

        if gpl:
            sub.menu("GPENCIL_MT_layer_context_menu", icon='DOWNARROW_HLT', text="")

            if len(gpd.layers) > 1:
                col.separator()

                sub = col.column(align=True)
                sub.operator("gpencil.layer_move", icon='TRIA_UP', text="").type = 'UP'
                sub.operator("gpencil.layer_move", icon='TRIA_DOWN', text="").type = 'DOWN'

                col.separator()

                sub = col.column(align=True)
                sub.operator("gpencil.layer_isolate", icon='RESTRICT_VIEW_ON', text="").affect_visibility = True
                sub.operator("gpencil.layer_isolate", icon='LOCKED', text="").affect_visibility = False

        # Layer main properties
        row = layout.row()
        col = layout.column(align=True)

        if gpl:
            layout = self.layout
            layout.use_property_split = True
            layout.use_property_decorate = True
            col = layout.column(align=True)

            if not gpd.watercolor:
                col = layout.row(align=True)
                col.prop(gpl, "blend_mode", text="Blend")

            if not gpd.watercolor:
                col = layout.row(align=True)
                col.prop(gpl, "opacity", text="Opacity", slider=True)
                col = layout.row(align=True)
                col.prop(gpl, "use_lights")

            if gpd.watercolor:
                layout.use_property_split = False

                row = layout.row()
                col = row.column()
                col.scale_x = 0.78
                col.label(text="")
                col = row.column()
                col.prop(gpl, "clear_underlying")
                col.prop(gpl, "mix_with_underlying")
                col.prop(gpl, "limit_to_underlying")
                col = row.column()
                col.prop(gpl, "gouache_style")
                col = col.column()
                col.enabled = (gpl.stroke_dryness == 0)
                col.prop(gpl, "scale_pigment_flow")
                col.prop(gpl, "is_wetted")

                layout.use_property_split = True
                layout.separator()
                col = layout.column()
                col.prop(gpl, "opacity", text="Opacity", slider=True)
                col = layout.column(align=True)
                col.prop(gpl, "watercolor_alpha_variation", slider=True)
                col.prop(gpl, "watercolor_color_variation", slider=True)

                sub = layout.grid_flow(columns=1, align=True)
                col = sub.column()
                col.enabled = (gpl.stroke_dryness == 0)
                col.prop(gpl, "stroke_wetness", slider=True)
                col = sub.column()
                col.enabled = True
                col.prop(gpl, "stroke_dryness", slider=True)


class DATA_PT_gpencil_layer_masks(LayerDataButtonsPanel, GreasePencilLayerMasksPanel, Panel):
    bl_label = "Masks"
    bl_parent_id = "DATA_PT_gpencil_layers"
    bl_options = {'DEFAULT_CLOSED'}


class DATA_PT_gpencil_layer_default_render(LayerDataButtonsPanel, Panel):
    bl_label = "Default Render"
    bl_parent_id = "DATA_PT_gpencil_layers"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = True

        gpd = context.gpencil
        gpl = gpd.layers.active

        col = layout.column()
        col.prop(gpl, "blend_mode")
        col.prop(gpl, "use_lights")


class DATA_PT_gpencil_layer_transform(LayerDataButtonsPanel, GreasePencilLayerTransformPanel, Panel):
    bl_label = "Transform"
    bl_parent_id = "DATA_PT_gpencil_layers"
    bl_options = {'DEFAULT_CLOSED'}


class DATA_PT_gpencil_layer_adjustments(LayerDataButtonsPanel, GreasePencilLayerAdjustmentsPanel, Panel):
    bl_label = "Adjustments"
    bl_parent_id = "DATA_PT_gpencil_layers"
    bl_options = {'DEFAULT_CLOSED'}


class DATA_PT_gpencil_layer_relations(LayerDataButtonsPanel, GreasePencilLayerRelationsPanel, Panel):
    bl_label = "Relations"
    bl_parent_id = "DATA_PT_gpencil_layers"
    bl_options = {'DEFAULT_CLOSED'}


class DATA_PT_gpencil_layer_display(LayerDataButtonsPanel, GreasePencilLayerDisplayPanel, Panel):
    bl_label = "Display"
    bl_parent_id = "DATA_PT_gpencil_layers"
    bl_options = {'DEFAULT_CLOSED'}


class DATA_PT_gpencil_onion_skinning(DataButtonsPanel, Panel):
    bl_label = "Onion Skinning"

    def draw(self, context):
        gpd = context.gpencil

        layout = self.layout
        layout.use_property_split = True

        col = layout.column()
        col.prop(gpd, "onion_mode")
        col.prop(gpd, "onion_factor", text="Opacity", slider=True)
        col.prop(gpd, "onion_keyframe_type")

        if gpd.onion_mode == 'ABSOLUTE':
            col = layout.column(align=True)
            col.prop(gpd, "ghost_before_range", text="Frames Before")
            col.prop(gpd, "ghost_after_range", text="Frames After")
        elif gpd.onion_mode == 'RELATIVE':
            col = layout.column(align=True)
            col.prop(gpd, "ghost_before_range", text="Keyframes Before")
            col.prop(gpd, "ghost_after_range", text="Keyframes After")


class DATA_PT_gpencil_onion_skinning_custom_colors(DataButtonsPanel, Panel):
    bl_parent_id = "DATA_PT_gpencil_onion_skinning"
    bl_label = "Custom Colors"
    bl_options = {'DEFAULT_CLOSED'}

    def draw_header(self, context):
        gpd = context.gpencil

        self.layout.prop(gpd, "use_ghost_custom_colors", text="")

    def draw(self, context):
        gpd = context.gpencil

        layout = self.layout
        layout.use_property_split = True
        layout.enabled = gpd.users <= 1 and gpd.use_ghost_custom_colors

        layout.prop(gpd, "before_color", text="Before")
        layout.prop(gpd, "after_color", text="After")


class DATA_PT_gpencil_onion_skinning_display(DataButtonsPanel, Panel):
    bl_parent_id = "DATA_PT_gpencil_onion_skinning"
    bl_label = "Display"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        gpd = context.gpencil

        layout = self.layout
        layout.use_property_split = True
        layout.enabled = gpd.users <= 1

        layout.prop(gpd, "use_ghosts_always", text="View in Render")

        col = layout.column(align=True)
        col.prop(gpd, "use_onion_fade", text="Fade")
        sub = layout.column()
        sub.active = gpd.onion_mode in {'RELATIVE', 'SELECTED'}
        sub.prop(gpd, "use_onion_loop", text="Show Start Frame")


class GPENCIL_MT_gpencil_vertex_group(Menu):
    bl_label = "Grease Pencil Vertex Groups"

    def draw(self, context):
        layout = self.layout

        layout.operator_context = 'EXEC_AREA'
        layout.operator("object.vertex_group_add")

        ob = context.active_object
        if ob.vertex_groups.active:
            layout.separator()

            layout.operator("gpencil.vertex_group_assign", text="Assign to Active Group")
            layout.operator("gpencil.vertex_group_remove_from", text="Remove from Active Group")

            layout.separator()
            layout.operator_menu_enum("object.vertex_group_set_active", "group", text="Set Active Group")
            layout.operator("object.vertex_group_remove", text="Remove Active Group").all = False
            layout.operator("object.vertex_group_remove", text="Remove All Groups").all = True

            layout.separator()
            layout.operator("gpencil.vertex_group_select", text="Select Points")
            layout.operator("gpencil.vertex_group_deselect", text="Deselect Points")


class GPENCIL_UL_vgroups(UIList):
    def draw_item(self, _context, layout, _data, item, icon, _active_data, _active_propname, _index):
        vgroup = item
        if self.layout_type in {'DEFAULT', 'COMPACT'}:
            layout.prop(vgroup, "name", text="", emboss=False, icon_value=icon)
            icon = 'LOCKED' if vgroup.lock_weight else 'UNLOCKED'
            layout.prop(vgroup, "lock_weight", text="", icon=icon, emboss=False)
        elif self.layout_type == 'GRID':
            layout.alignment = 'CENTER'
            layout.label(text="", icon_value=icon)


class DATA_PT_gpencil_vertex_groups(ObjectButtonsPanel, Panel):
    bl_label = "Vertex Groups"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout

        ob = context.object
        group = ob.vertex_groups.active

        rows = 2
        if group:
            rows = 4

        row = layout.row()
        row.template_list("GPENCIL_UL_vgroups", "", ob, "vertex_groups", ob.vertex_groups, "active_index", rows=rows)

        col = row.column(align=True)
        col.operator("object.vertex_group_add", icon='ADD', text="")
        col.operator("object.vertex_group_remove", icon='REMOVE', text="").all = False

        if group:
            col.separator()
            col.operator("object.vertex_group_move", icon='TRIA_UP', text="").direction = 'UP'
            col.operator("object.vertex_group_move", icon='TRIA_DOWN', text="").direction = 'DOWN'

        if ob.vertex_groups:
            row = layout.row()

            sub = row.row(align=True)
            sub.operator("gpencil.vertex_group_assign", text="Assign")
            sub.operator("gpencil.vertex_group_remove_from", text="Remove")

            sub = row.row(align=True)
            sub.operator("gpencil.vertex_group_select", text="Select")
            sub.operator("gpencil.vertex_group_deselect", text="Deselect")

            layout.prop(context.tool_settings, "vertex_group_weight", text="Weight")


class DATA_MT_morph_targets_context_menu(Menu):
    bl_label = "Grease Pencil Morph Targets"

    def draw(self, context):
        layout = self.layout

        layout.operator("gpencil.morph_target_duplicate", icon='DUPLICATE', text="Duplicate")
        layout.separator()
        layout.operator("gpencil.morph_target_remove_all", icon='X', text="Delete All")
        layout.operator("gpencil.morph_target_apply_all", icon='NODE_COMPOSITING', text="Apply All")


class DATA_PT_gpencil_morph_targets(DataButtonsPanel, Panel):
    bl_label = "Morph Targets"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout

        gpd = context.gpencil
        gpmt = gpd.morph_targets.active

        row = layout.row()
        mt_rows = 6

        col = row.column()
        col.template_list("GPENCIL_UL_morph_target", "", gpd, "morph_targets", gpd.morph_targets,
                          "active_index", rows=mt_rows, sort_reverse=False)

        col = row.column()
        sub = col.column(align=True)
        sub.operator("gpencil.morph_target_add", icon='ADD', text="")
        sub.operator("gpencil.morph_target_remove", icon='REMOVE', text="")

        if gpmt:
            col.separator()
            col.operator("gpencil.morph_target_move", icon='TRIA_UP', text="").direction = 'UP'
            col.operator("gpencil.morph_target_move", icon='TRIA_DOWN', text="").direction = 'DOWN'

        sub.separator()
        sub.menu("DATA_MT_morph_targets_context_menu", icon='DOWNARROW_HLT', text="")

        if gpmt:
            row = layout.row()
            sub = row.column()
            sub.active = not gpd.in_morph_edit_mode
            sub.operator("gpencil.morph_target_edit", text="Edit")
            sub = row.column()
            sub.active = gpd.in_morph_edit_mode
            sub.operator("gpencil.morph_target_edit_finish", text="Finish Edit", depress=sub.active)

            if sub.active:
                row = layout.row()
                row.label(text="Note: do not add or remove points while editing.", icon='INFO')

            layout.use_property_split = True
            layout.separator()
            col = layout.column()
            col.prop(gpmt, "value", slider=True)

            col = layout.column(align=True)
            col.prop(gpmt, "slider_min", text="Range Min")
            col.prop(gpmt, "slider_max", text="Max")

            if gpmt.morphed_layer_order:
                layout.separator()
                col = layout.column(align=True)
                col.prop(gpmt, "layer_order_compare")
                col.prop(gpmt, "layer_order_value", text=" ", slider=True)


class DATA_PT_gpencil_strokes(DataButtonsPanel, Panel):
    bl_label = "Strokes"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        ob = context.object
        gpd = context.gpencil

        col = layout.column(align=True)
        col.prop(gpd, "stroke_depth_order")

        if ob:
            col.enabled = not ob.show_in_front

        col = layout.column(align=True)
        col.prop(gpd, "stroke_thickness_space")
        sub = col.column()
        sub.active = gpd.stroke_thickness_space == 'WORLDSPACE'
        sub.prop(gpd, "pixel_factor", text="Thickness Scale")

        col.prop(gpd, "edit_curve_resolution")


class DATA_PT_gpencil_display(DataButtonsPanel, Panel):
    bl_label = "Viewport Display"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        gpd = context.gpencil

        layout.prop(gpd, "edit_line_color", text="Edit Line Color")


class DATA_PT_gpencil_canvas(DataButtonsPanel, Panel):
    bl_label = "Canvas"
    bl_parent_id = "DATA_PT_gpencil_display"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        gpd = context.gpencil
        grid = gpd.grid

        row = layout.row(align=True)
        col = row.column()
        col.prop(grid, "color", text="Color")
        col.prop(grid, "scale", text="Scale")
        col.prop(grid, "offset")
        row = layout.row(align=True)
        col = row.column()
        col.prop(grid, "lines", text="Subdivisions")


class DATA_PT_custom_props_gpencil(DataButtonsPanel, PropertyPanel, Panel):
    _context_path = "object.data"
    _property_type = bpy.types.GreasePencil


###############################


classes = (
    DATA_PT_context_gpencil,
    DATA_PT_gpencil_layers,
    DATA_PT_gpencil_onion_skinning,
    DATA_PT_gpencil_onion_skinning_custom_colors,
    DATA_PT_gpencil_onion_skinning_display,
    DATA_PT_gpencil_layer_masks,
    DATA_PT_gpencil_layer_default_render,
    DATA_PT_gpencil_layer_transform,
    DATA_PT_gpencil_layer_adjustments,
    DATA_PT_gpencil_layer_relations,
    DATA_PT_gpencil_layer_display,
    DATA_PT_gpencil_vertex_groups,
    DATA_PT_gpencil_morph_targets,
    DATA_PT_gpencil_strokes,
    DATA_PT_gpencil_display,
    DATA_PT_gpencil_canvas,
    DATA_PT_custom_props_gpencil,

    GPENCIL_UL_vgroups,

    GPENCIL_MT_layer_context_menu,
    GPENCIL_MT_gpencil_vertex_group,
    DATA_MT_morph_targets_context_menu,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class

    for cls in classes:
        register_class(cls)
