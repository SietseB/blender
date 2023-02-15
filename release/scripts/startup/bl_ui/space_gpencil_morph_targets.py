# SPDX-License-Identifier: GPL-2.0-or-later
import bpy

class GPENCIL_PT_MorphTargetsNPanel(bpy.types.Panel):
    """N-Panel for morph targets of Grease Pencil object"""
    bl_label = 'Morph Targets'
    bl_idname = 'GPENCIL_PT_MorphTargetsPanel'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = 'GP Morphs'

    @classmethod
    def poll(cls, context):
        if context.object is None or context.object.type != 'GPENCIL':
            return False
        gpd = context.object.data
        return len(gpd.morph_targets) > 0
    
    def draw(self, context):
        layout = self.layout
        layout.use_property_decorate = True
        layout.use_property_split = True
        gpd = context.object.data
        
        mt_list = [mt for mt in gpd.morph_targets]
        col = layout.column(align=True)
        for mt in sorted(mt_list, key=lambda el: el.order_nr):
            sub = col.split(factor=0.9, align=True)
            sub.prop(mt, "value", text=mt.name)
            row = sub.row(align=True)
            row.use_property_decorate = False
            row.emboss = 'NONE'
            row.prop(mt, "mute")
        
        layout.separator(factor=0.2)


classes = (
    GPENCIL_PT_MorphTargetsNPanel,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
