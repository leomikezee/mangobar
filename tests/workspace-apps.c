#define _GNU_SOURCE
#include "workspace-apps.h"
#include "icons.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int server;
static uint64_t now = 1000;

static int begin_query(void) {
  g_assert_false(workspace_apps_refresh(now));
  now += 1000;
  g_assert_cmpint(workspace_apps_fd(), >=, 0);
  int peer = accept(server, NULL, NULL);
  g_assert_cmpint(peer, >=, 0);
  char command[64] = {0};
  ssize_t n = read(peer, command, sizeof(command) - 1);
  g_assert_cmpint(n, ==, (ssize_t)strlen("get all-clients\n"));
  g_assert_cmpstr(command, ==, "get all-clients\n");
  return peer;
}

static bool query(const char *json) {
  int peer = begin_query();
  g_assert_cmpint(write(peer, json, strlen(json)), ==, (ssize_t)strlen(json));
  g_assert_cmpint(write(peer, "\n", 1), ==, 1);
  bool changed = workspace_apps_dispatch();
  close(peer);
  g_assert_cmpint(workspace_apps_fd(), ==, -1);
  return changed;
}

static void write_file(const char *root, const char *relative, const char *text) {
  char *path = g_build_filename(root, relative, NULL);
  char *parent = g_path_get_dirname(path);
  g_assert_cmpint(g_mkdir_with_parents(parent, 0700), ==, 0);
  g_assert_true(g_file_set_contents(path, text, -1, NULL));
  g_free(parent);
  g_free(path);
}

static void write_icon(const char *root, const char *name, guint32 color) {
  char *directory = g_build_filename(root, "icons/hicolor/32x32/apps", NULL);
  g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0);
  char *path = g_build_filename(directory, name, NULL);
  GdkPixbuf *pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 32, 32);
  gdk_pixbuf_fill(pixbuf, color);
  g_assert_true(gdk_pixbuf_save(pixbuf, path, "png", NULL, NULL));
  g_object_unref(pixbuf);
  g_free(directory);
  g_free(path);
}

static WorkspaceIcons icons(const char *monitor, int tag, int scale, int limit) {
  return workspace_apps_icons(monitor, tag, 16, scale, 4, limit);
}

static void assert_color(pixman_image_t *image, uint32_t color, int size) {
  g_assert_nonnull(image);
  g_assert_cmpint(pixman_image_get_width(image), ==, size);
  g_assert_cmphex(pixman_image_get_data(image)[0], ==, color);
}

static void remove_tree(const char *path) {
  GDir *directory = g_dir_open(path, 0, NULL);
  if (directory) {
    const char *name;
    while ((name = g_dir_read_name(directory))) {
      char *child = g_build_filename(path, name, NULL);
      remove_tree(child);
      g_free(child);
    }
    g_dir_close(directory);
    g_rmdir(path);
  } else {
    g_unlink(path);
  }
}

