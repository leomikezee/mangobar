#define _GNU_SOURCE
#include "workspace-apps.h"
#include "icons.h"

#include <cjson/cJSON.h>
#include <errno.h>
#include <gio/gdesktopappinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_REPLY_SIZE (4 * 1024 * 1024)
#define MAX_CACHED_ICONS 256

typedef struct {
  char *appid;
  char *monitor;
  uint32_t tags;
} Client;

static GPtrArray *clients;
static GHashTable *app_icons;
static GList *desktop_apps;
static bool desktop_apps_loaded;
static int fd = -1;
static GString *reply;
static uint64_t next_query;
static uint64_t deadline;

static void client_free(gpointer data) {
  Client *client = data;
  g_free(client->appid);
  g_free(client->monitor);
  g_free(client);
}

static bool update_clients(const char *text) {
  cJSON *root = cJSON_Parse(text);
  cJSON *array = cJSON_GetObjectItemCaseSensitive(root, "clients");
  if (!cJSON_IsArray(array)) {
    cJSON_Delete(root);
    return false;
  }
  GPtrArray *next = g_ptr_array_new_with_free_func(client_free);
  cJSON *item;
  cJSON_ArrayForEach(item, array) {
    const char *appid = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(item, "appid"));
    const char *monitor = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(item, "monitor"));
    cJSON *tags = cJSON_GetObjectItemCaseSensitive(item, "tags");
    if (!appid || !*appid || !monitor || !cJSON_IsArray(tags) ||
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "is_swallowedby")) ||
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "is_scratchpad")) ||
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "is_namedscratchpad")))
      continue;
    uint32_t mask = 0;
    cJSON *tag;
    cJSON_ArrayForEach(tag, tags) {
      if (cJSON_IsNumber(tag) && tag->valueint >= 1 && tag->valueint <= 32 &&
          tag->valuedouble == tag->valueint)
        mask |= UINT32_C(1) << (tag->valueint - 1);
    }
    if (!mask)
      continue;
    Client *client = g_new0(Client, 1);
    client->appid = g_strdup(appid);
    client->monitor = g_strdup(monitor);
    client->tags = mask;
    g_ptr_array_add(next, client);
  }
  cJSON_Delete(root);

  bool changed = !clients || clients->len != next->len;
  for (guint i = 0; !changed && i < next->len; i++) {
    const Client *a = clients->pdata[i], *b = next->pdata[i];
    changed = a->tags != b->tags || strcmp(a->appid, b->appid) != 0 ||
              strcmp(a->monitor, b->monitor) != 0;
  }
  if (clients)
    g_ptr_array_unref(clients);
  clients = next;
  return changed;
}

static void close_query(void) {
  if (fd >= 0)
    close(fd);
  fd = -1;
  if (reply)
    g_string_truncate(reply, 0);
}

int workspace_apps_fd(void) {
  return fd;
}

bool workspace_apps_refresh(uint64_t now) {
  if (fd >= 0) {
    if (now < deadline)
      return false;
    close_query();
  }
  if (now < next_query)
    return false;
  next_query = now + 1000;
  const char *path = getenv("MANGO_INSTANCE_SIGNATURE");
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  if (!path || strlen(path) >= sizeof(address.sun_path))
    return false;
  memcpy(address.sun_path, path, strlen(path) + 1);
  fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return false;
  static const char command[] = "get all-clients\n";
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
      send(fd, command, sizeof(command) - 1, MSG_NOSIGNAL) != sizeof(command) - 1) {
    close_query();
    bool changed = clients && clients->len > 0;
    if (clients)
      g_ptr_array_set_size(clients, 0);
    return changed;
  }
  if (!reply)
    reply = g_string_new(NULL);
  deadline = now + 1000;
  return false;
}

bool workspace_apps_dispatch(void) {
  char buffer[8192];
  while (fd >= 0) {
    ssize_t n = recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return false;
    if (n <= 0 || reply->len + (size_t)n > MAX_REPLY_SIZE) {
      close_query();
      return false;
    }
    g_string_append_len(reply, buffer, n);
    char *newline = memchr(reply->str, '\n', reply->len);
    if (newline) {
      *newline = '\0';
      bool changed = update_clients(reply->str);
      close_query();
      return changed;
    }
  }
  return false;
}

static char *desktop_icon_name(const char *appid) {
  char *id = g_str_has_suffix(appid, ".desktop")
                 ? g_strdup(appid) : g_strconcat(appid, ".desktop", NULL);
  GDesktopAppInfo *info = g_desktop_app_info_new(id);
  if (info) {
    char *name = g_desktop_app_info_get_string(info, "Icon");
    g_object_unref(info);
    g_free(id);
    return name;
  }
  if (!desktop_apps_loaded) {
    desktop_apps = g_app_info_get_all();
    desktop_apps_loaded = true;
  }
  GDesktopAppInfo *wm_match = NULL;
  for (GList *it = desktop_apps; it; it = it->next) {
    if (!G_IS_DESKTOP_APP_INFO(it->data))
      continue;
    GDesktopAppInfo *app = it->data;
    const char *app_id = g_app_info_get_id(G_APP_INFO(app));
    if (app_id && g_ascii_strcasecmp(id, app_id) == 0) {
      info = app;
      break;
    }
    const char *wm_class = g_desktop_app_info_get_startup_wm_class(app);
    if (!wm_match && wm_class && g_ascii_strcasecmp(appid, wm_class) == 0)
      wm_match = app;
  }
  g_free(id);
  if (!info)
    info = wm_match;
  return info ? g_desktop_app_info_get_string(info, "Icon") : NULL;
}

static void image_free(gpointer image) {
  if (image)
    pixman_image_unref(image);
}

static pixman_image_t *app_icon(const char *appid, int size) {
  if (!app_icons)
    app_icons = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, image_free);
  char *key = g_strdup_printf("%d:%s", size, appid);
  gpointer image;
  if (g_hash_table_lookup_extended(app_icons, key, NULL, &image)) {
    g_free(key);
    return image;
  }
  if (g_hash_table_size(app_icons) >= MAX_CACHED_ICONS) {
    g_free(key);
    return NULL;
  }
  char *name = desktop_icon_name(appid);
  image = icon_load(name ? name : appid, NULL, size);
  g_free(name);
  if (!image)
    image = icon_load("application-x-executable", NULL, size);
  g_hash_table_insert(app_icons, key, image);
  return image;
}

WorkspaceIcons workspace_apps_icons(const char *monitor, int tag, int size,
                                    int scale, int spacing, int limit) {
  WorkspaceIcons icons = {.size = size, .spacing = spacing};
  if (!clients || !monitor || tag < 0 || tag >= 32 || size < 1 || scale < 1)
    return icons;
  if (limit > WORKSPACE_MAX_ICONS)
    limit = WORKSPACE_MAX_ICONS;
  for (guint i = 0; i < clients->len && icons.count < limit; i++) {
    const Client *client = clients->pdata[i];
    if (!(client->tags & (UINT32_C(1) << tag)) || strcmp(client->monitor, monitor) != 0)
      continue;
    pixman_image_t *image = app_icon(client->appid, size * scale);
    if (image)
      icons.images[icons.count++] = image;
  }
  return icons;
}

void workspace_apps_finish(void) {
  close_query();
  if (reply)
    g_string_free(reply, TRUE);
  reply = NULL;
  g_clear_pointer(&clients, g_ptr_array_unref);
  g_clear_pointer(&app_icons, g_hash_table_unref);
  g_list_free_full(desktop_apps, g_object_unref);
  desktop_apps = NULL;
  desktop_apps_loaded = false;
  next_query = 0;
}
