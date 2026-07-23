/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVCODEC_PACKET_INTERNAL_H
#define AVCODEC_PACKET_INTERNAL_H

#include <stdint.h>

#include "packet.h"

#define AVPACKET_IS_EMPTY(pkt) (!(pkt)->data && !(pkt)->side_data_elems)

typedef struct PacketListEntry {
    struct PacketListEntry *next;
    AVPacket pkt;
} PacketListEntry;

typedef struct PacketList {
    PacketListEntry *head, *tail;
} PacketList;

#define FF_PACKETLIST_FLAG_PREPEND (1 << 0) /**< Prepend created AVPacketList instead of appending */

int avpriv_packet_list_put(PacketList *list, AVPacket *pkt,
                           int (*copy)(AVPacket *dst, const AVPacket *src),
                           int flags);

int avpriv_packet_list_get(PacketList *list, AVPacket *pkt);

void avpriv_packet_list_free(PacketList *list);

int ff_side_data_set_prft(AVPacket *pkt, int64_t timestamp);

#endif // AVCODEC_PACKET_INTERNAL_H
