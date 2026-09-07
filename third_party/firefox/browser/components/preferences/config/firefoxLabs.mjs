/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

const { ExperimentAPI } = ChromeUtils.importESModule(
  "resource://nimbus/ExperimentAPI.sys.mjs"
);
const { FirefoxLabs } = ChromeUtils.importESModule(
  "resource://nimbus/FirefoxLabs.sys.mjs"
);
import { SettingGroupManager } from "chrome://browser/content/preferences/config/SettingGroupManager.mjs";

SettingGroupManager.registerGroup("firefoxLabsFeatures", {
  items: [],
  hidden: true,
});

let firefoxLabs = null;
let featuresContainer = null;
let observerAdded = false;

function onNimbusUpdate(_event, { slug, active }) {
  if (firefoxLabs?.get(slug)) {
    document.getElementById(slug).checked = active;
  }
}

async function onCheckboxChanged(event) {
  let target = event.target;
  let slug = target.dataset.nimbusSlug;
  let branchSlug = target.dataset.nimbusBranchSlug;
  let enrolling = !(ExperimentAPI.manager.store.get(slug)?.active ?? false);

  let shouldRestart = false;
  if (firefoxLabs.get(slug).requiresRestart) {
    let buttonIndex = await window.confirmRestartPrompt(
      enrolling,
      1,
      true,
      false
    );
    shouldRestart = buttonIndex === window.CONFIRM_RESTART_PROMPT_RESTART_NOW;

    if (!shouldRestart) {
      target.checked = false;
      return;
    }
  }

  target.disabled = true;

  if (enrolling) {
    await firefoxLabs.enroll(slug, branchSlug);
  } else {
    firefoxLabs.unenroll(slug);
  }

  target.disabled = false;

  if (shouldRestart) {
    Services.startup.quit(
      Ci.nsIAppStartup.eAttemptQuit | Ci.nsIAppStartup.eRestart
    );
  }
}

function resetAllFeatures() {
  for (let optIn of firefoxLabs.all()) {
    let enrolled = ExperimentAPI.manager.store.get(optIn.slug)?.active ?? false;
    if (enrolled) {
      firefoxLabs.unenroll(optIn.slug);
    }
  }
}

function createDescriptionAndReset(container) {
  let description = document.createElement("p");
  description.classList.add("firefoxLabs-description");
  document.l10n.setAttributes(description, "pane-experimental-description4");

  let link = document.createElement("a", { is: "moz-support-link" });
  link.setAttribute("data-l10n-name", "data-collection");
  link.setAttribute("support-page", "technical-and-interaction-data");
  description.append(link);

  container.append(description);

  let resetButton = document.createElement("moz-button");
  resetButton.id = "experimentalCategory-reset";
  document.l10n.setAttributes(resetButton, "pane-experimental-reset");
  resetButton.setAttribute(
    "iconsrc",
    "chrome://global/skin/icons/arrow-counterclockwise-16.svg"
  );
  resetButton.addEventListener("click", resetAllFeatures);
  container.append(resetButton);
}

function renderFeatures() {
  featuresContainer.querySelectorAll(".featureGate").forEach(el => el.remove());

  let groups = new Map();
  for (let optIn of firefoxLabs.all()) {
    if (!groups.has(optIn.firefoxLabsGroup)) {
      groups.set(optIn.firefoxLabsGroup, []);
    }
    groups.get(optIn.firefoxLabsGroup).push(optIn);
  }

  let frag = document.createDocumentFragment();
  for (let [group, optIns] of groups) {
    let card = document.createElement("moz-card");
    card.classList.add("featureGate");

    let fieldset = document.createElement("moz-fieldset");
    document.l10n.setAttributes(fieldset, group);
    card.append(fieldset);

    for (let optIn of optIns) {
      let checkbox = document.createElement("moz-checkbox");
      checkbox.dataset.nimbusSlug = optIn.slug;
      checkbox.dataset.nimbusBranchSlug = optIn.branches[0].slug;

      let description = document.createElement("div");
      description.slot = "description";
      description.id = `${optIn.slug}-description`;
      description.classList.add("featureGateDescription");

      for (let [key, value] of Object.entries(
        optIn.firefoxLabsDescriptionLinks ?? {}
      )) {
        let link = document.createElement("a");
        link.setAttribute("data-l10n-name", key);
        link.setAttribute("href", value);
        link.setAttribute("target", "_blank");
        description.append(link);
      }

      document.l10n.setAttributes(description, optIn.firefoxLabsDescription);
      checkbox.id = optIn.slug;
      checkbox.setAttribute("aria-describedby", description.id);
      document.l10n.setAttributes(checkbox, optIn.firefoxLabsTitle);

      checkbox.checked =
        ExperimentAPI.manager.store.get(optIn.slug)?.active ?? false;
      checkbox.addEventListener("change", onCheckboxChanged);

      checkbox.append(description);
      fieldset.append(checkbox);
    }

    frag.append(card);
  }

  featuresContainer.appendChild(frag);
  ExperimentAPI.manager.store.on("update", onNimbusUpdate);
  Services.obs.notifyObservers(window, "experimental-pane-features-rendered");
}

function setCategoryVisibility(shouldHide) {
  document.getElementById("category-experimental").hidden = shouldHide;

  Services.prefs.setBoolPref(
    "browser.preferences.experimental.hidden",
    shouldHide
  );

  if (
    shouldHide &&
    document.getElementById("categories").currentView == "paneExperimental"
  ) {
    window.gotoPref("general");
  }
}

function removeObservers() {
  ExperimentAPI.manager.store.off("update", onNimbusUpdate);

  if (observerAdded) {
    Services.obs.removeObserver(
      enrollmentsObserver,
      ExperimentAPI.ENROLLMENTS_UPDATED
    );
    observerAdded = false;
  }
}

let renderingPromise = Promise.resolve();

async function maybeRenderLabsRecipes() {
  firefoxLabs = await FirefoxLabs.create();

  let shouldHide = firefoxLabs.count === 0;
  setCategoryVisibility(shouldHide);

  if (shouldHide) {
    return;
  }

  renderFeatures();
}

function queueRender() {
  renderingPromise = renderingPromise.then(() => maybeRenderLabsRecipes());
  return renderingPromise;
}

let enrollmentsObserver = {
  observe(_subject, topic, _data) {
    if (topic === ExperimentAPI.ENROLLMENTS_UPDATED) {
      queueRender();
    }
  },
};

if (!ExperimentAPI.labsEnabled) {
  setCategoryVisibility(true);
} else {
  let settingPane = document.querySelector(
    'setting-pane[data-category="paneExperimental"]'
  );
  let section = settingPane?.querySelector("section");
  if (section) {
    featuresContainer = document.createElement("div");
    featuresContainer.id = "pane-experimental-featureGates";
    createDescriptionAndReset(section);
    section.append(featuresContainer);

    Services.obs.addObserver(
      enrollmentsObserver,
      ExperimentAPI.ENROLLMENTS_UPDATED
    );
    observerAdded = true;
    window.addEventListener("unload", removeObservers);
    queueRender();
  }
}