int main(void) {
  char *root = g_dir_make_tmp("mangobar-icons-XXXXXX", NULL);
  g_assert_nonnull(root);
  g_setenv("XDG_DATA_HOME", root, TRUE);
  char *icon_root = g_build_filename(root, "system-data", NULL);
  GString *data_dirs = g_string_new(NULL);
  for (int i = 0; i < 20; i++)
    g_string_append_printf(data_dirs, "%s/missing-%d:", root, i);
  g_string_append(data_dirs, icon_root);
  g_setenv("XDG_DATA_DIRS", data_dirs->str, TRUE);
  g_string_free(data_dirs, TRUE);
  g_setenv("XDG_CONFIG_HOME", root, TRUE);
  g_setenv("HOME", root, TRUE);
  write_file(root, "gtk-3.0/settings.ini", "[Settings]\ngtk-icon-theme-name=hicolor\n");
  write_file(root, "applications/org.test.Editor.desktop",
             "[Desktop Entry]\nType=Application\nName=Editor\nExec=/bin/sh\nIcon=editor\n");
  write_file(root, "applications/terminal.desktop",
             "[Desktop Entry]\nType=Application\nName=Terminal\nExec=/bin/sh\n"
             "StartupWMClass=TestTerminal\nIcon=terminal\n");
  write_icon(icon_root, "editor.png", 0xff0000ff);
  write_icon(icon_root, "terminal.png", 0x0000ffff);
  write_icon(icon_root, "application-x-executable.png", 0x00ff00ff);

  struct sockaddr_un address = {.sun_family = AF_UNIX};
  g_snprintf(address.sun_path, sizeof(address.sun_path), "%s/ipc.sock", root);
  g_setenv("MANGO_INSTANCE_SIGNATURE", address.sun_path, TRUE);
  server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  g_assert_cmpint(server, >=, 0);
  g_assert_cmpint(bind(server, (struct sockaddr *)&address, sizeof(address)), ==, 0);
  g_assert_cmpint(listen(server, 4), ==, 0);

  const char *snapshot = "{\"clients\":["
      "{\"appid\":\"org.test.Editor\",\"monitor\":\"DP-1\",\"tags\":[1]},"
      "{\"appid\":\"testterminal\",\"monitor\":\"DP-1\",\"tags\":[1,2]},"
      "{\"appid\":\"ORG.TEST.EDITOR\",\"monitor\":\"DP-2\",\"tags\":[1]},"
      "{\"appid\":\"org.test.Editor\",\"monitor\":\"DP-1\",\"tags\":[1],\"is_swallowedby\":true},"
      "{\"appid\":\"org.test.Editor\",\"monitor\":\"DP-1\",\"tags\":[1],\"is_scratchpad\":true},"
      "{\"appid\":\"org.test.Editor\",\"monitor\":\"DP-1\",\"tags\":[1],\"is_namedscratchpad\":true},"
      "{\"appid\":null,\"monitor\":\"DP-1\",\"tags\":[1]},"
      "{\"appid\":\"org.test.Editor\",\"monitor\":\"DP-1\",\"tags\":[0,33]}]}";
  g_assert_true(query(snapshot));
  WorkspaceIcons first = icons("DP-1", 0, 1, 5);
  g_assert_cmpint(first.count, ==, 2);
  assert_color(first.images[0], 0xffff0000, 16);
  assert_color(first.images[1], 0xff0000ff, 16);
  g_assert_cmpint(icons("DP-1", 1, 1, 5).count, ==, 1);
  g_assert_cmpint(icons("DP-1", 2, 1, 5).count, ==, 0);
  g_assert_cmpint(icons("DP-1", 0, 1, 1).count, ==, 1);
  WorkspaceIcons other = icons("DP-2", 0, 2, 5);
  g_assert_cmpint(other.count, ==, 1);
  assert_color(other.images[0], 0xffff0000, 32);
  g_assert_false(query(snapshot));
  g_assert_false(query("{\"clients\":null}"));
  g_assert_cmpint(icons("DP-1", 0, 1, 5).count, ==, 2);

  // Partial replies must not replace the previous snapshot.
  int peer = begin_query();
  const char *partial = "{\"clients\":[{\"appid\":\"unknown-app\",";
  g_assert_cmpint(write(peer, partial, strlen(partial)), ==, (ssize_t)strlen(partial));
  g_assert_false(workspace_apps_dispatch());
  g_assert_cmpint(icons("DP-1", 0, 1, 5).count, ==, 2);
  const char *rest = "\"monitor\":\"DP-1\",\"tags\":[3]}]}\n";
  g_assert_cmpint(write(peer, rest, strlen(rest)), ==, (ssize_t)strlen(rest));
  g_assert_true(workspace_apps_dispatch());
  close(peer);
  g_assert_cmpint(icons("DP-1", 0, 1, 5).count, ==, 0);
  WorkspaceIcons fallback = icons("DP-1", 2, 1, 5);
  g_assert_cmpint(fallback.count, ==, 1);
  assert_color(fallback.images[0], 0xff00ff00, 16);

  // Loaded images remain available even if their source file disappears.
  char *editor = g_build_filename(icon_root, "icons/hicolor/32x32/apps/editor.png", NULL);
  g_assert_cmpint(g_unlink(editor), ==, 0);
  g_free(editor);
  g_assert_true(query(snapshot));
  g_assert_true(icons("DP-1", 0, 1, 5).images[0] == first.images[0]);
  g_assert_true(query("{\"clients\":[]}"));
  g_assert_cmpint(icons("DP-1", 0, 1, 5).count, ==, 0);
  g_assert_false(query("{\"clients\":[]}"));

  workspace_apps_finish();
  icons_finish();
  close(server);
  remove_tree(root);
  g_free(icon_root);
  g_free(root);
  return 0;
}
