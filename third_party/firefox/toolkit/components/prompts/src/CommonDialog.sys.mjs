/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};
ChromeUtils.defineESModuleGetters(lazy, {
  EnableDelayHelper: "resource://gre/modules/PromptUtils.sys.mjs",
});

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

export class CommonDialog {
  constructor(args, ui) {
    this.args = args;
    this.ui = ui;
    this.initialFocusPromise = new Promise(resolve => {
      this.initialFocusResolver = resolve;
    });
  }

  static DEFAULT_APP_ICON_CSS = `image-set(url("chrome://branding/content/icon16.png") 1x,
    url("chrome://branding/content/icon32.png") 2x,
    url("chrome://branding/content/icon64.png") 4x)`;

  args = null;
  ui = null;

  hasInputField = true;
  numButtons = undefined;
  iconClass = undefined;
  soundID = undefined;
  focusTimer = null;
  initialFocusPromise = null;
  initialFocusResolver = null;

  async onLoad(commonDialogEl) {
    let isEmbedded =
      !!commonDialogEl.documentGlobal.docShell.chromeEventHandler;

    switch (this.args.promptType) {
      case "alert":
      case "alertCheck":
        this.hasInputField = false;
        this.numButtons = 1;
        this.iconClass = ["alert-icon"];
        this.soundID = Ci.nsISound.EVENT_ALERT_DIALOG_OPEN;
        break;
      case "confirmCheck":
      case "confirm":
        this.hasInputField = false;
        this.numButtons = 2;
        this.iconClass = ["question-icon"];
        this.soundID = Ci.nsISound.EVENT_CONFIRM_DIALOG_OPEN;
        break;
      case "confirmEx":
        var numButtons = 0;
        if (this.args.button0Label) {
          numButtons++;
        }
        if (this.args.button1Label) {
          numButtons++;
        }
        if (this.args.button2Label) {
          numButtons++;
        }
        if (this.args.button3Label) {
          numButtons++;
        }
        if (numButtons == 0 && !this.args.allowNoButtons) {
          throw new Error(
            "A dialog with no buttons requires the allowNoButtons argument"
          );
        }
        this.numButtons = numButtons;
        this.hasInputField = false;
        this.iconClass = ["question-icon"];
        this.soundID = Ci.nsISound.EVENT_CONFIRM_DIALOG_OPEN;
        break;
      case "prompt":
        this.numButtons = 2;
        this.iconClass = ["question-icon"];
        this.soundID = Ci.nsISound.EVENT_PROMPT_DIALOG_OPEN;
        this.initTextbox("login", this.args.value);
        this.ui.loginLabel.setAttribute("value", "");
        this.ui.loginTextbox.setAttribute("aria-labelledby", "infoBody");
        break;
      case "promptUserAndPass":
        this.numButtons = 2;
        this.iconClass = ["authentication-icon", "question-icon"];
        this.soundID = Ci.nsISound.EVENT_PROMPT_DIALOG_OPEN;
        this.initTextbox("login", this.args.user);
        this.initTextbox("password1", this.args.pass);
        break;
      case "promptPassword":
        this.numButtons = 2;
        this.iconClass = ["authentication-icon", "question-icon"];
        this.soundID = Ci.nsISound.EVENT_PROMPT_DIALOG_OPEN;
        this.initTextbox("password1", this.args.pass);
        this.ui.password1Label.setAttribute("value", "");
        break;
      default:
        console.error(
          "commonDialog opened for unknown type: ",
          this.args.promptType
        );
        throw new Error("unknown dialog type");
    }

    commonDialogEl.setAttribute("windowtype", "prompt:" + this.args.promptType);

    let title = this.args.title;
    let infoTitle = this.ui.infoTitle;
    infoTitle.appendChild(infoTitle.ownerDocument.createTextNode(title));

    infoTitle.hidden = !(AppConstants.platform === "macosx" || isEmbedded);

    commonDialogEl.ownerDocument.title = title;

    switch (this.numButtons) {
      case 4:
        this.setLabelForNode(this.ui.button3, this.args.button3Label);
        this.ui.button3.hidden = false;
      // fall through
      case 3:
        this.setLabelForNode(this.ui.button2, this.args.button2Label);
        this.ui.button2.hidden = false;
      // fall through
      case 2:
        if (this.args.button1Label) {
          this.setLabelForNode(this.ui.button1, this.args.button1Label);
        }
        break;
      case 0:
        this.ui.button0.hidden = true;
      // fall through
      case 1:
        this.ui.button1.hidden = true;
        break;
    }
    if (this.args.button0Label) {
      this.setLabelForNode(this.ui.button0, this.args.button0Label);
    }

    let croppedMessage = "";
    if (this.args.text) {
      croppedMessage = this.args.text.substr(0, 10000);
      if (this.ui.infoRow) {
        this.ui.infoRow.hidden = false;
      }
    }
    let infoBody = this.ui.infoBody;
    infoBody.appendChild(infoBody.ownerDocument.createTextNode(croppedMessage));

    let label = this.args.checkLabel;
    if (label) {
      this.ui.checkboxContainer.hidden = false;
      this.ui.checkboxContainer.clientTop; 
      this.setLabelForNode(this.ui.checkbox, label);
      this.ui.checkbox.checked = this.args.checked;
    }

    let icon = this.ui.infoIcon;
    if (icon) {
      this.iconClass.forEach(el => icon.classList.add(el));
    }

    this.args.ok = false;
    this.args.buttonNumClicked = 1;

    let b = this.args.defaultButtonNum || 0;
    commonDialogEl.defaultButton = ["accept", "cancel", "extra1", "extra2"][b];

    if (!isEmbedded && !this.ui.promptContainer?.hidden) {
      this.setDefaultFocus(true);
    }

    if (this.args.enableDelay) {
      this.delayHelper = new lazy.EnableDelayHelper({
        disableDialog: () => this.setButtonsEnabledState(false),
        enableDialog: () => this.setButtonsEnabledState(true),
        focusTarget: this.ui.focusTarget,
      });
    }

    try {
      if (this.soundID && !this.args.openedWithTabDialog) {
        Cc["@mozilla.org/sound;1"]
          .getService(Ci.nsISound)
          .playEventSound(this.soundID);
      }
    } catch (e) {
      console.error("Couldn't play common dialog event sound: ", e);
    }

    if (isEmbedded) {
      await this.initialFocusPromise;
    }
    Services.obs.notifyObservers(this.ui.prompt, "common-dialog-loaded");
  }

