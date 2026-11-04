#define _GNU_SOURCE
#include "tray.h"
#include "icons.h"
#include "sd-bus.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-util.h>

#define WATCHER_PATH "/StatusNotifierWatcher"
#define DEFAULT_ICON_SIZE 64

typedef struct {
  int size;
  uint32_t pixels[]; // host-order ARGB (unpremultiplied)
} TrayPixmap;

struct MangobarTrayItem {
  char *watcher_id;
  char *service;
  char *path;
  const char *interface;
  char *status;
  char *icon_name;
  char *attention_icon_name;
  char *menu;
  char *icon_theme_path;
  TrayPixmap **pixmaps;
  int pixmap_count;
  TrayPixmap **attention_pixmaps;
  int attention_pixmap_count;
  bool item_is_menu;
  pixman_image_t *icon;
  int icon_size;
  unsigned icon_rev;
  sd_bus_slot **slots;
  int slot_count;
  struct wl_list pending_props;
  struct MangobarTray *tray;
};

typedef struct {
  struct wl_list link;
  MangobarTrayItem *sni;
  const char *prop;
  const char *type; // NULL = icon pixmap
  void *dest;
  sd_bus_slot *slot;
} PropSlot;

typedef struct {
  char *interface;
  sd_bus_slot *vtable_slot;
  sd_bus_slot *signal_slot;
  char **items;
  int item_count;
  char **hosts;
  int host_count;
  bool owns_name;
  struct MangobarTray *tray;
} TrayWatcher;

typedef struct {
  char *interface;
  char *service;
  sd_bus_slot *reg_slot;
  sd_bus_slot *unreg_slot;
  sd_bus_slot *watcher_slot;
  struct MangobarTray *tray;
} TrayHost;

struct MangobarTray {
  sd_bus *bus;
  int fd;
  TrayWatcher *watcher_xdg;
  TrayWatcher *watcher_kde;
  TrayHost *host_xdg;
  TrayHost *host_kde;
  MangobarTrayItem **items;
  int item_count;
  void (*set_dirty)(void);
  sd_bus_slot *name_slot; // NameOwnerChanged subscription for cleanup
  bool dead;
};

// ---------- Utilities ----------
static char *fmt_str(const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return strdup(buf);
}

static void set_tray_dirty(MangobarTray *tray) {
  if (tray && tray->set_dirty)
    tray->set_dirty();
}

static int find_index(char **arr, int n, const char *id) {
  for (int i = 0; i < n; i++)
    if (arr[i] && strcmp(arr[i], id) == 0)
      return i;
  return -1;
}

// ---------- Icon conversion ----------
static void image_free_data(pixman_image_t *img, void *data) {
  free(data);
}

static pixman_image_t *pixmap_to_pixman(const TrayPixmap *pm) {
  int s = pm->size;
  uint32_t *buf = malloc(sizeof(uint32_t) * (size_t)s * s);
  if (!buf)
    return NULL;
  for (int i = 0; i < s * s; i++) {
    uint32_t p = pm->pixels[i];
    uint32_t a = (p >> 24) & 0xff;
    uint32_t r = ((p >> 16) & 0xff) * a / 255;
    uint32_t g = ((p >> 8) & 0xff) * a / 255;
    uint32_t b = (p & 0xff) * a / 255;
    buf[i] = (a << 24) | (r << 16) | (g << 8) | b;
  }
  pixman_image_t *img =
      pixman_image_create_bits(PIXMAN_a8r8g8b8, s, s, buf, s * 4);
  if (img)
    pixman_image_set_destroy_function(img, image_free_data, buf);
  else
    free(buf);
  return img;
}

static pixman_image_t *make_placeholder(int size) {
  pixman_image_t *img =
      pixman_image_create_bits(PIXMAN_a8r8g8b8, size, size, NULL, size * 4);
  if (!img)
    return NULL;
  pixman_image_t *solid = pixman_image_create_solid_fill(
      &(pixman_color_t){0x6666, 0x6666, 0x6666, 0xFFFF});
  pixman_image_composite32(PIXMAN_OP_OVER, solid, NULL, img, 0, 0, 0, 0, 0, 0,
                           size, size);
  pixman_image_unref(solid);
  return img;
}

// ---------- Items ----------
static void pixmaps_clear(TrayPixmap ***arr, int *count) {
  if (!*arr)
    return;
  for (int i = 0; i < *count; i++)
    free((*arr)[i]);
  free(*arr);
  *arr = NULL;
  *count = 0;
}

