/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */


import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

let JSPROCESSACTORS = {
  AsyncPrefs: {
    parent: {
      esModuleURI: "resource://gre/modules/AsyncPrefs.sys.mjs",
    },
    child: {
      esModuleURI: "resource://gre/modules/AsyncPrefs.sys.mjs",
    },
    safeForUntrustedWebProcess: true,
  },

  ContentPrefs: {
    parent: {
      esModuleURI: "resource://gre/modules/ContentPrefServiceParent.sys.mjs",
    },
    child: {
      esModuleURI: "resource://gre/modules/ContentPrefServiceChild.sys.mjs",
    },
    safeForUntrustedWebProcess: true,
  },

  HPKEConfigManager: {
    remoteTypes: ["privilegedabout"],
    parent: {
      esModuleURI: "resource://gre/modules/HPKEConfigManager.sys.mjs",
    },
  },

};

let JSWINDOWACTORS = {
  AboutHttpsOnlyError: {
    parent: {
      esModuleURI: "resource://gre/actors/AboutHttpsOnlyErrorParent.sys.mjs",
    },
    child: {
      esModuleURI: "resource://gre/actors/AboutHttpsOnlyErrorChild.sys.mjs",
      events: {
        DOMDocElementInserted: {},
      },
    },
    matches: ["about:httpsonlyerror?*"],
    allFrames: true,
    safeForUntrustedWebProcess: true,
  },

  AboutRestricted: {
    parent: {
      esModuleURI: "resource://gre/actors/AboutRestrictedParent.sys.mjs",
    },
    child: {
      esModuleURI: "resource://gre/actors/AboutRestrictedChild.sys.mjs",
      events: {
        DOMDocElementInserted: {},
      },
    },
    matches: ["about:restricted?*"],
    allFrames: true,
    safeForUntrustedWebProcess: true,
  },

  AudioPlayback: {
    parent: {
      esModuleURI: "resource://gre/actors/AudioPlaybackParent.sys.mjs",
    },

    child: {
      esModuleURI: "resource://gre/actors/AudioPlaybackChild.sys.mjs",
      observers: ["audio-playback"],
    },

    allFrames: true,
    safeForUntrustedWebProcess: true,
  },

  AutoComplete: {
    parent: {
      esModuleURI: "resource://gre/actors/AutoCompleteParent.sys.mjs",
    },

    child: {
      esModuleURI: "resource://gre/actors/AutoCompleteChild.sys.mjs",
    },

    allFrames: true,
    safeForUntrustedWebProcess: true,
  },

  Autoplay: {
    parent: {
      esModuleURI: "resource://gre/actors/AutoplayParent.sys.mjs",
    },

    child: {
      esModuleURI: "resource://gre/actors/AutoplayChild.sys.mjs",
      events: {
        GloballyAutoplayBlocked: {},
      },
    },

    allFrames: true,
    safeForUntrustedWebProcess: true,
  },

  AutoScroll: {
    parent: {
      esModuleURI: "resource://gre/actors/AutoScrollParent.sys.mjs",
    },

    child: {
      esModuleURI: "resource://gre/actors/AutoScrollChild.sys.mjs",
      events: {
        mousedown: { capture: true, mozSystemGroup: true },
      },
    },

    allFrames: true,
    safeForUntrustedWebProcess: true,
  },

  BrowserElement: {
    parent: {
      esModuleURI: "resource://gre/actors/BrowserElementParent.sys.mjs",
    },

    child: {
      esModuleURI: "resource://gre/actors/BrowserElementChild.sys.mjs",
      events: {
        DOMWindowClose: {},
      },
    },

    allFrames: true,
    safeForUntrustedWebProcess: true,
  },

  Controllers: {
    parent: {
      esModuleURI: "resource://gre/actors/ControllersParent.sys.mjs",
    },
    child: {
      esModuleURI: "resource://gre/actors/ControllersChild.sys.mjs",
    },

    allFrames: true,
    safeForUntrustedWebProcess: true,
  },

  FindBar: {
    parent: {
      esModuleURI: "resource://gre/actors/FindBarParent.sys.mjs",
    },
    child: {
      esModuleURI: "resource://gre/actors/FindBarChild.sys.mjs",
      events: {
        keypress: { mozSystemGroup: true },
      },
    },

    allFrames: true,
    messageManagerGroups: ["browsers", "test"],
    safeForUntrustedWebProcess: true,
  },

  Finder: {
    child: {
      esModuleURI: "resource://gre/actors/FinderChild.sys.mjs",
    },

    allFrames: true,
    safeForUntrustedWebProcess: true,
  },

  KeyPressEventModelChecker: {
    child: {
      esModuleURI:
        "resource://gre/actors/KeyPressEventModelCheckerChild.sys.mjs",
      events: {
        CheckKeyPressEventModel: { capture: true, mozSystemGroup: true },
      },
    },

    allFrames: true,
    safeForUntrustedWebProcess: true,
  },

  NetError: {
    parent: {
      esModuleURI: "resource://gre/actors/NetErrorParent.sys.mjs",
    },
    child: {
      esModuleURI: "resource://gre/actors/NetErrorChild.sys.mjs",
      events: {
        DOMDocElementInserted: {},
      },
    },

    matches: ["about:certerror?*", "about:neterror?*"],
    allFrames: true,
    safeForUntrustedWebProcess: true,
  },

  OpenSearchLoader: {
    child: {
      esModuleURI:
        "moz-src:///toolkit/components/search/OpenSearchLoaderChild.sys.mjs",
    },
    matches: ["about:blank"],
    messageManagerGroups: ["opensearch"],
    safeForUntrustedWebProcess: true,
  },

  PopupAndRedirectBlocking: {
    parent: {
      esModuleURI:
        "resource://gre/actors/PopupAndRedirectBlockingParent.sys.mjs",
    },
    child: {
      esModuleURI:
        "resource://gre/actors/PopupAndRedirectBlockingChild.sys.mjs",
      events: {
        DOMPopupBlocked: { capture: true },
        DOMRedirectBlocked: { capture: true },
        pageshow: { createActor: false },
      },
    },
    allFrames: true,
    safeForUntrustedWebProcess: true,
  },

  TLSCertificateBinding: {
    child: {
      esModuleURI: "resource://gre/actors/TLSCertificateBindingChild.sys.mjs",
    },

    messageManagerGroups: ["browsers"],
    safeForUntrustedWebProcess: true,
  },

  ViewSource: {
    child: {
      esModuleURI: "resource://gre/actors/ViewSourceChild.sys.mjs",
    },

    allFrames: true,
    safeForUntrustedWebProcess: true,
  },

  ViewSourcePage: {
    parent: {
      esModuleURI: "resource://gre/actors/ViewSourcePageParent.sys.mjs",
    },
    child: {
      esModuleURI: "resource://gre/actors/ViewSourcePageChild.sys.mjs",
      events: {
        pageshow: { capture: true },
        click: {},
      },
    },

    matches: ["view-source:*"],
    allFrames: true,
    safeForUntrustedWebProcess: true,
  },

  UAWidgets: {
    child: {
      esModuleURI: "resource://gre/actors/UAWidgetsChild.sys.mjs",
      events: {
        UAWidgetSetupOrChange: {},
        UAWidgetTeardown: {},
      },
    },

    includeChrome: true,
    allFrames: true,
    safeForUntrustedWebProcess: true,
  },

  UnselectedTabHover: {
    parent: {
      esModuleURI: "resource://gre/actors/UnselectedTabHoverParent.sys.mjs",
    },
    child: {
      esModuleURI: "resource://gre/actors/UnselectedTabHoverChild.sys.mjs",
      events: {
        "UnselectedTabHover:Enable": {},
        "UnselectedTabHover:Disable": {},
      },
    },

    allFrames: true,
    safeForUntrustedWebProcess: true,
  },
};

