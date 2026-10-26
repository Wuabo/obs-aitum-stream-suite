#pragma once

/* When Main Canvas is dockified it becomes narrow; OBSBasic then sets
 * ContextBarSize_Minimized and clears typed source toolbars from emptySpace.
 * Re-inject the same toolbars so Window/Media/Audio controls keep working. */
void install_main_canvas_context_bar_fix();
void shutdown_main_canvas_context_bar_fix();