static void destroy_sni(MangobarTrayItem *sni) {
  if (!sni)
    return;
  if (sni->icon)
    pixman_image_unref(sni->icon);
  free(sni->watcher_id);
  free(sni->service);
  free(sni->path);
  free(sni->status);
  free(sni->icon_name);
  free(sni->attention_icon_name);
  free(sni->menu);
  free(sni->icon_theme_path);
  pixmaps_clear(&sni->pixmaps, &sni->pixmap_count);
  pixmaps_clear(&sni->attention_pixmaps, &sni->attention_pixmap_count);
  for (int i = 0; i < sni->slot_count; i++)
    sd_bus_slot_unref(sni->slots[i]);
  free(sni->slots);
  PropSlot *d, *tmp;
  wl_list_for_each_safe(d, tmp, &sni->pending_props, link) {
    sd_bus_slot_unref(d->slot);
    free(d);
  }
  free(sni);
}

static bool sni_ready(MangobarTrayItem *sni) {
  if (!sni->status)
    return false;
  if (sni->status[0] == 'N')
    return sni->attention_icon_name || sni->attention_pixmap_count > 0;
  return sni->icon_name || sni->pixmap_count > 0;
}

static void reload_icon(MangobarTrayItem *sni) {
  pixman_image_t *old = sni->icon;
  sni->icon = NULL;
  sni->icon_size = 0;
  sni->icon_rev++;
  bool attention = sni->status && sni->status[0] == 'N';
  const char *icon_name = attention ? sni->attention_icon_name : sni->icon_name;
  TrayPixmap **pixmaps = attention ? sni->attention_pixmaps : sni->pixmaps;
  int pixmap_count = attention ? sni->attention_pixmap_count
                               : sni->pixmap_count;
  if (pixmap_count > 0) {
    TrayPixmap *best = pixmaps[0];
    // Pick the largest pixmap so downscaling stays sharp.
    for (int i = 1; i < pixmap_count; i++)
      if (pixmaps[i]->size > best->size)
        best = pixmaps[i];
    sni->icon = pixmap_to_pixman(best);
    sni->icon_size = best->size;
  } else if (icon_name && *icon_name) {
    sni->icon = icon_load(icon_name, sni->icon_theme_path, DEFAULT_ICON_SIZE);
    sni->icon_size = sni->icon ? pixman_image_get_width(sni->icon) : 0;
  }
  if (!sni->icon) {
    sni->icon = make_placeholder(DEFAULT_ICON_SIZE);
    sni->icon_size = DEFAULT_ICON_SIZE;
  }
  if (old)
    pixman_image_unref(old);
}

static void sni_dirty(MangobarTrayItem *sni) {
  if (sni_ready(sni)) {
    reload_icon(sni);
    set_tray_dirty(sni->tray);
  }
}

// ---------- Async property reads ----------
static int read_pixmap(sd_bus_message *msg, MangobarTrayItem *sni,
                       bool attention) {
  int ret = sd_bus_message_enter_container(msg, 'a', "(iiay)");
  if (ret < 0)
    return ret;
  if (sd_bus_message_at_end(msg, 0))
    return 0;
  TrayPixmap **pixmaps = NULL;
  int n = 0;
  while (!sd_bus_message_at_end(msg, 0)) {
    ret = sd_bus_message_enter_container(msg, 'r', "iiay");
    if (ret < 0)
      goto err;
    int width, height;
    ret = sd_bus_message_read(msg, "ii", &width, &height);
    if (ret < 0) {
      sd_bus_message_exit_container(msg);
      goto err;
    }
    const void *pixels;
    size_t size;
    ret = sd_bus_message_read_array(msg, 'y', &pixels, &size);
    if (ret < 0) {
      sd_bus_message_exit_container(msg);
      goto err;
    }
    sd_bus_message_exit_container(msg);
    if (height > 0 && width == height && (size_t)width * height <= size / 4) {
      TrayPixmap *pm = malloc(sizeof(TrayPixmap) +
                              sizeof(uint32_t) * (size_t)width * height);
      if (!pm)
        continue;
      pm->size = height;
      const uint32_t *src = pixels;
      for (int i = 0; i < width * height; i++)
        pm->pixels[i] = ntohl(src[i]);
      pixmaps = realloc(pixmaps, sizeof(*pixmaps) * (n + 1));
      if (!pixmaps)
        break;
      pixmaps[n++] = pm;
    }
  }
  if (attention) {
    pixmaps_clear(&sni->attention_pixmaps, &sni->attention_pixmap_count);
    sni->attention_pixmaps = pixmaps;
    sni->attention_pixmap_count = n;
  } else {
    pixmaps_clear(&sni->pixmaps, &sni->pixmap_count);
    sni->pixmaps = pixmaps;
    sni->pixmap_count = n;
  }
  return 0;
err:
  for (int i = 0; i < n; i++)
    free(pixmaps[i]);
  free(pixmaps);
  return ret;
}

