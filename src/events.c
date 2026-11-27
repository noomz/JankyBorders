#include "events.h"
#include "misc/extern.h"
#include "windows.h"
#include "border.h"
#include "misc/window.h"
#include <libproc.h>

extern struct table g_windows;
extern pid_t g_pid;

bool g_mission_control_active;

// On macOS 27, WindowManager places a level-19 backdrop on each display
// during Mission Control instead of hiding our independent overlay windows.
#define MISSION_CONTROL_BACKDROP_LEVEL 19

static bool covers_display(CGRect bounds) {
  CGDirectDisplayID display;
  uint32_t count = 0;
  return CGGetDisplaysWithRect(bounds, 1, &display, &count) == kCGErrorSuccess
         && count && CGRectEqualToRect(bounds, CGDisplayBounds(display));
}

static bool mission_control_backdrop_is_visible(void) {
  CFArrayRef windows = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly,
                                                 kCGNullWindowID);
  if (!windows) return false;

  bool found = false;
  for (CFIndex i = 0; i < CFArrayGetCount(windows); ++i) {
    CFDictionaryRef window = CFArrayGetValueAtIndex(windows, i);
    CFNumberRef layer = CFDictionaryGetValue(window, kCGWindowLayer);
    int level;
    if (!layer || !CFNumberGetValue(layer, kCFNumberIntType, &level)
        || level != MISSION_CONTROL_BACKDROP_LEVEL) continue;

    CFStringRef owner = CFDictionaryGetValue(window, kCGWindowOwnerName);
    CFDictionaryRef geometry = CFDictionaryGetValue(window, kCGWindowBounds);
    CGRect bounds;
    if (owner && CFEqual(owner, CFSTR("WindowManager"))
        && geometry && CGRectMakeWithDictionaryRepresentation(geometry, &bounds)
        && covers_display(bounds)) {
      found = true;
      break;
    }
  }
  CFRelease(windows);
  return found;
}

static void mission_control_watch_for_exit(void) {
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC),
                 dispatch_get_main_queue(), ^{
    if (mission_control_backdrop_is_visible()) {
      mission_control_watch_for_exit();
    } else {
      g_mission_control_active = false;
      windows_draw_borders_on_current_spaces(&g_windows);
      windows_determine_and_focus_active_window(&g_windows);
    }
  });
}

static void mission_control_enter(void) {
  if (g_mission_control_active) return;
  g_mission_control_active = true;
  for (int i = 0; i < g_windows.capacity; ++i) {
    for (struct bucket* bucket = g_windows.buckets[i]; bucket; bucket = bucket->next) {
      struct border* border = bucket->value;
      if (!border) continue;
      border_hide(border);
      if (border->proxy) border_hide(border->proxy);
    }
  }
  mission_control_watch_for_exit();
}

static void mission_control_check_window(int cid, uint32_t wid) {
  if (g_mission_control_active
      || window_level(cid, wid) != MISSION_CONTROL_BACKDROP_LEVEL) return;

  int owner;
  pid_t pid;
  char name[PROC_PIDPATHINFO_MAXSIZE];
  CGRect bounds;
  if (SLSGetWindowOwner(cid, wid, &owner) == kCGErrorSuccess
      && SLSConnectionGetPID(owner, &pid) == kCGErrorSuccess
      && proc_name(pid, name, sizeof(name)) > 0
      && strcmp(name, "WindowManager") == 0
      && SLSGetWindowBounds(cid, wid, &bounds) == kCGErrorSuccess
      && covers_display(bounds)) {
    mission_control_enter();
  }
}

#ifdef DEBUG
static void dump_event(void* data, size_t data_length) {
  for (int i = 0; i < data_length; i++) {
    printf("%02x ", *((unsigned char*)data + i));
  }
  printf("\n");
}

static void event_watcher(uint32_t event, void* data, size_t data_length, void* context) {
  static int count = 0;
  printf("(%d) Event: %d; Payload:\n", ++count, event);
  dump_event(data, data_length);
}
#endif

