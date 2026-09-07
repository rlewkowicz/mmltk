/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const BUNDLE_URL = "chrome://global/locale/viewSource.properties";

export class ViewSourcePageParent extends JSWindowActorParent {
  constructor() {
    super();

    this.lastLineFound = null;
  }

  receiveMessage(message) {
    let data = message.data;

    switch (message.name) {
      case "ViewSource:PromptAndGoToLine":
        this.promptAndGoToLine();
        break;
      case "ViewSource:GoToLine:Success":
        this.onGoToLineSuccess(data.lineNumber);
        break;
      case "ViewSource:GoToLine:Failed":
        this.onGoToLineFailed();
        break;
    }
  }

  get bundle() {
    if (this._bundle) {
      return this._bundle;
    }
    return (this._bundle = Services.strings.createBundle(BUNDLE_URL));
  }

  promptAndGoToLine() {
    let input = { value: this.lastLineFound };
    let window = Services.wm.getMostRecentWindow(null);

    let ok = Services.prompt.prompt(
      window,
      this.bundle.GetStringFromName("goToLineTitle"),
      this.bundle.GetStringFromName("goToLineText"),
      input,
      null,
      { value: 0 }
    );

    if (!ok) {
      return;
    }

    let line = parseInt(input.value, 10);

    if (!(line > 0)) {
      Services.prompt.alert(
        window,
        this.bundle.GetStringFromName("invalidInputTitle"),
        this.bundle.GetStringFromName("invalidInputText")
      );
      this.promptAndGoToLine();
    } else {
      this.goToLine(line);
    }
  }

  goToLine(lineNumber) {
    this.sendAsyncMessage("ViewSource:GoToLine", { lineNumber });
  }

  onGoToLineSuccess(lineNumber) {
    this.lastLineFound = lineNumber;
  }

  onGoToLineFailed() {
    let window = Services.wm.getMostRecentWindow(null);
    Services.prompt.alert(
      window,
      this.bundle.GetStringFromName("outOfRangeTitle"),
      this.bundle.GetStringFromName("outOfRangeText")
    );
    this.promptAndGoToLine();
  }
}
