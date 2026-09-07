/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const BUNDLE_URL = "chrome://global/locale/viewSource.properties";

const MARK_SELECTION_START = "\uFDD0";
const MARK_SELECTION_END = "\uFDEF";

let gNeedsDrawSelection = false;

let gInitialLineNumber = -1;

export class ViewSourcePageChild extends JSWindowActorChild {
  constructor() {
    super();

    ChromeUtils.defineLazyGetter(this, "bundle", function () {
      return Services.strings.createBundle(BUNDLE_URL);
    });
  }

  static setNeedsDrawSelection(value) {
    gNeedsDrawSelection = value;
  }

  static setInitialLineNumber(value) {
    gInitialLineNumber = value;
  }

  receiveMessage(msg) {
    switch (msg.name) {
      case "ViewSource:GoToLine":
        this.goToLine(msg.data.lineNumber);
        break;
    }
    return undefined;
  }

  handleEvent(event) {
    switch (event.type) {
      case "pageshow":
        this.onPageShow(event);
        break;
      case "click":
        this.onClick(event);
        break;
    }
  }

  get selectionController() {
    return this.docShell
      .QueryInterface(Ci.nsIInterfaceRequestor)
      .getInterface(Ci.nsISelectionDisplay)
      .QueryInterface(Ci.nsISelectionController);
  }

  get webBrowserFind() {
    return this.docShell
      .QueryInterface(Ci.nsIInterfaceRequestor)
      .getInterface(Ci.nsIWebBrowserFind);
  }

  onClick(event) {
    let target = event.originalTarget;

    if (!event.isTrusted || event.target.localName != "button") {
      return;
    }

    let errorDoc = target.ownerDocument;

    if (/^about:blocked/.test(errorDoc.documentURI)) {

      if (target == errorDoc.getElementById("goBackButton")) {
        this.sendAsyncMessage("ViewSource:Close");
      }
    }
  }

  onPageShow() {
    if (
      gNeedsDrawSelection &&
      this.document.documentURI.startsWith("view-source:")
    ) {
      gNeedsDrawSelection = false;
      this.drawSelection();
    }

    if (gInitialLineNumber >= 0) {
      this.goToLine(gInitialLineNumber);
      gInitialLineNumber = -1;
    }
  }

  goToLine(lineNumber) {
    let range = this.findLocation(lineNumber);
    if (!range) {
      this.sendAsyncMessage("ViewSource:GoToLine:Failed");
      return;
    }

    let selection = this.document.defaultView.getSelection();
    selection.removeAllRanges();
    selection.addRange(range);

    let selCon = this.selectionController;
    selCon.setDisplaySelection(Ci.nsISelectionController.SELECTION_ON);
    selCon.setCaretVisibilityDuringSelection(true);

    selCon.scrollSelectionIntoView(
      Ci.nsISelectionController.SELECTION_NORMAL,
      Ci.nsISelectionController.SELECTION_WHOLE_SELECTION,
      0
    );

    this.sendAsyncMessage("ViewSource:GoToLine:Success", { lineNumber });
  }

  findLocation(lineNumber) {
    let line = this.document.getElementById(`line${lineNumber}`);
    let range = null;
    if (line) {
      range = this.document.createRange();
      range.setStart(line, 0);
      range.setEndAfter(line, line.childNodes.length);
      return range;
    }

    let pre = this.document.querySelector("pre");
    if (pre.id) {
      return null;
    }
    let curLine = 1;
    let treewalker = this.document.createTreeWalker(
      pre,
      NodeFilter.SHOW_TEXT,
      null
    );

    let found = false;
    for (
      let textNode = treewalker.firstChild();
      textNode && !found;
      textNode = treewalker.nextNode()
    ) {
      let lineArray = textNode.data.split(/\n/);
      let lastLineInNode = curLine + lineArray.length - 1;

      if (lastLineInNode < lineNumber) {
        curLine = lastLineInNode;
        continue;
      }

      for (
        var i = 0, curPos = 0;
        i < lineArray.length;
        curPos += lineArray[i++].length + 1
      ) {
        if (i > 0) {
          curLine++;
        }

        if (curLine == lineNumber && !range) {
          range = this.document.createRange();
          range.setStart(textNode, curPos);

          range.setEndAfter(pre.lastChild);
        } else if (curLine == lineNumber + 1) {
          range.setEnd(textNode, curPos - 1);
          break;
        }
      }
    }

    return range;
  }

  drawSelection() {
    this.document.title = this.bundle.GetStringFromName(
      "viewSelectionSourceTitle"
    );

    var findService = null;
    try {
      findService = Cc["@mozilla.org/find/find_service;1"].getService(
        Ci.nsIFindService
      );
    } catch (e) {}
    if (!findService) {
      return;
    }

    var matchCase = findService.matchCase;
    var entireWord = findService.entireWord;
    var wrapFind = findService.wrapFind;
    var findBackwards = findService.findBackwards;
    var searchString = findService.searchString;
    var replaceString = findService.replaceString;

    var findInst = this.webBrowserFind;
    findInst.matchCase = true;
    findInst.entireWord = false;
    findInst.wrapFind = true;
    findInst.findBackwards = false;

    findInst.searchString = MARK_SELECTION_START;
    var startLength = MARK_SELECTION_START.length;
    findInst.findNext();

    var selection = this.document.defaultView.getSelection();
    if (!selection.rangeCount) {
      return;
    }

    var range = selection.getRangeAt(0);

    var startContainer = range.startContainer;
    var startOffset = range.startOffset;

    findInst.searchString = MARK_SELECTION_END;
    var endLength = MARK_SELECTION_END.length;
    findInst.findNext();

    var endContainer = selection.anchorNode;
    var endOffset = selection.anchorOffset;

    selection.removeAllRanges();

    endContainer.deleteData(endOffset, endLength);
    startContainer.deleteData(startOffset, startLength);
    if (startContainer == endContainer) {
      endOffset -= startLength;
    } 
    range.setEnd(endContainer, endOffset);

    selection.addRange(range);
    try {
      this.selectionController.scrollSelectionIntoView(
        Ci.nsISelectionController.SELECTION_NORMAL,
        Ci.nsISelectionController.SELECTION_ANCHOR_REGION,
        true
      );
    } catch (e) {}

    findService.matchCase = matchCase;
    findService.entireWord = entireWord;
    findService.wrapFind = wrapFind;
    findService.findBackwards = findBackwards;
    findService.searchString = searchString;
    findService.replaceString = replaceString;

    findInst.matchCase = matchCase;
    findInst.entireWord = entireWord;
    findInst.wrapFind = wrapFind;
    findInst.findBackwards = findBackwards;
    findInst.searchString = searchString;
  }
}
