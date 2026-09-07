/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef MozWayland_h_
#define MozWayland_h_

#include "mozilla/Types.h"
#include <gtk/gtk.h>
#include <gdk/gdkwayland.h>

#ifdef __cplusplus
extern "C" {
#endif

MOZ_EXPORT struct wl_display* wl_display_connect(const char* name);
MOZ_EXPORT int wl_display_roundtrip_queue(struct wl_display* display,
                                          struct wl_event_queue* queue);
MOZ_EXPORT uint32_t wl_proxy_get_version(struct wl_proxy* proxy);
MOZ_EXPORT void wl_proxy_marshal(struct wl_proxy* p, uint32_t opcode, ...);
MOZ_EXPORT struct wl_proxy* wl_proxy_marshal_constructor(
    struct wl_proxy* proxy, uint32_t opcode,
    const struct wl_interface* interface, ...);
MOZ_EXPORT struct wl_proxy* wl_proxy_marshal_constructor_versioned(
    struct wl_proxy* proxy, uint32_t opcode,
    const struct wl_interface* interface, uint32_t version, ...);
MOZ_EXPORT struct wl_proxy* wl_proxy_marshal_flags(
    struct wl_proxy* proxy, uint32_t opcode,
    const struct wl_interface* interface, uint32_t version, uint32_t flags,
    ...);
MOZ_EXPORT void wl_proxy_destroy(struct wl_proxy* proxy);
MOZ_EXPORT void* wl_proxy_create_wrapper(void* proxy);
MOZ_EXPORT void wl_proxy_wrapper_destroy(void* proxy_wrapper);
MOZ_EXPORT void wl_display_set_max_buffer_size(struct wl_display* display,
                                               size_t max_buffer_size);

#ifndef WL_MARSHAL_FLAG_DESTROY
#  define WL_MARSHAL_FLAG_DESTROY (1 << 0)
#endif

#ifndef WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION
#  define WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION 4
#endif

#ifndef WL_DATA_DEVICE_MANAGER_DND_ACTION_ENUM
enum wl_data_device_manager_dnd_action {
  WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE = 0,
  WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY = 1,
  WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE = 2,
  WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK = 4
};
#endif

#ifndef WL_DATA_OFFER_SET_ACTIONS
#  define WL_DATA_OFFER_SET_ACTIONS 4

struct moz_wl_data_offer_listener {
  void (*offer)(void* data, struct wl_data_offer* wl_data_offer,
                const char* mime_type);
  void (*source_actions)(void* data, struct wl_data_offer* wl_data_offer,
                         uint32_t source_actions);
  void (*action)(void* data, struct wl_data_offer* wl_data_offer,
                 uint32_t dnd_action);
};

static inline void wl_data_offer_set_actions(
    struct wl_data_offer* wl_data_offer, uint32_t dnd_actions,
    uint32_t preferred_action) {
  wl_proxy_marshal((struct wl_proxy*)wl_data_offer, WL_DATA_OFFER_SET_ACTIONS,
                   dnd_actions, preferred_action);
}
#else
typedef struct wl_data_offer_listener moz_wl_data_offer_listener;
#endif

#ifndef WL_DATA_OFFER_FINISH
#  define WL_DATA_OFFER_FINISH 3
static inline void wl_data_offer_finish(struct wl_data_offer* wl_data_offer) {
  wl_proxy_marshal_flags(
      (struct wl_proxy*)wl_data_offer, WL_DATA_OFFER_FINISH, NULL,
      wl_proxy_get_version((struct wl_proxy*)wl_data_offer), 0);
}
#endif

#ifndef WL_SUBCOMPOSITOR_GET_SUBSURFACE
#  define WL_SUBCOMPOSITOR_GET_SUBSURFACE 1
struct wl_subcompositor;

#  pragma GCC visibility push(default)
extern const struct wl_interface wl_subsurface_interface;
extern const struct wl_interface wl_subcompositor_interface;
#  pragma GCC visibility pop

#  define WL_SUBSURFACE_DESTROY 0
#  define WL_SUBSURFACE_SET_POSITION 1
#  define WL_SUBSURFACE_PLACE_ABOVE 2
#  define WL_SUBSURFACE_PLACE_BELOW 3
#  define WL_SUBSURFACE_SET_SYNC 4
#  define WL_SUBSURFACE_SET_DESYNC 5

static inline struct wl_subsurface* wl_subcompositor_get_subsurface(
    struct wl_subcompositor* wl_subcompositor, struct wl_surface* surface,
    struct wl_surface* parent) {
  struct wl_proxy* id;

  id = wl_proxy_marshal_constructor(
      (struct wl_proxy*)wl_subcompositor, WL_SUBCOMPOSITOR_GET_SUBSURFACE,
      &wl_subsurface_interface, NULL, surface, parent);

  return (struct wl_subsurface*)id;
}

static inline void wl_subsurface_set_position(
    struct wl_subsurface* wl_subsurface, int32_t x, int32_t y) {
  wl_proxy_marshal((struct wl_proxy*)wl_subsurface, WL_SUBSURFACE_SET_POSITION,
                   x, y);
}

static inline void wl_subsurface_set_desync(
    struct wl_subsurface* wl_subsurface) {
  wl_proxy_marshal((struct wl_proxy*)wl_subsurface, WL_SUBSURFACE_SET_DESYNC);
}

static inline void wl_subsurface_destroy(struct wl_subsurface* wl_subsurface) {
  wl_proxy_marshal((struct wl_proxy*)wl_subsurface, WL_SUBSURFACE_DESTROY);

  wl_proxy_destroy((struct wl_proxy*)wl_subsurface);
}
#endif

#ifndef WL_SURFACE_DAMAGE_BUFFER
#  define WL_SURFACE_DAMAGE_BUFFER 9

static inline void wl_surface_damage_buffer(struct wl_surface* wl_surface,
                                            int32_t x, int32_t y, int32_t width,
                                            int32_t height) {
  wl_proxy_marshal((struct wl_proxy*)wl_surface, WL_SURFACE_DAMAGE_BUFFER, x, y,
                   width, height);
}
#endif

#ifndef WL_POINTER_AXIS_ENUM
#  define WL_POINTER_AXIS_ENUM
enum wl_pointer_axis {
  WL_POINTER_AXIS_VERTICAL_SCROLL = 0,
  WL_POINTER_AXIS_HORIZONTAL_SCROLL = 1,
};
#endif /* WL_POINTER_AXIS_ENUM */

#ifndef WL_POINTER_AXIS_SOURCE_ENUM
#  define WL_POINTER_AXIS_SOURCE_ENUM
enum wl_pointer_axis_source {
  WL_POINTER_AXIS_SOURCE_WHEEL = 0,
  WL_POINTER_AXIS_SOURCE_FINGER = 1,
  WL_POINTER_AXIS_SOURCE_CONTINUOUS = 2,
  WL_POINTER_AXIS_SOURCE_WHEEL_TILT = 3,
};
#  define WL_POINTER_AXIS_SOURCE_WHEEL_TILT_SINCE_VERSION 6
#endif /* WL_POINTER_AXIS_SOURCE_ENUM */

#ifndef WL_POINTER_AXIS_RELATIVE_DIRECTION_ENUM
#  define WL_POINTER_AXIS_RELATIVE_DIRECTION_ENUM
enum wl_pointer_axis_relative_direction {
  WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL = 0,
  WL_POINTER_AXIS_RELATIVE_DIRECTION_INVERTED = 1,
};
#endif /* WL_POINTER_AXIS_RELATIVE_DIRECTION_ENUM */

#ifndef WL_POINTER_AXIS_SOURCE_ENUM
#  define WL_POINTER_AXIS_SOURCE_ENUM
enum wl_pointer_axis_source {
  WL_POINTER_AXIS_SOURCE_WHEEL = 0,
  WL_POINTER_AXIS_SOURCE_FINGER = 1,
  WL_POINTER_AXIS_SOURCE_CONTINUOUS = 2,
  WL_POINTER_AXIS_SOURCE_WHEEL_TILT = 3,
};
#  define WL_POINTER_AXIS_SOURCE_WHEEL_TILT_SINCE_VERSION 6
#endif /* WL_POINTER_AXIS_SOURCE_ENUM */

struct moz_wl_pointer_listener {
  void (*enter)(void* data, struct wl_pointer* wl_pointer, uint32_t serial,
                struct wl_surface* surface, wl_fixed_t surface_x,
                wl_fixed_t surface_y);
  void (*leave)(void* data, struct wl_pointer* wl_pointer, uint32_t serial,
                struct wl_surface* surface);
  void (*motion)(void* data, struct wl_pointer* wl_pointer, uint32_t time,
                 wl_fixed_t surface_x, wl_fixed_t surface_y);
  void (*button)(void* data, struct wl_pointer* wl_pointer, uint32_t serial,
                 uint32_t time, uint32_t button, uint32_t state);
  void (*axis)(void* data, struct wl_pointer* wl_pointer, uint32_t time,
               uint32_t axis, wl_fixed_t value);
  /**
   * end of a pointer event sequence
   *
   * Indicates the end of a set of events that logically belong
   * together. A client is expected to accumulate the data in all
   * events within the frame before proceeding.
   *
   * All wl_pointer events before a wl_pointer.frame event belong
   * logically together. For example, in a diagonal scroll motion the
   * compositor will send an optional wl_pointer.axis_source event,
   * two wl_pointer.axis events (horizontal and vertical) and finally
   * a wl_pointer.frame event. The client may use this information to
   * calculate a diagonal vector for scrolling.
   *
   * When multiple wl_pointer.axis events occur within the same
   * frame, the motion vector is the combined motion of all events.
   * When a wl_pointer.axis and a wl_pointer.axis_stop event occur
   * within the same frame, this indicates that axis movement in one
   * axis has stopped but continues in the other axis. When multiple
   * wl_pointer.axis_stop events occur within the same frame, this
   * indicates that these axes stopped in the same instance.
   *
   * A wl_pointer.frame event is sent for every logical event group,
   * even if the group only contains a single wl_pointer event.
   * Specifically, a client may get a sequence: motion, frame,
   * button, frame, axis, frame, axis_stop, frame.
   *
   * The wl_pointer.enter and wl_pointer.leave events are logical
   * events generated by the compositor and not the hardware. These
   * events are also grouped by a wl_pointer.frame. When a pointer
   * moves from one surface to another, a compositor should group the
   * wl_pointer.leave event within the same wl_pointer.frame.
   * However, a client must not rely on wl_pointer.leave and
   * wl_pointer.enter being in the same wl_pointer.frame.
   * Compositor-specific policies may require the wl_pointer.leave
   * and wl_pointer.enter event being split across multiple
   * wl_pointer.frame groups.
   * @since 5
   */
  void (*frame)(void* data, struct wl_pointer* wl_pointer);
  void (*axis_source)(void* data, struct wl_pointer* wl_pointer,
                      uint32_t axis_source);
  void (*axis_stop)(void* data, struct wl_pointer* wl_pointer, uint32_t time,
                    uint32_t axis);
  void (*axis_discrete)(void* data, struct wl_pointer* wl_pointer,
                        uint32_t axis, int32_t discrete);
  void (*axis_value120)(void* data, struct wl_pointer* wl_pointer,
                        uint32_t axis, int32_t value120);
  void (*axis_relative_direction)(void* data, struct wl_pointer* wl_pointer,
                                  uint32_t axis, uint32_t direction);
};

#ifndef WL_POINTER_RELEASE_SINCE_VERSION
#  define WL_POINTER_RELEASE_SINCE_VERSION 3
#endif

#ifndef WL_POINTER_AXIS_VALUE120_SINCE_VERSION
#  define WL_POINTER_AXIS_VALUE120_SINCE_VERSION 8
#endif

#ifndef WL_FIXES_DESTROY_SINCE_VERSION
#  define WL_FIXES_DESTROY_SINCE_VERSION 1

#  define WL_FIXES_DESTROY 0
#  define WL_FIXES_DESTROY_REGISTRY 1
#  define WL_FIXES_DESTROY_SINCE_VERSION 1
#  define WL_FIXES_DESTROY_REGISTRY_SINCE_VERSION 1

static inline void wl_fixes_set_user_data(struct wl_fixes* wl_fixes,
                                          void* user_data) {
  wl_proxy_set_user_data((struct wl_proxy*)wl_fixes, user_data);
}

static inline void* wl_fixes_get_user_data(struct wl_fixes* wl_fixes) {
  return wl_proxy_get_user_data((struct wl_proxy*)wl_fixes);
}

static inline uint32_t wl_fixes_get_version(struct wl_fixes* wl_fixes) {
  return wl_proxy_get_version((struct wl_proxy*)wl_fixes);
}

static inline void wl_fixes_destroy(struct wl_fixes* wl_fixes) {
  wl_proxy_marshal_flags((struct wl_proxy*)wl_fixes, WL_FIXES_DESTROY, NULL,
                         wl_proxy_get_version((struct wl_proxy*)wl_fixes),
                         WL_MARSHAL_FLAG_DESTROY);
}

static inline void wl_fixes_destroy_registry(struct wl_fixes* wl_fixes,
                                             struct wl_registry* registry) {
  wl_proxy_marshal_flags((struct wl_proxy*)wl_fixes, WL_FIXES_DESTROY_REGISTRY,
                         NULL, wl_proxy_get_version((struct wl_proxy*)wl_fixes),
                         0, registry);
}
#endif

#ifndef WL_FIXES_ERROR_ENUM
#  define WL_FIXES_ERROR_ENUM
enum wl_fixes_error {
  WL_FIXES_ERROR_INVALID_ACK_REMOVE = 0,
};
#endif /* WL_FIXES_ERROR_ENUM */

#ifndef WL_FIXES_ACK_GLOBAL_REMOVE_SINCE_VERSION
#  define WL_FIXES_ACK_GLOBAL_REMOVE_SINCE_VERSION 2

#  define WL_FIXES_ACK_GLOBAL_REMOVE 2

static inline void wl_fixes_ack_global_remove(struct wl_fixes* wl_fixes,
                                              struct wl_registry* registry,
                                              uint32_t name) {
  wl_proxy_marshal_flags((struct wl_proxy*)wl_fixes, WL_FIXES_ACK_GLOBAL_REMOVE,
                         NULL, wl_proxy_get_version((struct wl_proxy*)wl_fixes),
                         0, registry, name);
}
#endif

#ifndef WL_OUTPUT_RELEASE_SINCE_VERSION
#  define WL_OUTPUT_RELEASE_SINCE_VERSION 3

#  define WL_OUTPUT_RELEASE 0

static inline void wl_output_release(struct wl_output* wl_output) {
  wl_proxy_marshal_flags((struct wl_proxy*)wl_output, WL_OUTPUT_RELEASE, NULL,
                         wl_proxy_get_version((struct wl_proxy*)wl_output),
                         WL_MARSHAL_FLAG_DESTROY);
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* MozWayland_h_ */