static int get_property_callback(sd_bus_message *msg, void *data,
                                 sd_bus_error *error) {
  PropSlot *d = data;
  MangobarTrayItem *sni = d->sni;
  const char *prop = d->prop;
  const char *type = d->type;
  void *dest = d->dest;
  int ret;
  if (sd_bus_message_is_method_error(msg, NULL)) {
    ret = sd_bus_message_get_errno(msg);
    goto cleanup;
  }
  ret = sd_bus_message_enter_container(msg, 'v', type);
  if (ret < 0)
    goto cleanup;
  if (!type) {
    bool attention = strncmp(prop, "Attention", 9) == 0;
    ret = read_pixmap(msg, sni, attention);
    if (ret < 0)
      goto cleanup;
  } else {
    if ((*type == 's' || *type == 'o') && *(char **)dest)
      free(*(char **)dest);
    ret = sd_bus_message_read(msg, type, dest);
    if (ret < 0)
      goto cleanup;
    if (*type == 's' || *type == 'o')
      *(char **)dest = strdup(*(char **)dest);
  }
  if (strcmp(prop, "Status") == 0 ||
      (sni->status && (sni->status[0] == 'N' ? prop[0] == 'A'
                                             : strncmp(prop, "Icon", 4) == 0)))
    sni_dirty(sni);
cleanup:
  wl_list_remove(&d->link);
  sd_bus_slot_unref(d->slot);
  free(d);
  return ret;
}

static void sni_get_property_async(MangobarTrayItem *sni, const char *prop,
                                   const char *type, void *dest) {
  PropSlot *data = calloc(1, sizeof(*data));
  data->sni = sni;
  data->prop = prop;
  data->type = type;
  data->dest = dest;
  int ret = sd_bus_call_method_async(
      sni->tray->bus, &data->slot, sni->service, sni->path,
      "org.freedesktop.DBus.Properties", "Get", get_property_callback, data,
      "ss", sni->interface, prop);
  if (ret < 0)
    free(data);
  else
    wl_list_insert(&sni->pending_props, &data->link);
}

// ---------- Item signals ----------
static int handle_new_icon(sd_bus_message *msg, void *data,
                           sd_bus_error *error) {
  MangobarTrayItem *sni = data;
  sni_get_property_async(sni, "IconName", "s", &sni->icon_name);
  sni_get_property_async(sni, "IconPixmap", NULL, NULL);
  return 0;
}

static int handle_new_attention_icon(sd_bus_message *msg, void *data,
                                     sd_bus_error *error) {
  MangobarTrayItem *sni = data;
  sni_get_property_async(sni, "AttentionIconName", "s",
                         &sni->attention_icon_name);
  sni_get_property_async(sni, "AttentionIconPixmap", NULL, NULL);
  return 0;
}

static int handle_new_status(sd_bus_message *msg, void *data,
                             sd_bus_error *error) {
  MangobarTrayItem *sni = data;
  char *status;
  int r = sd_bus_message_read(msg, "s", &status);
  if (r < 0)
    return r;
  free(sni->status);
  sni->status = strdup(status);
  sni_dirty(sni);
  return 0;
}

static void sni_match_signal_async(MangobarTrayItem *sni, const char *signal,
                                   sd_bus_message_handler_t callback) {
  sd_bus_slot *slot = NULL;
  int ret = sd_bus_match_signal_async(sni->tray->bus, &slot, sni->service,
                                      sni->path, sni->interface, signal,
                                      callback, NULL, sni);
  if (ret >= 0) {
    sni->slots = realloc(sni->slots, sizeof(*sni->slots) * (sni->slot_count + 1));
    sni->slots[sni->slot_count++] = slot;
  }
}

