#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <pixman.h>

#define WORKSPACE_MAX_ICONS 16

typedef struct {
  pixman_image_t *images[WORKSPACE_MAX_ICONS]; // borrowed from the app icon cache
  int count;
  int size;
  int spacing;
} WorkspaceIcons;

// Nonblocking get all-clients requests on Mango's IPC socket.
bool workspace_apps_refresh(uint64_t now);
int workspace_apps_fd(void);
bool workspace_apps_dispatch(void);
WorkspaceIcons workspace_apps_icons(const char *monitor, int tag, int size,
                                    int scale, int spacing, int limit);
void workspace_apps_finish(void);