if (AppConstants.platform != "android") {
  JSWINDOWACTORS.Select = {
    parent: {
      esModuleURI: "resource://gre/actors/SelectParent.sys.mjs",
    },

    child: {
      esModuleURI: "resource://gre/actors/SelectChild.sys.mjs",
      events: {
        mozshowdropdown: {},
        "mozshowdropdown-sourcetouch": {},
        mozhidedropdown: { mozSystemGroup: true },
      },
    },

    includeChrome: true,
    allFrames: true,
    safeForUntrustedWebProcess: true,
  };

  JSWINDOWACTORS.DateTimePicker = {
    parent: {
      esModuleURI: "moz-src:///toolkit/actors/DateTimePickerParent.sys.mjs",
    },

    child: {
      esModuleURI: "moz-src:///toolkit/actors/DateTimePickerChild.sys.mjs",
      events: {
        MozOpenDateTimePicker: {},
        MozCloseDateTimePicker: {},
      },
    },

    includeChrome: true,
    allFrames: true,
    safeForUntrustedWebProcess: true,
  };

  JSWINDOWACTORS.ColorPicker = {
    parent: {
      esModuleURI: "moz-src:///toolkit/actors/ColorPickerParent.sys.mjs",
    },

    child: {
      esModuleURI: "moz-src:///toolkit/actors/ColorPickerChild.sys.mjs",
      events: {
        MozOpenColorPicker: {},
        MozCloseColorPicker: {},
      },
    },

    includeChrome: true,
    allFrames: true,
    safeForUntrustedWebProcess: true,
  };
}

export var ActorManagerParent = {
  _addActors(actors, kind) {
    let register, unregister;
    switch (kind) {
      case "JSProcessActor":
        register = ChromeUtils.registerProcessActor;
        unregister = ChromeUtils.unregisterProcessActor;
        break;
      case "JSWindowActor":
        register = ChromeUtils.registerWindowActor;
        unregister = ChromeUtils.unregisterWindowActor;
        break;
      default:
        throw new Error("Invalid JSActor kind " + kind);
    }
    for (let [actorName, actor] of Object.entries(actors)) {
      let actorRegistered = false;
      const registerActor = () => {
        if (!actorRegistered) {
          register(actorName, actor);
          actorRegistered = true;
        }
      };
      const unregisterActor = () => {
        if (actorRegistered) {
          unregister(actorName, actor);
          actorRegistered = false;
        }
      };

      if (actor.onAddActor) {
        actor.onAddActor(registerActor, unregisterActor);
        continue;
      }

      if (actor.enablePreference) {
        Services.prefs.addObserver(actor.enablePreference, () => {
          const isEnabled = Services.prefs.getBoolPref(
            actor.enablePreference,
            false
          );
          if (isEnabled) {
            registerActor();
          } else {
            unregisterActor();
          }
          if (actor.onPreferenceChanged) {
            actor.onPreferenceChanged(isEnabled);
          }
        });

        if (!Services.prefs.getBoolPref(actor.enablePreference, false)) {
          continue;
        }
      }

      registerActor();
    }
  },

  addJSProcessActors(actors) {
    this._addActors(actors, "JSProcessActor");
  },
  addJSWindowActors(actors) {
    this._addActors(actors, "JSWindowActor");
  },
};

ActorManagerParent.addJSProcessActors(JSPROCESSACTORS);
ActorManagerParent.addJSWindowActors(JSWINDOWACTORS);
