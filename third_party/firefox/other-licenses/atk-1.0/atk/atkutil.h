/* ATK -  Accessibility Toolkit
 * Copyright 2001 Sun Microsystems Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef __ATK_UTIL_H__
#define __ATK_UTIL_H__

#include <atk/atkobject.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define ATK_TYPE_UTIL                   (atk_util_get_type ())
#define ATK_IS_UTIL(obj)                G_TYPE_CHECK_INSTANCE_TYPE ((obj), ATK_TYPE_UTIL)
#define ATK_UTIL(obj)                   G_TYPE_CHECK_INSTANCE_CAST ((obj), ATK_TYPE_UTIL, AtkUtil)
#define ATK_UTIL_CLASS(klass)                   (G_TYPE_CHECK_CLASS_CAST ((klass), ATK_TYPE_UTIL, AtkUtilClass))
#define ATK_IS_UTIL_CLASS(klass)                (G_TYPE_CHECK_CLASS_TYPE ((klass), ATK_TYPE_UTIL))
#define ATK_UTIL_GET_CLASS(obj)                 (G_TYPE_INSTANCE_GET_CLASS ((obj), ATK_TYPE_UTIL, AtkUtilClass))


#ifndef _TYPEDEF_ATK_UTIL_
#define _TYPEDEF_ATK_UTIL_
typedef struct _AtkUtil      AtkUtil;
typedef struct _AtkUtilClass AtkUtilClass;
typedef struct _AtkKeyEventStruct AtkKeyEventStruct;
#endif

typedef void  (*AtkEventListener) (AtkObject* obj);
typedef void  (*AtkEventListenerInit) (void);
typedef gint  (*AtkKeySnoopFunc)  (AtkKeyEventStruct *event,
				   gpointer func_data);

struct _AtkKeyEventStruct {
  gint type;
  guint state;
  guint keyval;
  gint length;
  gchar *string;
  guint16 keycode;
  guint32 timestamp;	
};

typedef enum
{
  ATK_KEY_EVENT_PRESS,
  ATK_KEY_EVENT_RELEASE,
  ATK_KEY_EVENT_LAST_DEFINED
} AtkKeyEventType;

struct _AtkUtil
{
  GObject parent;
};

struct _AtkUtilClass
{
   GObjectClass parent;
   guint        (* add_global_event_listener)    (GSignalEmissionHook listener,
						  const gchar        *event_type);
   void         (* remove_global_event_listener) (guint               listener_id);
   guint	(* add_key_event_listener) 	 (AtkKeySnoopFunc     listener,
						  gpointer data);
   void         (* remove_key_event_listener)    (guint               listener_id);
   AtkObject*   (* get_root)                     (void);
   G_CONST_RETURN gchar* (* get_toolkit_name)    (void);
   G_CONST_RETURN gchar* (* get_toolkit_version) (void);
};
GType atk_util_get_type (void);

typedef enum {
  ATK_XY_SCREEN,
  ATK_XY_WINDOW,
  ATK_XY_PARENT
}AtkCoordType;

guint    atk_add_focus_tracker     (AtkEventListener      focus_tracker);

void     atk_remove_focus_tracker  (guint                tracker_id);

void     atk_focus_tracker_init    (AtkEventListenerInit  init);

void     atk_focus_tracker_notify  (AtkObject            *object);

guint	atk_add_global_event_listener (GSignalEmissionHook listener,
				       const gchar        *event_type);

void	atk_remove_global_event_listener (guint listener_id);

guint	atk_add_key_event_listener (AtkKeySnoopFunc listener, gpointer data);

void	atk_remove_key_event_listener (guint listener_id);

AtkObject* atk_get_root(void);

AtkObject* atk_get_focus_object (void);

G_CONST_RETURN gchar *atk_get_toolkit_name (void);

G_CONST_RETURN gchar *atk_get_toolkit_version (void);

#ifdef __cplusplus
}
#endif /* __cplusplus */


#endif /* __ATK_UTIL_H__ */
