/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineLazyGetter(lazy, "logConsole", function () {
  return console.createInstance({
    prefix: "BackupUIChild",
    maxLogLevel: Services.prefs.getBoolPref("browser.backup.log", false)
      ? "Debug"
      : "Warn",
  });
});

export class BackupUIChild extends JSWindowActorChild {
  #inittedWidgets = new WeakSet();

  #findWidget(nodeName) {
    let widgets = ChromeUtils.nondeterministicGetWeakSetKeys(
      this.#inittedWidgets
    );
    for (let widget of widgets) {
      if (widget.isConnected && widget.nodeName == nodeName) {
        return widget;
      }
    }
    return null;
  }

  async handleEvent(event) {
    if (event.type == "BackupUI:InitWidget") {
      this.#inittedWidgets.add(event.target);
      this.sendAsyncMessage("RequestState");
    } else if (event.type == "BackupUI:TriggerCreateBackup") {
      let result = await this.sendQuery("TriggerCreateBackup", event.detail);

      if (!result.success) {
        event.target.backupErrorCode = result.errorCode;
      }
    } else if (event.type == "BackupUI:EnableScheduledBackups") {
      const target = event.target;

      const result = await this.sendQuery(
        "EnableScheduledBackups",
        event.detail
      );
      if (result.success) {
        target.close();
      } else {
        target.enableBackupErrorCode = result.errorCode;
      }
    } else if (event.type == "BackupUI:DisableScheduledBackups") {
      const target = event.target;

      this.sendAsyncMessage("DisableScheduledBackups", event.detail);

      target.close();
    } else if (event.type == "BackupUI:ShowFilepicker") {
      let targetNodeName = event.composedTarget.nodeName;
      let { path, filename, iconURL } = await this.sendQuery("ShowFilepicker", {
        win: event.detail?.win,
        filter: event.detail?.filter,
        existingBackupPath: event.detail?.existingBackupPath,
        alsoDeleteLastBackup: event.detail?.alsoDeleteLastBackup,
      });

      let widget = this.#findWidget(targetNodeName);
      if (widget) {
        const win = widget.documentGlobal;
        const detail = Cu.cloneInto({ path, filename, iconURL }, win, {
          wrapReflectors: true,
        });
        const evt = new win.CustomEvent("BackupUI:SelectNewFilepickerPath", {
          bubbles: true,
          composed: true,
          detail,
        });
        widget.dispatchEvent(evt);
      }
    } else if (event.type == "BackupUI:GetBackupFileInfo") {
      let { backupFile } = event.detail;
      this.sendAsyncMessage("GetBackupFileInfo", {
        backupFile,
      });
    } else if (event.type == "BackupUI:RestoreFromBackupFile") {
      let { backupPassword, restoreType, source } = event.detail;
      let result = await this.sendQuery("RestoreFromBackupFile", {
        backupPassword,
        restoreType,
        source,
      });

      if (result.success) {
        event.target.restoreFromBackupDialogEl?.close();

        this.sendAsyncMessage("QuitCurrentProfile");
      }
    } else if (event.type == "BackupUI:RestoreFromBackupChooseFile") {
      this.sendAsyncMessage("RestoreFromBackupChooseFile");
    } else if (event.type == "BackupUI:EnableEncryption") {
      const target = event.target;

      const result = await this.sendQuery("EnableEncryption", event.detail);
      if (result.success) {
        target.close();
      } else {
        target.enableEncryptionErrorCode = result.errorCode;
      }
    } else if (event.type == "BackupUI:DisableEncryption") {
      const target = event.target;

      const result = await this.sendQuery("DisableEncryption", event.detail);
      if (result.success) {
        target.close();
      } else {
        target.disableEncryptionErrorCode = result.errorCode;
      }
    } else if (event.type == "BackupUI:ShowBackupLocation") {
      this.sendAsyncMessage("ShowBackupLocation");
    } else if (event.type == "BackupUI:SetEmbeddedComponentPersistentData") {
      this.sendAsyncMessage("SetEmbeddedComponentPersistentData", event.detail);
    } else if (event.type == "BackupUI:FlushEmbeddedComponentPersistentData") {
      this.sendAsyncMessage("FlushEmbeddedComponentPersistentData");
    } else if (event.type == "BackupUI:ErrorBarDismissed") {
      this.sendAsyncMessage("ErrorBarDismissed");
    } else if (event.type == "BackupUI:FindBackupsInWellKnownLocations") {
      this.sendAsyncMessage("FindBackupsInWellKnownLocations", event.detail);
    } else if (event.type == "BackupUI:ProbeDefaultBackupDir") {
      let targetNodeName = event.composedTarget.nodeName;
      let parentDirPath = event.detail?.parentDirPath;
      let readAccessGranted = false;
      try {
        ({ readAccessGranted } = await this.sendQuery("ProbeDefaultBackupDir", {
          parentDirPath,
        }));
      } catch (e) {
        lazy.logConsole.error("ProbeDefaultBackupDir failed:", e);
      }

      let widget = this.#findWidget(targetNodeName);
      if (widget) {
        const win = widget.documentGlobal;
        const detail = Cu.cloneInto({ readAccessGranted }, win, {
          wrapReflectors: true,
        });
        widget.dispatchEvent(
          new win.CustomEvent("BackupUI:DefaultDirProbeResult", {
            bubbles: false,
            detail,
          })
        );
      }
    } else if (event.type == "BackupUI:PrepareRestoreDialog") {
      let targetNodeName = event.composedTarget.nodeName;
      try {
        await this.sendQuery("PrepareRestoreDialog", event.detail);
      } catch (e) {
        lazy.logConsole.error("PrepareRestoreDialog failed:", e);
      }

      let widget = this.#findWidget(targetNodeName);
      if (widget) {
        widget.dispatchEvent(
          new widget.documentGlobal.CustomEvent("BackupUI:RestoreDialogReady", {
            bubbles: false,
          })
        );
      }
    }
  }

  receiveMessage(message) {
    if (message.name == "StateUpdate") {
      let widgets = ChromeUtils.nondeterministicGetWeakSetKeys(
        this.#inittedWidgets
      );
      for (let widget of widgets) {
        if (!widget.isConnected || !widget.documentGlobal) {
          continue;
        }

        const state = Cu.cloneInto(message.data.state, widget.documentGlobal);

        const waivedWidget = Cu.waiveXrays(widget);
        waivedWidget.backupServiceState = state;
        widget.dispatchEvent(
          new this.contentWindow.CustomEvent("BackupUI:StateWasUpdated", {
            bubbles: true,
            composed: true,
            detail: { state },
          })
        );
      }
    }
  }
}
