/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

var gGfxUtils = {
  _isRecording: false,

  toggleWindowRecording() {
    window.windowUtils.setCompositionRecording(!this._isRecording);
    this._isRecording = !this._isRecording;
  },
};
