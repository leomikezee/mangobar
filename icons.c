#define _GNU_SOURCE
#include "icons.h"

#include <dirent.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
  char **basedirs;
  int basedir_count;
  char *icon_theme;
} IconTheme;

static IconTheme theme;
static bool initialized;

static char *fmt_str(const char *fmt, ...) {
  char *result = NULL;
  va_list ap;
  va_start(ap, fmt);
  if (vasprintf(&result, fmt, ap) < 0)
    result = NULL;
  va_end(ap);
  return result;
}

// ---------- GTK icon theme ----------
// Read gtk-icon-theme-name from a GTK settings.ini file.
static bool read_settings_ini_value(const char *path, char *out,
                                    size_t outsz) {
  FILE *f = fopen(path, "r");
  if (!f)
    return false;
  bool found = false;
  char line[1024];
  while (fgets(line, sizeof(line), f)) {
    char *p = line;
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p == '\0' || *p == '#' || *p == ';' || *p == '\n')
      continue; // empty line or comment
    char *eq = strchr(p, '=');
    if (!eq)
      continue;
    char *key_end = eq;
    while (key_end > p && (key_end[-1] == ' ' || key_end[-1] == '\t'))
      key_end--;
    static const char key[] = "gtk-icon-theme-name";
    if ((size_t)(key_end - p) != sizeof(key) - 1 ||
        memcmp(p, key, sizeof(key) - 1) != 0)
      continue;
    char *v = eq + 1;
    while (*v == ' ' || *v == '\t')
      v++;
    char *end = v + strlen(v);
    while (end > v && (end[-1] == '\n' || end[-1] == '\r' ||
                       end[-1] == ' ' || end[-1] == '\t'))
      end--;
    *end = '\0';
    // Strip optional surrounding quotes
    if (*v == '"' || *v == '\'') {
      char quote = *v;
      size_t l = strlen(v);
      if (l >= 2 && v[l - 1] == quote) {
        v++;
        v[l - 2] = '\0';
      }
    }
    size_t len = strlen(v);
    if (len > 0) {
      if (len >= outsz)
        len = outsz - 1;
      memcpy(out, v, len);
      out[len] = '\0';
      found = true;
      break;
    }
  }
  fclose(f);
  return found;
}

// Try $XDG_CONFIG_HOME/gtk-X.0/settings.ini, then /etc/gtk-X.0,
// with GTK3 before GTK4.
static char *gtk_icon_theme_name(void) {
  char cfgdir[1600];
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg && xdg[0] == '/') {
    snprintf(cfgdir, sizeof(cfgdir), "%s", xdg);
  } else {
    const char *home = getenv("HOME");
    if (!home || !*home)
      return NULL;
    snprintf(cfgdir, sizeof(cfgdir), "%s/.config", home);
  }
  char value[256];
  static const char *versions[] = {"3.0", "4.0"};
  for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
    char path[1800];
    snprintf(path, sizeof(path), "%s/gtk-%s/settings.ini", cfgdir,
             versions[i]);
    if (read_settings_ini_value(path, value, sizeof(value)))
      return strdup(value);
    snprintf(path, sizeof(path), "/etc/gtk-%s/settings.ini", versions[i]);
    if (read_settings_ini_value(path, value, sizeof(value)))
      return strdup(value);
  }
  return NULL;
}

static void image_free_data(pixman_image_t *img, void *data) {
  free(data);
}

static pixman_image_t *pixbuf_to_pixman(GdkPixbuf *pb) {
  int w = gdk_pixbuf_get_width(pb);
  int h = gdk_pixbuf_get_height(pb);
  GdkPixbuf *rgba = pb;
  bool need_unref = false;
  if (gdk_pixbuf_get_n_channels(pb) != 4 ||
      gdk_pixbuf_get_bits_per_sample(pb) != 8) {
    rgba = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, w, h);
    gdk_pixbuf_composite(pb, rgba, 0, 0, w, h, 0, 0, 1.0, 1.0,
                         GDK_INTERP_BILINEAR, 255);
    need_unref = true;
  } else if (!gdk_pixbuf_get_has_alpha(pb)) {
    rgba = gdk_pixbuf_add_alpha(pb, FALSE, 0, 0, 0);
    need_unref = true;
  }
  const guchar *px = gdk_pixbuf_get_pixels(rgba);
  int rowstride = gdk_pixbuf_get_rowstride(rgba);
  uint32_t *buf = malloc(sizeof(uint32_t) * (size_t)w * h);
  if (buf) {
    for (int y = 0; y < h; y++) {
      const guchar *row = px + y * rowstride;
      for (int x = 0; x < w; x++) {
        int r = row[x * 4 + 0], g = row[x * 4 + 1], b = row[x * 4 + 2],
            a = row[x * 4 + 3];
        buf[y * w + x] = ((uint32_t)a << 24) | ((uint32_t)(r * a / 255) << 16) |
                         ((uint32_t)(g * a / 255) << 8) | (uint32_t)(b * a / 255);
      }
    }
  }
  if (need_unref)
    g_object_unref(rgba);
  pixman_image_t *img =
      buf ? pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, buf, w * 4) : NULL;
  if (img)
    pixman_image_set_destroy_function(img, image_free_data, buf);
  else
    free(buf);
  return img;
}