struct window_spawn_data {
  uint64_t sid;
  uint32_t wid;
};

static bool is_own_window(int cid, uint32_t wid) {
  int wid_cid = 0;
  SLSGetWindowOwner(cid, wid, &wid_cid);
  pid_t pid = 0;
  SLSConnectionGetPID(wid_cid, &pid);
  return pid == g_pid;
}

// Creating a border allocates a window and draws it, which takes tens of
// milliseconds for a large window. On a workspace switch WindowServer sends the
// creates for the incoming windows a few milliseconds before the destroys for
// the outgoing ones. Creating borders inline blocked those destroys, and the
// outgoing borders stayed on screen without their windows. Creates therefore
// wait CREATE_BATCH_DELAY so the destroys of the same switch run first, and
// then run one per main queue turn so a later destroy is never stuck behind
// the whole batch.
#define CREATE_BATCH_DELAY (50 * NSEC_PER_MSEC)

struct pending_create {
  uint32_t wid;
  uint64_t sid;
};

static struct pending_create* g_pending_creates = NULL;
static int g_pending_create_count = 0;
static bool g_pending_create_scheduled = false;

static void pending_create_remove(uint32_t wid) {
  for (int i = 0; i < g_pending_create_count; i++) {
    if (g_pending_creates[i].wid != wid) continue;
    memmove(&g_pending_creates[i],
            &g_pending_creates[i + 1],
            sizeof(struct pending_create) * (g_pending_create_count - i - 1));
    g_pending_create_count--;
    return;
  }
}

static bool g_focus_scheduled = false;

static void schedule_focus_update(void) {
  if (g_focus_scheduled) return;
  g_focus_scheduled = true;
  dispatch_async(dispatch_get_main_queue(), ^{
    g_focus_scheduled = false;
    windows_determine_and_focus_active_window(&g_windows);
  });
}

static void pending_create_process_next(void) {
  if (g_pending_create_count == 0) {
    g_pending_create_scheduled = false;
    schedule_focus_update();
    return;
  }

  struct pending_create next = g_pending_creates[0];
  pending_create_remove(next.wid);
  if (windows_window_create(&g_windows, next.wid, next.sid)) {
    debug("Window Created: %d %d\n", next.wid, next.sid);
  }

  dispatch_async(dispatch_get_main_queue(), ^{
    pending_create_process_next();
  });
}

static void pending_create_add(uint32_t wid, uint64_t sid) {
  pending_create_remove(wid);
  g_pending_creates = realloc(g_pending_creates,
                              sizeof(struct pending_create)
                              * (g_pending_create_count + 1));
  g_pending_creates[g_pending_create_count++]
    = (struct pending_create){ .wid = wid, .sid = sid };

  if (g_pending_create_scheduled) return;
  g_pending_create_scheduled = true;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, CREATE_BATCH_DELAY),
                 dispatch_get_main_queue(), ^{
    pending_create_process_next();
  });
}

static void window_spawn_handler(uint32_t event, struct window_spawn_data* data, size_t _, int cid) {
  struct table* windows = &g_windows;
  uint32_t wid = data->wid;
  uint64_t sid = data->sid;

  if (!wid || !sid || is_own_window(cid, wid)) return;

  if (event == EVENT_WINDOW_CREATE) {
    if (__builtin_available(macOS 27.0, *)) {
      mission_control_check_window(cid, wid);
    }
    pending_create_add(wid, sid);
  } else if (event == EVENT_WINDOW_DESTROY) {
    pending_create_remove(wid);
    if (windows_window_destroy(windows, wid, sid)) {
      debug("Window Destroyed: %d %d\n", wid, sid);
    }
    schedule_focus_update();
  }
}