  setLabelForNode(aNode, aLabel) {

    var accessKey = null;
    if (/ *\(\&([^&])\)(:?)$/.test(aLabel)) {
      aLabel = RegExp.leftContext + RegExp.$2;
      accessKey = RegExp.$1;
    } else if (/^([^&]*)\&(([^&]).*$)/.test(aLabel)) {
      aLabel = RegExp.$1 + RegExp.$2;
      accessKey = RegExp.$3;
    }

    aLabel = aLabel.replace(/\&\&/g, "&");
    aNode.label = aLabel;

    if (accessKey) {
      aNode.accessKey = accessKey;
    }
  }

  initTextbox(aName, aValue) {
    this.ui[aName + "Container"].hidden = false;
    this.ui[aName + "Textbox"].setAttribute(
      "value",
      aValue !== null ? aValue : ""
    );
  }

  setButtonsEnabledState(enabled) {
    this.ui.button0.disabled = !enabled;
    this.ui.button2.disabled = !enabled;
    this.ui.button3.disabled = !enabled;
  }

  setDefaultFocus(isInitialLoad) {
    let b = this.args.defaultButtonNum || 0;
    let button = this.ui["button" + b];

    if (!this.hasInputField) {
      let isOSX = "nsILocalFileMac" in Ci;
      if (isOSX && !(this.ui.infoRow && this.ui.infoRow.hidden)) {
        this.ui.infoBody.focus();
      } else {
        button.focus({ focusVisible: false });
      }
    } else if (this.args.promptType == "promptPassword") {
      if (isInitialLoad) {
        this.ui.password1Textbox.select();
      } else {
        this.ui.password1Textbox.focus();
      }
    } else if (isInitialLoad) {
      this.ui.loginTextbox.select();
    } else {
      this.ui.loginTextbox.focus();
    }

    if (isInitialLoad) {
      this.initialFocusResolver();
    }
  }

  onCheckbox() {
    this.args.checked = this.ui.checkbox.checked;
  }

  onButton0() {
    this.args.promptActive = false;
    this.args.ok = true;
    this.args.buttonNumClicked = 0;

    let username = this.ui.loginTextbox.value;
    let password = this.ui.password1Textbox.value;

    switch (this.args.promptType) {
      case "prompt":
        this.args.value = username;
        break;
      case "promptUserAndPass":
        this.args.user = username;
        this.args.pass = password;
        break;
      case "promptPassword":
        this.args.pass = password;
        break;
    }
  }

  onButton1() {
    this.args.promptActive = false;
    this.args.buttonNumClicked = 1;
  }

  onButton2() {
    this.args.promptActive = false;
    this.args.buttonNumClicked = 2;
  }

  onButton3() {
    this.args.promptActive = false;
    this.args.buttonNumClicked = 3;
  }

  abortPrompt() {
    this.args.promptActive = false;
    this.args.promptAborted = true;
  }
}