static MangobarTrayItem *create_sni(char *id, MangobarTray *tray) {
  MangobarTrayItem *sni = calloc(1, sizeof(*sni));
  if (!sni)
    return NULL;
  wl_list_init(&sni->pending_props);
  sni->tray = tray;
  sni->watcher_id = strdup(id);
  char *path_ptr = strchr(id, '/');
  if (!path_ptr) {
    sni->service = strdup(id);
    sni->path = strdup("/StatusNotifierItem");
    // SNIs may register only a service name (no path); use the org.kde interface
    sni->interface = "org.kde.StatusNotifierItem";
  } else {
    sni->service = strndup(id, (size_t)(path_ptr - id));
    sni->path = strdup(path_ptr);
    sni->interface = "org.kde.StatusNotifierItem";
    sni_get_property_async(sni, "IconThemePath", "s", &sni->icon_theme_path);
  }
  sni_get_property_async(sni, "Status", "s", &sni->status);
  sni_get_property_async(sni, "IconName", "s", &sni->icon_name);
  sni_get_property_async(sni, "IconPixmap", NULL, NULL);
  sni_get_property_async(sni, "AttentionIconName", "s",
                         &sni->attention_icon_name);
  sni_get_property_async(sni, "AttentionIconPixmap", NULL, NULL);
  sni_get_property_async(sni, "ItemIsMenu", "b", &sni->item_is_menu);
  sni_get_property_async(sni, "Menu", "o", &sni->menu);

  sni_match_signal_async(sni, "NewIcon", handle_new_icon);
  sni_match_signal_async(sni, "NewAttentionIcon", handle_new_attention_icon);
  sni_match_signal_async(sni, "NewStatus", handle_new_status);
  return sni;
}

static void add_sni(MangobarTray *tray, char *id) {
  for (int i = 0; i < tray->item_count; i++)
    if (strcmp(tray->items[i]->watcher_id, id) == 0)
      return;
  MangobarTrayItem *sni = create_sni(id, tray);
  if (sni) {
    tray->items = realloc(tray->items,
                          sizeof(*tray->items) * (tray->item_count + 1));
    tray->items[tray->item_count++] = sni;
    set_tray_dirty(tray);
  }
}

static void remove_sni(MangobarTray *tray, const char *id) {
  for (int i = 0; i < tray->item_count; i++) {
    if (strcmp(tray->items[i]->watcher_id, id) == 0) {
      destroy_sni(tray->items[i]);
      memmove(&tray->items[i], &tray->items[i + 1],
              sizeof(*tray->items) * (tray->item_count - i - 1));
      tray->item_count--;
      set_tray_dirty(tray);
      return;
    }
  }
}

// Remove stale items whose service (unique or well-known) lost its owner
static void remove_sni_by_service(MangobarTray *tray, const char *service) {
  for (int i = 0; i < tray->item_count; i++) {
    if (tray->items[i]->service &&
        strcmp(tray->items[i]->service, service) == 0) {
      destroy_sni(tray->items[i]);
      memmove(&tray->items[i], &tray->items[i + 1],
              sizeof(*tray->items) * (tray->item_count - i - 1));
      tray->item_count--;
      set_tray_dirty(tray);
      i--;
    }
  }
}

static int handle_name_owner_changed(sd_bus_message *msg, void *data,
                                     sd_bus_error *error) {
  char *name = NULL, *old_owner = NULL, *new_owner = NULL;
  int ret = sd_bus_message_read(msg, "sss", &name, &old_owner, &new_owner);
  if (ret < 0)
    return ret;
  if (!*new_owner && name)
    remove_sni_by_service((MangobarTray *)data, name);
  return 0;
}

// ---------- Host ----------
static int handle_sni_registered(sd_bus_message *msg, void *data,
                                 sd_bus_error *error) {
  char *id;
  int ret = sd_bus_message_read(msg, "s", &id);
  if (ret < 0)
    return ret;
  add_sni((MangobarTray *)data, id);
  return 0;
}

static int handle_sni_unregistered(sd_bus_message *msg, void *data,
                                   sd_bus_error *error) {
  char *id;
  int ret = sd_bus_message_read(msg, "s", &id);
  if (ret < 0)
    return ret;
  remove_sni((MangobarTray *)data, id);
  return 0;
}

