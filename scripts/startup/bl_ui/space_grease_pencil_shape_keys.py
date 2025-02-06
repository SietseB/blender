# SPDX-License-Identifier: GPL-2.0-or-later
import bpy


class GREASE_PENCIL_PT_ShapeKeysNPanel(bpy.types.Panel):
    """N-Panel for the shape keys of a Grease Pencil object"""
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = 'Shape Keys'
    bl_label = 'Shape Keys'
    bl_idname = 'GREASE_PENCIL_PT_ShapeKeysNPanel'

    @classmethod
    def poll(cls, context):
        if context.object is None or context.object.type != 'GREASEPENCIL':
            return False
        grease_pencil = context.object.data
        return len(grease_pencil.shape_keys) > 0

    def draw(self, context):
        layout = self.layout
        layout.use_property_decorate = True
        layout.use_property_split = True
        grease_pencil = context.grease_pencil

        col = layout.column(align=True)
        for shape_key in grease_pencil.shape_keys:
            sub = col.split(factor=0.9, align=True)
            sub.prop(shape_key, "value", text=shape_key.name, slider=True)
            row = sub.row(align=True)
            row.use_property_decorate = False
            row.emboss = 'NONE'
            row.prop(shape_key, "mute")

        layout.separator(factor=0.2)


classes = (
    GREASE_PENCIL_PT_ShapeKeysNPanel,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
