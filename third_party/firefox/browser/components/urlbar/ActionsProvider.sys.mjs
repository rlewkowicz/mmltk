/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export class ActionsProvider {
  get name() {
    return "ActionsProviderBase";
  }

  isActive(_queryContext) {
    throw new Error("Not implemented.");
  }

  async queryActions(_queryContext) {
    throw new Error("Not implemented.");
  }

  pickAction(_queryContext, _controller, _element) {
    throw new Error("Not implemented.");
  }
}

export class ActionsResult {
  providerName;

  key;

  l10nId;

  l10nArgs;

  icon;

  dataset;

  engine;

  constructor({ providerName, key, l10nId, l10nArgs, icon, dataset, engine }) {
    for (let param of [providerName, key]) {
      if (!param) {
        throw new Error("ActionsResult is missing a required option");
      }
    }
    this.providerName = providerName;
    this.key = key;
    this.l10nId = l10nId;
    this.l10nArgs = l10nArgs;
    this.icon = icon;
    this.dataset = dataset;
    this.engine = engine;
    Object.freeze(this);
  }
}