static pixman_image_t *load_icon_from_file(const char *path, int size) {
  GError *err = NULL;
  GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path, size, size, TRUE, &err);
  if (!pb) {
    if (err)
      g_error_free(err);
    return NULL;
  }
  pixman_image_t *img = pixbuf_to_pixman(pb);
  g_object_unref(pb);
  return img;
}

// ---------- Icon lookup (simplified theme search) ----------
#define MAX_ICON_CANDIDATES 32

typedef struct {
  char path[1600];
  int score; // higher is better: png + size proximity
} IconCandidate;

// Max entries scanned per lookup
#define MAX_ICON_LOOKUP_ENTRIES 200000

// Cache misses as well as resolved paths.
#define ICON_CACHE_MAX 64
typedef struct {
  char *name;
  char *extra;
  int size;
  char path[1600];
  bool found;
} IconCacheEntry;
static IconCacheEntry icon_cache[ICON_CACHE_MAX];
static int icon_cache_count;

static IconCacheEntry *cache_entry(const char *name, const char *extra, int size) {
  if (icon_cache_count == ICON_CACHE_MAX)
    return NULL;
  IconCacheEntry *entry = &icon_cache[icon_cache_count++];
  entry->name = strdup(name);
  entry->extra = strdup(extra ? extra : "");
  entry->size = size;
  return entry;
}

// Build exact candidate names: original + -symbolic variant
static void icon_name_variants(const char *name,
                               char variants[][128], int *n) {
  *n = 0;
  snprintf(variants[(*n)++], 128, "%s", name);
  size_t l = strlen(name);
  if (l >= 9 && strcmp(name + l - 9, "-symbolic") == 0)
    snprintf(variants[(*n)++], 128, "%.*s", (int)(l - 9), name);
  else
    snprintf(variants[(*n)++], 128, "%s-symbolic", name);
}

static void find_icon_in_dir(const char *base, const char *name, int target,
                             int depth, IconCandidate *cands, int *count,
                             int *budget, int base_score,
                             const char variants[][128], int nvariants) {
  if (*budget <= 0)
    return;
  DIR *d = opendir(base);
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d)) && *budget > 0) {
    (*budget)--;
    if (e->d_name[0] == '.')
      continue;
    if (*count >= MAX_ICON_CANDIDATES)
      break;
    char full[1600];
    snprintf(full, sizeof(full), "%.*s/%s", (int)(sizeof(full) - 300), base,
             e->d_name);
    bool is_dir;
    if (e->d_type == DT_DIR)
      is_dir = true;
    else if (e->d_type == DT_REG)
      is_dir = false;
    else {
      struct stat st;
      is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
    }
    if (is_dir) {
      if (depth < 4)
        find_icon_in_dir(full, name, target, depth + 1, cands, count, budget,
                         base_score, variants, nvariants);
    } else {
      size_t dl = strlen(e->d_name);
      bool is_png = dl >= 4 && strcmp(e->d_name + dl - 4, ".png") == 0;
      bool is_svg = dl >= 4 && strcmp(e->d_name + dl - 4, ".svg") == 0;
      if (is_png || is_svg) {
        if (dl < 5)
          continue;
        char stem[128];
        size_t sl = dl - 4;
        if (sl >= sizeof(stem))
          continue;
        memcpy(stem, e->d_name, sl);
        stem[sl] = '\0';
        bool match = false;
        for (int v = 0; v < nvariants; v++)
          if (strcmp(stem, variants[v]) == 0) {
            match = true;
            break;
          }
        if (!match)
          continue;
        int score = base_score;
        if (is_png)
          score += 500; // prefer png at equal size
        int best_err = INT_MAX;
        for (const char *q = full; *q; q++) {
          if (*q >= '0' && *q <= '9') {
            int n = 0;
            while (*q >= '0' && *q <= '9') {
              if (n <= 512)
                n = n * 10 + (*q - '0');
              q++;
            }
            if (n > 0 && n <= 512) {
              int err = abs(n - target);
              if (err < best_err)
                best_err = err;
            }
          }
        }
        if (best_err != INT_MAX) {
          int bonus = 1000 - (best_err > 1000 ? 1000 : best_err);
          if (bonus > 0)
            score += bonus;
        }
        // Penalize symbolic icons
        if (strstr(stem, "-symbolic"))
          score -= 200;
        IconCandidate *c = &cands[(*count)++];
        snprintf(c->path, sizeof(c->path), "%s", full);
        c->score = score;
      }
    }
  }
  closedir(d);
}

