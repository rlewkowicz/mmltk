/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

{
  const cachedFragments = {
    get editMenuItems() {
      return `
      <menuitem data-l10n-id="text-action-undo" cmd="cmd_undo"></menuitem>
      <menuitem data-l10n-id="text-action-redo" cmd="cmd_redo"></menuitem>
      <menuseparator></menuseparator>
      <menuitem data-l10n-id="text-action-cut" cmd="cmd_cut"></menuitem>
      <menuitem data-l10n-id="text-action-copy" cmd="cmd_copy"></menuitem>
      <menuitem data-l10n-id="text-action-paste" cmd="cmd_paste"></menuitem>
      <menuitem data-l10n-id="text-action-delete" cmd="cmd_delete"></menuitem>
      <menuitem data-l10n-id="text-action-select-all" cmd="cmd_selectAll"></menuitem>
    `;
    },
    get normal() {
      delete this.normal;
      this.normal = MozXULElement.parseXULToFragment(
        `
      <menupopup class="textbox-contextmenu" showservicesmenu="true">
        ${this.editMenuItems}
      </menupopup>
    `
      );
      MozXULElement.insertFTLIfNeeded("toolkit/global/textActions.ftl");
      return this.normal;
    },
  };

  class MozInputBox extends MozXULElement {
    connectedCallback() {
      this._initUI();
    }

    _initUI() {
      if (this.menupopup) {
        this.menupopup.remove();
      }

      this.setAttribute("context", "_child");
      this.appendChild(cachedFragments.normal.cloneNode(true));
      this.menupopup = this.querySelector(".textbox-contextmenu");

      this.menupopup.addEventListener("popupshowing", event => {
        let input = this._input;
        if (document.commandDispatcher.focusedElement != input) {
          input.focus();
        }
        this._doPopupItemEnabling(event);
      });

      this.menupopup.addEventListener("command", event => {
        var cmd = event.originalTarget.getAttribute("cmd");
        if (cmd) {
          this.doCommand(cmd);
          event.stopPropagation();
        }
      });

      this.dispatchEvent(
        new CustomEvent("moz-input-box-rebuilt", {
          bubbles: true,
          composed: false,
        })
      );
    }

    _doPopupItemEnabling(event) {
      let popupNode = event.target;
      var children = popupNode.childNodes;
      for (var i = 0; i < children.length; i++) {
        var command = children[i].getAttribute("cmd");
        if (command) {
          var controller =
            document.commandDispatcher.getControllerForCommand(command);
          var enabled = controller.isCommandEnabled(command);
          if (enabled) {
            children[i].removeAttribute("disabled");
          } else {
            children[i].setAttribute("disabled", "true");
          }
        }
      }
    }

    doCommand(command) {
      var controller =
        document.commandDispatcher.getControllerForCommand(command);
      controller.doCommand(command);
    }

    get _input() {
      return this.querySelector(".textbox-input");
    }
  }

  customElements.define("moz-input-box", MozInputBox);
}
