/* SPDX-FileCopyrightText: 2004 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup sequencer
 */

#include "SEQ_render.hh" /* Needed for #eSeqTaskId. */

struct ImBuf;
struct Scene;
struct SeqRenderData;
struct Strip;

namespace blender::seq {

struct SeqCache;

struct SeqCacheKey {
  SeqCache *cache_owner;
  void *userkey;
  SeqCacheKey *link_prev; /* Used for linking intermediate items to final frame. */
  SeqCacheKey *link_next; /* Used for linking intermediate items to final frame. */
  Strip *strip;
  RenderData context;
  float frame_index;  /* Usually same as timeline_frame. Mapped to media for RAW entries. */
  float cost;         /* In short: render time(s) divided by playback frame duration(s) */
  bool is_temp_cache; /* this cache entry will be freed before rendering next frame */
  /* ID of task for assigning temp cache entries to particular task(thread, etc.) */
  eTaskId task_id;
  int type;
};

ImBuf *seq_cache_get(const RenderData *context, Strip *strip, float timeline_frame, int type);
void seq_cache_put(
    const RenderData *context, Strip *strip, float timeline_frame, int type, ImBuf *i);
bool seq_cache_put_if_possible(
    const RenderData *context, Strip *strip, float timeline_frame, int type, ImBuf *ibuf);
/**
 * Find only "base" keys.
 * Sources(other types) for a frame must be freed all at once.
 */
bool seq_cache_recycle_item(Scene *scene);
void seq_cache_free_temp_cache(Scene *scene, short id, int timeline_frame);
void seq_cache_destruct(Scene *scene);
void seq_cache_cleanup_strip(Scene *scene,
                             Strip *strip,
                             Strip *strip_changed,
                             int invalidate_types,
                             bool force_seq_changed_range);
bool seq_cache_is_full();

}  // namespace blender::seq
