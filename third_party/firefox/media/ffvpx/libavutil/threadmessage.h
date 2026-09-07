/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with FFmpeg; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVUTIL_THREADMESSAGE_H
#define AVUTIL_THREADMESSAGE_H

typedef struct AVThreadMessageQueue AVThreadMessageQueue;

typedef enum AVThreadMessageFlags {

    AV_THREAD_MESSAGE_NONBLOCK = 1,

} AVThreadMessageFlags;

int av_thread_message_queue_alloc(AVThreadMessageQueue **mq,
                                  unsigned nelem,
                                  unsigned elsize);

void av_thread_message_queue_free(AVThreadMessageQueue **mq);

int av_thread_message_queue_send(AVThreadMessageQueue *mq,
                                 void *msg,
                                 unsigned flags);

int av_thread_message_queue_recv(AVThreadMessageQueue *mq,
                                 void *msg,
                                 unsigned flags);

void av_thread_message_queue_set_err_send(AVThreadMessageQueue *mq,
                                          int err);

void av_thread_message_queue_set_err_recv(AVThreadMessageQueue *mq,
                                          int err);

void av_thread_message_queue_set_free_func(AVThreadMessageQueue *mq,
                                           void (*free_func)(void *msg));

int av_thread_message_queue_nb_elems(AVThreadMessageQueue *mq);

void av_thread_message_flush(AVThreadMessageQueue *mq);

#endif /* AVUTIL_THREADMESSAGE_H */