// Whether this top-level theme dir was already searched by an earlier,
// higher-priority stage.
static bool theme_already_searched(const char *dir_name, const char *theme) {
  if (strcmp(dir_name, "hicolor") == 0 || strcmp(dir_name, "Adwaita") == 0)
    return true;
  return theme && strcmp(dir_name, theme) == 0;
}

// Search the icon theme directory with the given name under every basedir.
static void scan_named_theme(IconTheme *lookup, const char *dir_name,
                             const char *name, int target,
                             IconCandidate *cands, int *count, int *budget,
                             int pri, const char variants[][128],
                             int nvariants) {
  for (int i = 0; i < lookup->basedir_count && *count < MAX_ICON_CANDIDATES &&
                     *budget > 0;
       i++) {
    DIR *rd = opendir(lookup->basedirs[i]);
    if (!rd)
      continue;
    struct dirent *e;
    while ((e = readdir(rd)) && *count < MAX_ICON_CANDIDATES && *budget > 0) {
      (*budget)--;
      if (e->d_name[0] == '.' || strcmp(e->d_name, dir_name) != 0)
        continue;
      char p[1600];
      snprintf(p, sizeof(p), "%.*s/%s", (int)(sizeof(p) - 300),
               lookup->basedirs[i], e->d_name);
      struct stat st;
      if (stat(p, &st) == 0 && S_ISDIR(st.st_mode))
        find_icon_in_dir(p, name, target, 0, cands, count, budget, pri,
                         variants, nvariants);
      break; // a theme directory appears at most once per basedir
    }
    closedir(rd);
  }
}

static char *find_icon(IconTheme *lookup, const char *name,
                       const char *extra, int target) {
  if (!name || !*name)
    return NULL;
  // Return cached result
  for (int i = 0; i < icon_cache_count; i++) {
    if (strcmp(icon_cache[i].name, name) == 0 &&
        strcmp(icon_cache[i].extra, extra ? extra : "") == 0 &&
        icon_cache[i].size == target)
      return icon_cache[i].found ? strdup(icon_cache[i].path) : NULL;
  }
  // IconName may be an absolute path
  if (name[0] == '/') {
    struct stat st;
    if (stat(name, &st) == 0 && S_ISREG(st.st_mode)) {
      if (icon_cache_count < ICON_CACHE_MAX) {
        IconCacheEntry *c = cache_entry(name, extra, target);
        snprintf(c->path, sizeof(c->path), "%s", name);
        c->found = true;
      }
      return strdup(name);
    }
    return NULL;
  }
  char variants[2][128];
  int nvariants;
  icon_name_variants(name, variants, &nvariants);
  IconCandidate cands[MAX_ICON_CANDIDATES];
  int count = 0;
  int budget = MAX_ICON_LOOKUP_ENTRIES;
  const char *theme = lookup->icon_theme;
  // App-provided IconThemePath overrides the configured theme.
  if (extra && *extra && count < MAX_ICON_CANDIDATES)
    find_icon_in_dir(extra, name, target, 0, cands, &count, &budget, 50000,
                     variants, nvariants);
  // Search preferred themes first, and only continue when none has the icon.
  if (count == 0 && theme && *theme)
    scan_named_theme(lookup, theme, name, target, cands, &count, &budget, 40000,
                     variants, nvariants);
  if (count == 0 && (!theme || strcmp(theme, "hicolor") != 0))
    scan_named_theme(lookup, "hicolor", name, target, cands, &count, &budget,
                     30000, variants, nvariants);
  if (count == 0 && (!theme || strcmp(theme, "Adwaita") != 0))
    scan_named_theme(lookup, "Adwaita", name, target, cands, &count, &budget,
                     20000, variants, nvariants);
  if (count == 0) {
    // Search remaining themes and loose icon files.
    for (int i = 0; i < lookup->basedir_count && count < MAX_ICON_CANDIDATES;
         i++) {
      DIR *rd = opendir(lookup->basedirs[i]);
      if (!rd) {
        find_icon_in_dir(lookup->basedirs[i], name, target, 0, cands, &count,
                         &budget, 10000, variants, nvariants);
        continue;
      }
      struct dirent *e;
      while ((e = readdir(rd)) && count < MAX_ICON_CANDIDATES && budget > 0) {
        budget--;
        if (e->d_name[0] == '.')
          continue;
        char p[1600];
        snprintf(p, sizeof(p), "%.*s/%s", (int)(sizeof(p) - 300),
                 lookup->basedirs[i], e->d_name);
        struct stat st;
        if (stat(p, &st) != 0)
          continue;
        if (S_ISDIR(st.st_mode)) {
          if (theme_already_searched(e->d_name, theme))
            continue;
          find_icon_in_dir(p, name, target, 0, cands, &count, &budget, 10000,
                           variants, nvariants);
        } else {
          // Loose files at the basedir root (e.g. /usr/share/pixmaps)
          size_t dl = strlen(e->d_name);
          bool is_png = dl >= 4 && strcmp(e->d_name + dl - 4, ".png") == 0;
          bool is_svg = dl >= 4 && strcmp(e->d_name + dl - 4, ".svg") == 0;
          if (!is_png && !is_svg)
            continue;
          if (dl < 5)
            continue;
          char stem[128];
          size_t sl = dl - 4;
          if (sl >= sizeof(stem))
            continue;
          memcpy(stem, e->d_name, sl);
          stem[sl] = '\0';
          bool match = false;
          for (int v = 0; v < nvariants; v++)
            if (strcmp(stem, variants[v]) == 0) {
              match = true;
              break;
            }
          if (match && count < MAX_ICON_CANDIDATES) {
            IconCandidate *c = &cands[count++];
            snprintf(c->path, sizeof(c->path), "%s", p);
            c->score = 10000;
          }
        }
      }
      closedir(rd);
    }
  }
  if (count == 0) {
    // Cache misses too
    if (icon_cache_count < ICON_CACHE_MAX) {
      IconCacheEntry *c = cache_entry(name, extra, target);
      c->path[0] = '\0';
      c->found = false;
    }
    return NULL;
  }
  int best = 0;
  for (int i = 1; i < count; i++)
    if (cands[i].score > cands[best].score)
      best = i;
  if (icon_cache_count < ICON_CACHE_MAX) {
    IconCacheEntry *c = cache_entry(name, extra, target);
    size_t len = strlen(cands[best].path);
    if (len >= sizeof(c->path))
      len = sizeof(c->path) - 1;
    memcpy(c->path, cands[best].path, len);
    c->path[len] = '\0';
    c->found = true;
  }
  return strdup(cands[best].path);
}

