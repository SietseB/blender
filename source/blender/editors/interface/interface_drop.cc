/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "BLI_listbase.h"

#include "UI_interface.hh"

namespace blender::ui {

DragInfo::DragInfo(const wmDrag &drag, const wmEvent &event, const DropLocation drop_location)
    : drag_data(drag), event(event), drop_location(drop_location)
{
}

std::optional<DropLocation> DropTargetInterface::choose_drop_location(
    const ARegion & /*region*/, const wmEvent & /*event*/) const
{
  return DropLocation::Into;
}

bool drop_target_apply_drop(bContext &C,
                            const ARegion &region,
                            const wmEvent &event,
                            const DropTargetInterface &drop_target,
                            const ListBase &drags)
{
  const char *disabled_hint_dummy = nullptr;
  LISTBASE_FOREACH (const wmDrag *, drag, &drags) {
    if (!drop_target.can_drop(*drag, &disabled_hint_dummy)) {
      return false;
    }

    const std::optional<DropLocation> drop_location = drop_target.choose_drop_location(region,
                                                                                       event);
    if (!drop_location) {
      return false;
    }

    const DragInfo drag_info{*drag, event, *drop_location};
    return drop_target.on_drop(&C, drag_info);
  }

  return false;
}

std::string drop_target_tooltip_and_linehint(ARegion &region,
                                             const DropTargetInterface &drop_target,
                                             const wmDrag &drag,
                                             const wmEvent &event)
{
  const char *disabled_hint_dummy = nullptr;
  if (!drop_target.can_drop(drag, &disabled_hint_dummy)) {
    return {};
  }

  const std::optional<DropLocation> drop_location = drop_target.choose_drop_location(region,
                                                                                     event);
  if (!drop_location) {
    return {};
  }

  const DragInfo drag_info{drag, event, *drop_location};

  /* Draw drop line hint. */
  drop_target.drop_linehint(region, drag_info);

  /* Get drop tooltip. */
  return drop_target.drop_tooltip(drag_info);
}

}  // namespace blender::ui