static int get_registered_snis_callback(sd_bus_message *msg, void *data,
                                        sd_bus_error *error) {
  if (sd_bus_message_is_method_error(msg, NULL))
    return 0;
  MangobarTray *tray = data;
  int ret = sd_bus_message_enter_container(msg, 'v', NULL);
  if (ret < 0)
    return ret;
  char **ids;
  ret = sd_bus_message_read_strv(msg, &ids);
  if (ret < 0)
    return ret;
  if (ids) {
    for (char **id = ids; *id; ++id) {
      add_sni(tray, *id);
      free(*id);
    }
    free(ids);
  }
  return ret;
}

static void register_to_watcher(TrayHost *host) {
  if (!host->tray->dead) {
    sd_bus_call_method_async(
        host->tray->bus, NULL, host->interface, WATCHER_PATH,
        host->interface, "RegisterStatusNotifierHost", NULL, NULL, "s",
        host->service);
    sd_bus_call_method_async(host->tray->bus, NULL, host->interface,
                             WATCHER_PATH, "org.freedesktop.DBus.Properties",
                             "Get", get_registered_snis_callback,
                             host->tray, "ss", host->interface,
                             "RegisteredStatusNotifierItems");
  }
}

static int handle_new_watcher(sd_bus_message *msg, void *data,
                              sd_bus_error *error) {
  char *service, *old_owner, *new_owner;
  int ret = sd_bus_message_read(msg, "sss", &service, &old_owner, &new_owner);
  if (ret < 0)
    return ret;
  if (!*old_owner) {
    TrayHost *host = data;
    if (strcmp(service, host->interface) == 0)
      register_to_watcher(host);
  }
  return 0;
}

static void init_host(MangobarTray *tray, const char *protocol,
                      TrayHost **out) {
  TrayHost *host = calloc(1, sizeof(*host));
  if (!host)
    return;
  host->interface = fmt_str("org.%s.StatusNotifierWatcher", protocol);
  host->tray = tray;
  if (sd_bus_match_signal(tray->bus, &host->reg_slot, host->interface,
                          WATCHER_PATH, host->interface,
                          "StatusNotifierItemRegistered", handle_sni_registered,
                          tray) < 0)
    goto err;
  if (sd_bus_match_signal(tray->bus, &host->unreg_slot, host->interface,
                          WATCHER_PATH, host->interface,
                          "StatusNotifierItemUnregistered",
                          handle_sni_unregistered, tray) < 0)
    goto err;
  if (sd_bus_match_signal(tray->bus, &host->watcher_slot,
                          "org.freedesktop.DBus", "/org/freedesktop/DBus",
                          "org.freedesktop.DBus", "NameOwnerChanged",
                          handle_new_watcher, host) < 0)
    goto err;
  host->service =
      fmt_str("org.%s.StatusNotifierHost-%d", protocol, (int)getpid());
  sd_bus_request_name(tray->bus, host->service, 0);
  register_to_watcher(host);
  *out = host;
  return;
err:
  if (host->reg_slot)
    sd_bus_slot_unref(host->reg_slot);
  if (host->unreg_slot)
    sd_bus_slot_unref(host->unreg_slot);
  if (host->watcher_slot)
    sd_bus_slot_unref(host->watcher_slot);
  free(host->interface);
  free(host->service);
  free(host);
}

// ---------- Watcher (optionally owns the watcher name) ----------
static int watcher_register_sni(sd_bus_message *msg, void *data,
                                sd_bus_error *error) {
  TrayWatcher *watcher = data;
  char *service_or_path;
  int ret = sd_bus_message_read(msg, "s", &service_or_path);
  if (ret < 0)
    return ret;
  char *id;
  if (service_or_path[0] == '/') {
    const char *sender = sd_bus_message_get_sender(msg);
    id = fmt_str("%s%s", sender, service_or_path);
  } else {
    id = strdup(service_or_path);
  }
  if (find_index(watcher->items, watcher->item_count, id) == -1) {
    watcher->items =
        realloc(watcher->items, sizeof(char *) * (watcher->item_count + 1));
    watcher->items[watcher->item_count++] = id;
    // When we are the watcher, our own signals don't loop back; add directly
    add_sni(watcher->tray, id);
    sd_bus_emit_signal(watcher->tray->bus, WATCHER_PATH, watcher->interface,
                       "StatusNotifierItemRegistered", "s", id);
    sd_bus_emit_properties_changed(watcher->tray->bus, WATCHER_PATH,
                                   watcher->interface,
                                   "RegisteredStatusNotifierItems", NULL);
  } else {
    free(id);
  }
  return sd_bus_reply_method_return(msg, "");
}

