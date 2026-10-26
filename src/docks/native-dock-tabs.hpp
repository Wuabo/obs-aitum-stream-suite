#pragma once

/* Inject canvas tabs into OBS's own Scenes and Sources docks.
 * Does not replace those docks — only inserts a tab bar and extra pages. */

void refresh_native_canvas_tabs();
void detach_native_canvas_tabs();
void shutdown_native_canvas_tabs();
/* Canvas docks are created after OBS restoreState. Re-apply the saved
 * BasicWindow DockState so bottom strip sizes stick. */
void schedule_restore_obs_dock_layout();
