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

#ifndef __ATK_STREAMABLE_CONTENT_H__
#define __ATK_STREAMABLE_CONTENT_H__

#include <atk/atkobject.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define ATK_TYPE_STREAMABLE_CONTENT           (atk_streamable_content_get_type ())
#define ATK_IS_STREAMABLE_CONTENT(obj)        G_TYPE_CHECK_INSTANCE_TYPE ((obj), ATK_TYPE_STREAMABLE_CONTENT)
#define ATK_STREAMABLE_CONTENT(obj)           G_TYPE_CHECK_INSTANCE_CAST ((obj), ATK_TYPE_STREAMABLE_CONTENT, AtkStreamableContent)
#define ATK_STREAMABLE_CONTENT_GET_IFACE(obj) (G_TYPE_INSTANCE_GET_INTERFACE ((obj), ATK_TYPE_STREAMABLE_CONTENT, AtkStreamableContentIface))

#ifndef _TYPEDEF_ATK_STREAMABLE_CONTENT
#define _TYPEDEF_ATK_STREAMABLE_CONTENT
typedef struct _AtkStreamableContent AtkStreamableContent;
#endif
typedef struct _AtkStreamableContentIface AtkStreamableContentIface;

struct _AtkStreamableContentIface
{
  GTypeInterface parent;

  gint                      (* get_n_mime_types)  (AtkStreamableContent     *streamable);
  G_CONST_RETURN gchar*     (* get_mime_type)     (AtkStreamableContent     *streamable,
                                                   gint                     i);
  GIOChannel*               (* get_stream)        (AtkStreamableContent     *streamable,
                                                   const gchar              *mime_type);

    G_CONST_RETURN  gchar*  (* get_uri)           (AtkStreamableContent     *streamable,
                                                   const gchar              *mime_type);


  AtkFunction               pad1;
  AtkFunction               pad2;
  AtkFunction               pad3;
};
GType                  atk_streamable_content_get_type (void);

gint                   atk_streamable_content_get_n_mime_types (AtkStreamableContent     *streamable);
                                                       
G_CONST_RETURN gchar*  atk_streamable_content_get_mime_type    (AtkStreamableContent     *streamable,
                                                                gint                     i);
GIOChannel*             atk_streamable_content_get_stream       (AtkStreamableContent     *streamable,
                                                                 const gchar              *mime_type);

gchar*                  atk_streamable_content_get_uri          (AtkStreamableContent     *streamable,
                                                                 const gchar              *mime_type);


#ifdef __cplusplus
}
#endif /* __cplusplus */


#endif /* __ATK_STREAMABLE_CONTENT_H__ */