static int watcher_register_host(sd_bus_message *msg, void *data,
                                 sd_bus_error *error) {
  TrayWatcher *watcher = data;
  char *service;
  int ret = sd_bus_message_read(msg, "s", &service);
  if (ret < 0)
    return ret;
  if (find_index(watcher->hosts, watcher->host_count, service) == -1) {
    watcher->hosts =
        realloc(watcher->hosts, sizeof(char *) * (watcher->host_count + 1));
    watcher->hosts[watcher->host_count++] = strdup(service);
  }
  return sd_bus_reply_method_return(msg, "");
}

static int watcher_get_registered_snis(sd_bus *bus, const char *obj_path,
                                       const char *interface,
                                       const char *property,
                                       sd_bus_message *reply, void *data,
                                       sd_bus_error *error) {
  TrayWatcher *watcher = data;
  watcher->items =
      realloc(watcher->items, sizeof(char *) * (watcher->item_count + 1));
  watcher->items[watcher->item_count] = NULL;
  int ret = sd_bus_message_append_strv(reply, watcher->items);
  free(watcher->items[watcher->item_count]);
  return ret;
}

static int watcher_is_host_registered(sd_bus *bus, const char *obj_path,
                                      const char *interface,
                                      const char *property,
                                      sd_bus_message *reply, void *data,
                                      sd_bus_error *error) {
  TrayWatcher *watcher = data;
  int val = watcher->host_count > 0;
  return sd_bus_message_append_basic(reply, 'b', &val);
}

static int watcher_get_protocol_version(sd_bus *bus, const char *obj_path,
                                        const char *interface,
                                        const char *property,
                                        sd_bus_message *reply, void *data,
                                        sd_bus_error *error) {
  int val = 0;
  return sd_bus_message_append_basic(reply, 'i', &val);
}

static int watcher_noop(sd_bus_message *msg, void *data, sd_bus_error *error) {
  return 0;
}

