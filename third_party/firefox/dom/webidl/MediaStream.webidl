/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The origins of this IDL file are
 * http://dev.w3.org/2011/webrtc/editor/getusermedia.html
 *
 * Copyright � 2012 W3C� (MIT, ERCIM, Keio), All Rights Reserved. W3C
 * liability, trademark and document use rules apply.
 */

[Exposed=Window]
interface MediaStream : EventTarget {
    [Throws]
    constructor();
    [Throws]
    constructor(MediaStream stream);
    [Throws]
    constructor(sequence<MediaStreamTrack> tracks);

    readonly    attribute DOMString    id;
    sequence<MediaStreamTrack> getAudioTracks ();
    sequence<MediaStreamTrack> getVideoTracks ();
    sequence<MediaStreamTrack> getTracks ();
    MediaStreamTrack?          getTrackById (DOMString trackId);
    undefined                  addTrack (MediaStreamTrack track);
    undefined                  removeTrack (MediaStreamTrack track);
    MediaStream                clone ();
    readonly    attribute boolean      active;
                attribute EventHandler onaddtrack;
                attribute EventHandler onremovetrack;

    [ChromeOnly, NewObject]
    static Promise<long> countUnderlyingStreams();

    // Webrtc allows the remote side to name a stream whatever it wants, and we
    // need to surface this to content.
    [ChromeOnly]
    undefined assignId(DOMString id);
};
