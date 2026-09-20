#pragma once

/* ui.c — in-OS screens (Step 14). `ui <n>` on the shell switches:
 *   0 SHELL    status + thread/memory summary
 *   1 OBJECTS  object grid, colored by type
 *   2 LINEAGE  ancestor graph of the newest object
 *   3 HEATMAP  byte heatmap of the newest array/chunk                */
void ui_render(int screen);
int  ui_screen(void);