static const sd_bus_vtable watcher_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("RegisterStatusNotifierItem", "s", "", watcher_register_sni,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RegisterStatusNotifierHost", "s", "", watcher_register_host,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("RegisteredStatusNotifierItems", "as",
                    watcher_get_registered_snis, 0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("IsStatusNotifierHostRegistered", "b",
                    watcher_is_host_registered, 0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("ProtocolVersion", "i", watcher_get_protocol_version, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_SIGNAL("StatusNotifierItemRegistered", "s", 0),
    SD_BUS_SIGNAL("StatusNotifierItemUnregistered", "s", 0),
    SD_BUS_SIGNAL("StatusNotifierHostRegistered", NULL, 0),
    SD_BUS_VTABLE_END};

static TrayWatcher *create_watcher(MangobarTray *tray, const char *protocol) {
  TrayWatcher *w = calloc(1, sizeof(*w));
  if (!w)
    return NULL;
  w->interface = fmt_str("org.%s.StatusNotifierWatcher", protocol);
  w->tray = tray;
  if (sd_bus_add_object_vtable(tray->bus, &w->vtable_slot, WATCHER_PATH,
                               w->interface, watcher_vtable, w) < 0)
    goto err;
  if (sd_bus_match_signal(tray->bus, &w->signal_slot, "org.freedesktop.DBus",
                          "/org/freedesktop/DBus", "org.freedesktop.DBus",
                          "NameOwnerChanged", watcher_noop, w) < 0)
    goto err;
  if (sd_bus_request_name(tray->bus, w->interface, 0) < 0)
    goto err;
  w->owns_name = true;
  return w;
err:
  if (w->vtable_slot)
    sd_bus_slot_unref(w->vtable_slot);
  if (w->signal_slot)
    sd_bus_slot_unref(w->signal_slot);
  free(w->interface);
  free(w);
  return NULL;
}

// ---------- Public API ----------
MangobarTray *tray_init(void (*set_dirty)(void)) {
  sd_bus *bus;
  if (sd_bus_open_user(&bus) < 0)
    return NULL;
  MangobarTray *tray = calloc(1, sizeof(*tray));
  if (!tray) {
    sd_bus_unref(bus);
    return NULL;
  }
  tray->bus = bus;
  tray->fd = sd_bus_get_fd(bus);
  tray->set_dirty = set_dirty;
  tray->watcher_xdg = create_watcher(tray, "freedesktop");
  tray->watcher_kde = create_watcher(tray, "kde");
  init_host(tray, "freedesktop", &tray->host_xdg);
  init_host(tray, "kde", &tray->host_kde);
  // Clean up stale tray items when services die
  sd_bus_match_signal(bus, &tray->name_slot, "org.freedesktop.DBus",
                      "/org/freedesktop/DBus", "org.freedesktop.DBus",
                      "NameOwnerChanged", handle_name_owner_changed, tray);
  sd_bus_process(bus, NULL);
  return tray;
}

void tray_destroy(MangobarTray *tray) {
  if (!tray)
    return;
  if (tray->name_slot)
    sd_bus_slot_unref(tray->name_slot);
  if (tray->host_xdg) {
    if (tray->host_xdg->service)
      sd_bus_release_name(tray->bus, tray->host_xdg->service);
    if (tray->host_xdg->reg_slot)
      sd_bus_slot_unref(tray->host_xdg->reg_slot);
    if (tray->host_xdg->unreg_slot)
      sd_bus_slot_unref(tray->host_xdg->unreg_slot);
    if (tray->host_xdg->watcher_slot)
      sd_bus_slot_unref(tray->host_xdg->watcher_slot);
    free(tray->host_xdg->interface);
    free(tray->host_xdg->service);
    free(tray->host_xdg);
  }
  if (tray->host_kde) {
    if (tray->host_kde->service)
      sd_bus_release_name(tray->bus, tray->host_kde->service);
    if (tray->host_kde->reg_slot)
      sd_bus_slot_unref(tray->host_kde->reg_slot);
    if (tray->host_kde->unreg_slot)
      sd_bus_slot_unref(tray->host_kde->unreg_slot);
    if (tray->host_kde->watcher_slot)
      sd_bus_slot_unref(tray->host_kde->watcher_slot);
    free(tray->host_kde->interface);
    free(tray->host_kde->service);
    free(tray->host_kde);
  }
  if (tray->watcher_xdg) {
    sd_bus_release_name(tray->bus, tray->watcher_xdg->interface);
    sd_bus_slot_unref(tray->watcher_xdg->vtable_slot);
    sd_bus_slot_unref(tray->watcher_xdg->signal_slot);
    for (int i = 0; i < tray->watcher_xdg->item_count; i++)
      free(tray->watcher_xdg->items[i]);
    free(tray->watcher_xdg->items);
    for (int i = 0; i < tray->watcher_xdg->host_count; i++)
      free(tray->watcher_xdg->hosts[i]);
    free(tray->watcher_xdg->hosts);
    free(tray->watcher_xdg->interface);
    free(tray->watcher_xdg);
  }
  if (tray->watcher_kde) {
    sd_bus_release_name(tray->bus, tray->watcher_kde->interface);
    sd_bus_slot_unref(tray->watcher_kde->vtable_slot);
    sd_bus_slot_unref(tray->watcher_kde->signal_slot);
    for (int i = 0; i < tray->watcher_kde->item_count; i++)
      free(tray->watcher_kde->items[i]);
    free(tray->watcher_kde->items);
    for (int i = 0; i < tray->watcher_kde->host_count; i++)
      free(tray->watcher_kde->hosts[i]);
    free(tray->watcher_kde->hosts);
    free(tray->watcher_kde->interface);
    free(tray->watcher_kde);
  }
  for (int i = 0; i < tray->item_count; i++)
    destroy_sni(tray->items[i]);
  free(tray->items);
  sd_bus_flush_close_unref(tray->bus);
  free(tray);
}

int tray_get_fd(MangobarTray *tray) { return tray ? tray->fd : -1; }

int tray_get_events(MangobarTray *tray) {
  if (!tray || !tray->bus)
    return POLLIN;
  int ev = sd_bus_get_events(tray->bus);
  return ev >= 0 ? ev : POLLIN;
}

void tray_dispatch(MangobarTray *tray) {
  if (!tray || tray->dead)
    return;
  int ret;
  while ((ret = sd_bus_process(tray->bus, NULL)) > 0) {
  }
  // Flush outgoing messages (e.g. menu Event clicks) promptly
  sd_bus_flush(tray->bus);
  if (ret < 0)
    tray->dead = true;
}

MangobarTrayItem **tray_visible_items(MangobarTray *tray, int *count) {
  *count = 0;
  if (!tray)
    return NULL;
  static MangobarTrayItem *out[MANGOBAR_TRAY_MAX_ITEMS];
  for (int i = 0; i < tray->item_count && *count < MANGOBAR_TRAY_MAX_ITEMS;
       i++) {
    MangobarTrayItem *it = tray->items[i];
    if (!it->status || it->status[0] == 'P' || !it->icon)
      continue;
    out[(*count)++] = it;
  }
  return out;
}

const char *tray_item_id(MangobarTrayItem *item) {
  return item ? item->watcher_id : NULL;
}
pixman_image_t *tray_item_icon(MangobarTrayItem *item) {
  return item ? item->icon : NULL;
}
int tray_item_icon_size(MangobarTrayItem *item) {
  return item ? item->icon_size : 0;
}
unsigned tray_item_icon_rev(MangobarTrayItem *item) {
  return item ? item->icon_rev : 0;
}
void tray_remove_item(MangobarTray *tray, MangobarTrayItem *item) {
  if (!tray || !item)
    return;
  for (int i = 0; i < tray->item_count; i++) {
    if (tray->items[i] == item) {
      destroy_sni(item);
      memmove(&tray->items[i], &tray->items[i + 1],
              sizeof(*tray->items) * (tray->item_count - i - 1));
      tray->item_count--;
      set_tray_dirty(tray);
      return;
    }
  }
}
void tray_prune(MangobarTray *tray) {
  if (!tray || tray->dead)
    return;
  for (int i = 0; i < tray->item_count; i++) {
    MangobarTrayItem *it = tray->items[i];
    if (!it->service)
      continue;
    // Check whether the service name is still owned via GetNameOwner
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_call_method(
        tray->bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetNameOwner", &err, &reply, "s", it->service);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);
    if (r < 0) { // no owner -> stale, remove
      destroy_sni(it);
      memmove(&tray->items[i], &tray->items[i + 1],
              sizeof(*tray->items) * (tray->item_count - i - 1));
      tray->item_count--;
      set_tray_dirty(tray);
      i--;
    }
  }
}

void tray_refresh(MangobarTray *tray) {
  if (!tray || tray->dead)
    return;
  tray_prune(tray);
  if (tray->host_xdg)
    register_to_watcher(tray->host_xdg);
  if (tray->host_kde)
    register_to_watcher(tray->host_kde);
  sd_bus_flush(tray->bus);
}
bool tray_item_has_menu(MangobarTrayItem *item) {
  return item && item->menu && *item->menu;
}
const char *tray_item_service(MangobarTrayItem *item) {
  return item ? item->service : NULL;
}
const char *tray_item_menu_path(MangobarTrayItem *item) {
  return item ? item->menu : NULL;
}
void *tray_get_bus(MangobarTray *tray) { return tray ? tray->bus : NULL; }

static uint32_t event_to_x11_button(uint32_t button) {
  switch (button) {
  case 0x110: // BTN_LEFT
    return 1;
  case 0x112: // BTN_MIDDLE
    return 2;
  case 0x111: // BTN_RIGHT
    return 3;
  case 4:
  case 5:
  case 6:
  case 7:
    return button;
  case 0x113: // BTN_SIDE
    return 8;
  case 0x114: // BTN_EXTRA
    return 9;
  default:
    return 0;
  }
}

void tray_handle_click(MangobarTray *tray, MangobarTrayItem *sni, double x,
                       double y, uint32_t button) {
  if (!tray || tray->dead || !sni)
    return;
  static const char *defaults[10] = {NULL,          "Activate",
                                     "SecondaryActivate", "ContextMenu",
                                     "ScrollUp",    "ScrollDown",
                                     "ScrollLeft",  "ScrollRight",
                                     NULL,          NULL};
  uint32_t idx = event_to_x11_button(button);
  if (idx == 0 || idx > 9)
    return;
  const char *method = defaults[idx];
  if (!method)
    return;
  if (sni->item_is_menu && strcmp(method, "Activate") == 0)
    method = "ContextMenu";
  if (strncmp(method, "Scroll", 6) == 0) {
    char dir = method[6];
    const char *orientation =
        (dir == 'U' || dir == 'D') ? "vertical" : "horizontal";
    int sign = (dir == 'U' || dir == 'L') ? -1 : 1;
    sd_bus_call_method_async(tray->bus, NULL, sni->service, sni->path,
                             sni->interface, "Scroll", NULL, NULL, "is", sign,
                             orientation);
  } else {
    sd_bus_call_method_async(tray->bus, NULL, sni->service, sni->path,
                             sni->interface, method, NULL, NULL, "ii", (int)x,
                             (int)y);
  }
}
