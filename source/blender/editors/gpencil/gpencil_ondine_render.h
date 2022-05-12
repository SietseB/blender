#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void gpencil_ondine_render_set_data(Object *ob);
void gpencil_ondine_render_set_zdepth(Object *ob);

void gpencil_ondine_set_render_data(Object *ob);
void gpencil_ondine_set_zdepth(Object *ob);
bool gpencil_ondine_render_init(bContext *C);


#ifdef __cplusplus
}
#endif
