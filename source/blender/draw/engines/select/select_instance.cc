/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup select
 */

#include "DRW_render.hh"

#include "BLT_translation.hh"

#include "select_engine.hh"

#include "../overlay/overlay_next_instance.hh"
#include "select_instance.hh"

using namespace blender::draw;

/* -------------------------------------------------------------------- */
/** \name Select-Next Engine
 * \{ */

using Instance = overlay::Instance;

struct SELECT_NextData {
  void *engine_type;
  Instance *instance;
};

static void SELECT_next_engine_init(void *vedata)
{
  OVERLAY_Data *ved = reinterpret_cast<OVERLAY_Data *>(vedata);

  if (ved->instance == nullptr) {
    ved->instance = new Instance(select::SelectionType::ENABLED);
  }
  reinterpret_cast<Instance *>(ved->instance)->init();
}

static void SELECT_next_cache_init(void *vedata)
{
  reinterpret_cast<Instance *>(reinterpret_cast<OVERLAY_Data *>(vedata)->instance)->begin_sync();
}

static void SELECT_next_cache_populate(void *vedata, blender::draw::ObjectRef &ob_ref)
{
  reinterpret_cast<Instance *>(reinterpret_cast<OVERLAY_Data *>(vedata)->instance)
      ->object_sync(ob_ref, *DRW_manager_get());
}

static void SELECT_next_cache_finish(void *vedata)
{
  reinterpret_cast<Instance *>(reinterpret_cast<OVERLAY_Data *>(vedata)->instance)->end_sync();
}

static void SELECT_next_draw_scene(void *vedata)
{
  DRW_submission_start();
  reinterpret_cast<Instance *>(reinterpret_cast<OVERLAY_Data *>(vedata)->instance)
      ->draw(*DRW_manager_get());
  DRW_submission_end();
}

static void SELECT_next_instance_free(void *instance_)
{
  Instance *instance = (Instance *)instance_;
  delete instance;
}

DrawEngineType draw_engine_select_next_type = {
    /*next*/ nullptr,
    /*prev*/ nullptr,
    /*idname*/ N_("Select-Next"),
    /*engine_init*/ &SELECT_next_engine_init,
    /*engine_free*/ nullptr,
    /*instance_free*/ &SELECT_next_instance_free,
    /*cache_init*/ &SELECT_next_cache_init,
    /*cache_populate*/ &SELECT_next_cache_populate,
    /*cache_finish*/ &SELECT_next_cache_finish,
    /*draw_scene*/ &SELECT_next_draw_scene,
    /*render_to_image*/ nullptr,
    /*store_metadata*/ nullptr,
};

/** \} */
