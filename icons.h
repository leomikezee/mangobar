#pragma once

#include <stdbool.h>
#include <pixman.h>

// The caller owns the returned image. extra is an optional icon-theme directory.
pixman_image_t *icon_load(const char *name, const char *extra, int size);
void icons_finish(void);