static void window_modify_handler(uint32_t event, uint32_t* window_id, size_t _, int cid) {
  uint32_t wid = *window_id;
  struct table* windows = &g_windows;

  if (is_own_window(cid, wid)) return;

  if (event == EVENT_WINDOW_MOVE) {
    debug("Window Move: %d\n", wid);
    windows_window_move(windows, wid);
  } else if (event == EVENT_WINDOW_RESIZE) {
    debug("Window Resize: %d\n", wid);
    windows_window_update(windows, wid);
  } else if (event == EVENT_WINDOW_REORDER) {
    debug("Window Reorder (and focus): %d\n", wid);
    windows_window_update(windows, wid);
    DELAY_ASYNC_EXEC_ON_MAIN_THREAD(10000, {
      windows_determine_and_focus_active_window(windows);
    });
  } else if (event == EVENT_WINDOW_LEVEL) {
    debug("Window Level: %d\n", wid);
    windows_window_update(windows, wid);
  } else if (event == EVENT_WINDOW_TITLE || event == EVENT_WINDOW_UPDATE) {
    debug("Window Focus\n");
    DELAY_ASYNC_EXEC_ON_MAIN_THREAD(50000, {
      windows_determine_and_focus_active_window(windows);
    });
  } else if (event == EVENT_WINDOW_UNHIDE) {
    debug("Window Unhide: %d\n", wid);
    windows_window_unhide(windows, wid);
  } else if (event == EVENT_WINDOW_HIDE) {
    debug("Window Hide: %d\n", wid);
    windows_window_hide(windows, wid);
  } else if (event == EVENT_WINDOW_CLOSE) {
    debug("Window Close: %d\n", wid);
    windows_window_destroy(windows, wid, 0);
  }
}

static void front_app_handler() {
  debug("Window Focus\n");
  DELAY_ASYNC_EXEC_ON_MAIN_THREAD(50000, {
    windows_determine_and_focus_active_window(&g_windows);
  });
}

static void space_handler() {
  // Not all native-fullscreen windows have yet updated their space id...
  DELAY_ASYNC_EXEC_ON_MAIN_THREAD(20000, {
    windows_draw_borders_on_current_spaces(&g_windows);
  });
}

void events_register(int cid) {
  void* cid_ctx = (void*)(intptr_t)cid;

  SLSRegisterNotifyProc(window_modify_handler, EVENT_WINDOW_CLOSE, cid_ctx);
  SLSRegisterNotifyProc(window_modify_handler, EVENT_WINDOW_MOVE, cid_ctx);
  SLSRegisterNotifyProc(window_modify_handler, EVENT_WINDOW_RESIZE, cid_ctx);
  SLSRegisterNotifyProc(window_modify_handler, EVENT_WINDOW_LEVEL, cid_ctx);
  SLSRegisterNotifyProc(window_modify_handler, EVENT_WINDOW_UNHIDE, cid_ctx);
  SLSRegisterNotifyProc(window_modify_handler, EVENT_WINDOW_HIDE, cid_ctx);
  SLSRegisterNotifyProc(window_modify_handler, EVENT_WINDOW_TITLE, cid_ctx);
  SLSRegisterNotifyProc(window_modify_handler, EVENT_WINDOW_REORDER, cid_ctx);
  SLSRegisterNotifyProc(window_modify_handler, EVENT_WINDOW_UPDATE, cid_ctx);
  SLSRegisterNotifyProc(window_spawn_handler, EVENT_WINDOW_CREATE, cid_ctx);
  SLSRegisterNotifyProc(window_spawn_handler, EVENT_WINDOW_DESTROY, cid_ctx);

  SLSRegisterNotifyProc(space_handler, EVENT_SPACE_CHANGE, cid_ctx);

  SLSRegisterNotifyProc(front_app_handler, EVENT_FRONT_CHANGE, cid_ctx);

  if (__builtin_available(macOS 27.0, *)) {
    if (mission_control_backdrop_is_visible()) mission_control_enter();
  }

#ifdef DEBUG
  for (int i = 0; i < 2000; i++) {
    SLSRegisterNotifyProc(event_watcher, i, NULL);
  }
#endif
}