static void add_basedir(IconTheme *lookup, char *path) {
  struct stat st;
  if (!path)
    return;
  if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
    free(path);
    return;
  }
  for (int i = 0; i < lookup->basedir_count; i++) {
    if (strcmp(lookup->basedirs[i], path) == 0) {
      free(path);
      return;
    }
  }
  char **dirs = realloc(lookup->basedirs,
                        sizeof(char *) * (lookup->basedir_count + 1));
  if (!dirs) {
    free(path);
    return;
  }
  lookup->basedirs = dirs;
  lookup->basedirs[lookup->basedir_count++] = path;
}

static void init_basedirs(IconTheme *lookup) {
  const char *home = getenv("HOME");
  const char *xdg_data = getenv("XDG_DATA_HOME");
  const char *xdg_dirs = getenv("XDG_DATA_DIRS");
  if (home && *home)
    add_basedir(lookup, fmt_str("%s/.icons", home));
  if (xdg_data && *xdg_data)
    add_basedir(lookup, fmt_str("%s/icons", xdg_data));
  else if (home && *home)
    add_basedir(lookup, fmt_str("%s/.local/share/icons", home));
  if (!(xdg_dirs && *xdg_dirs))
    xdg_dirs = "/usr/local/share:/usr/share";
  char *copy = strdup(xdg_dirs);
  char *save;
  for (char *dir = strtok_r(copy, ":", &save); dir;
       dir = strtok_r(NULL, ":", &save)) {
    if (dir[0] != '/')
      continue;
    add_basedir(lookup, fmt_str("%s/icons", dir));
    add_basedir(lookup, fmt_str("%s/pixmaps", dir));
  }
  free(copy);
}

pixman_image_t *icon_load(const char *name, const char *extra, int size) {
  if (!name || !*name || size <= 0)
    return NULL;
  if (!initialized) {
    init_basedirs(&theme);
    theme.icon_theme = gtk_icon_theme_name();
    initialized = true;
  }
  char *path = find_icon(&theme, name, extra, size);
  pixman_image_t *image = path ? load_icon_from_file(path, size) : NULL;
  free(path);
  return image;
}

void icons_finish(void) {
  for (int i = 0; i < theme.basedir_count; i++)
    free(theme.basedirs[i]);
  free(theme.basedirs);
  free(theme.icon_theme);
  memset(&theme, 0, sizeof(theme));
  initialized = false;
  for (int i = 0; i < icon_cache_count; i++) {
    free(icon_cache[i].name);
    free(icon_cache[i].extra);
  }
  icon_cache_count = 0;
}
