/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { CERT_ERRORS } from "chrome://global/content/errors/cert-errors.mjs";
import { PKIX_ERRORS } from "chrome://global/content/errors/pkix-errors.mjs";
import { SSL_ERRORS } from "chrome://global/content/errors/ssl-errors.mjs";
import { NET_ERRORS } from "chrome://global/content/errors/net-errors.mjs";

const ERROR_REGISTRY = new Map();

export function registerError(config) {
  if (!config.id) {
    throw new Error("Error configuration must have an id");
  }
  if (ERROR_REGISTRY.has(config.id)) {
    throw new Error(`Duplicate error registration: "${config.id}"`);
  }
  ERROR_REGISTRY.set(config.id, Object.freeze(config));
}

export function registerErrors(configs) {
  for (const config of configs) {
    registerError(config);
  }
}

export function getErrorConfig(id) {
  return ERROR_REGISTRY.get(id);
}

export function isErrorSupported(id) {
  return ERROR_REGISTRY.has(id);
}

export function getErrorsByCategory(category) {
  return [...ERROR_REGISTRY.values()].filter(e => e.category === category);
}

export function getAllErrorIds() {
  return [...ERROR_REGISTRY.keys()];
}

export function getErrorCount() {
  return ERROR_REGISTRY.size;
}

export function _testOnlyClearRegistry() {
  if (!false) {
    return;
  }
  ERROR_REGISTRY.clear();
}

export function initializeRegistry() {
  if (ERROR_REGISTRY.size > 0) {
    return; 
  }

  registerErrors(CERT_ERRORS);
  registerErrors(PKIX_ERRORS);
  registerErrors(SSL_ERRORS);
  registerErrors(NET_ERRORS);
}
