/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  ObliviousHTTP: "resource://gre/modules/ObliviousHTTP.sys.mjs",
  E10SUtils: "resource://gre/modules/E10SUtils.sys.mjs",
  NetUtil: "resource://gre/modules/NetUtil.sys.mjs",
});

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "obliviousHttpService",
  "@mozilla.org/network/oblivious-http-service;1",
  Ci.nsIObliviousHttpService
);

const HOST_MAP = new Map([
  [
    "newtab-image",
    {
      gatewayConfigURLPrefName:
        "browser.newtabpage.activity-stream.discoverystream.ohttp.configURL",
      relayURLPrefName:
        "browser.newtabpage.activity-stream.discoverystream.ohttp.relayURL",
    },
  ],
]);

export class MozCachedOHTTPProtocolHandler {
  scheme = "moz-cached-ohttp";

  #injectedOHTTPService = null;

  allowPort(_port, _scheme) {
    return false;
  }

  newChannel(uri, loadInfo) {
    if (
      Services.appinfo.remoteType == lazy.E10SUtils.PRIVILEGEDABOUT_REMOTE_TYPE
    ) {
      return new MozCachedOHTTPChannel(
        uri,
        loadInfo,
        this,
        this.#getOHTTPService(),
        !!this.#injectedOHTTPService 
      );
    }

    if (Services.appinfo.processType == Services.appinfo.PROCESS_TYPE_DEFAULT) {
      const loadingPrincipal = loadInfo?.loadingPrincipal;
      if (loadingPrincipal) {
        if (loadingPrincipal.isSystemPrincipal) {
          return new MozCachedOHTTPChannel(
            uri,
            loadInfo,
            this,
            this.#getOHTTPService(),
            !!this.#injectedOHTTPService 
          );
        }

        try {
          const remoteType =
            lazy.E10SUtils.getRemoteTypeForPrincipal(loadingPrincipal);
          if (remoteType === lazy.E10SUtils.PRIVILEGEDABOUT_REMOTE_TYPE) {
            return new MozCachedOHTTPChannel(
              uri,
              loadInfo,
              this,
              this.#getOHTTPService(),
              !!this.#injectedOHTTPService 
            );
          }
        } catch (e) {
          // E10SUtils might throw for invalid principals, fall through to rejection
        }
      }
    }

    throw Components.Exception(
      "moz-cached-ohttp protocol only accessible from privileged about content",
      Cr.NS_ERROR_INVALID_ARG
    );
  }

  async getOHTTPGatewayConfigAndRelayURI(host) {
    let hostMapping = HOST_MAP.get(host);
    if (!hostMapping) {
      throw new Error(`Unrecognized host for OHTTP config: ${host}`);
    }
    if (
      hostMapping.gatewayConfigURL === undefined ||
      hostMapping.relayURL === undefined
    ) {
      XPCOMUtils.defineLazyPreferenceGetter(
        hostMapping,
        "gatewayConfigURL",
        hostMapping.gatewayConfigURLPrefName,
        ""
      );
      XPCOMUtils.defineLazyPreferenceGetter(
        hostMapping,
        "relayURL",
        hostMapping.relayURLPrefName,
        ""
      );
    }
    if (!hostMapping.gatewayConfigURL) {
      throw new Error(
        `OHTTP Gateway config URL not configured for host: ${host}`
      );
    }
    if (!hostMapping.relayURL) {
      throw new Error(`OHTTP relay URL not configured for host: ${host}`);
    }

    const ohttpGatewayConfig = await lazy.ObliviousHTTP.getOHTTPConfig(
      hostMapping.gatewayConfigURL
    );

    return {
      ohttpGatewayConfig,
      relayURI: Services.io.newURI(hostMapping.relayURL),
    };
  }

  injectOHTTPService(service) {
    this.#injectedOHTTPService = service;
  }

  #getOHTTPService() {
    return this.#injectedOHTTPService || lazy.obliviousHttpService;
  }

  QueryInterface = ChromeUtils.generateQI(["nsIProtocolHandler"]);
}

export class MozCachedOHTTPChannel {
  #uri;
  #loadInfo;
  #protocolHandler;
  #ohttpService;
  #listener = null;
  #loadFlags = 0;
  #status = Cr.NS_OK;
  #cancelled = false;
  #originalURI;
  #contentType = "";
  #contentCharset = "";
  #contentLength = -1;
  #owner = null;
  #securityInfo = null;
  #notificationCallbacks = null;
  #loadGroup = null;
  #pendingChannel = null;
  #startedRequest = false;
  #inTestingMode = false;

  constructor(
    uri,
    loadInfo,
    protocolHandler,
    ohttpService,
    inTestingMode = false
  ) {
    this.#uri = uri;
    this.#loadInfo = loadInfo;
    this.#protocolHandler = protocolHandler;
    this.#ohttpService = ohttpService;
    this.#originalURI = uri;
    this.#inTestingMode = inTestingMode;
  }

  get URI() {
    return this.#uri;
  }

  get originalURI() {
    return this.#originalURI;
  }

  set originalURI(aURI) {
    this.#originalURI = aURI;
  }

  get status() {
    return this.#status;
  }

  get contentType() {
    return this.#contentType;
  }

  set contentType(aContentType) {
    this.#contentType = aContentType;
  }

  get contentCharset() {
    return this.#contentCharset;
  }

  set contentCharset(aContentCharset) {
    this.#contentCharset = aContentCharset;
  }

  get contentLength() {
    return this.#contentLength;
  }

  set contentLength(aContentLength) {
    this.#contentLength = aContentLength;
  }

  get loadFlags() {
    return this.#loadFlags;
  }

  set loadFlags(aLoadFlags) {
    this.#loadFlags = aLoadFlags;
  }

  get loadInfo() {
    return this.#loadInfo;
  }

  set loadInfo(aLoadInfo) {
    this.#loadInfo = aLoadInfo;
  }

  get owner() {
    return this.#owner;
  }

  set owner(aOwner) {
    this.#owner = aOwner;
  }

  get securityInfo() {
    return this.#securityInfo;
  }

  get notificationCallbacks() {
    return this.#notificationCallbacks;
  }

  set notificationCallbacks(aCallbacks) {
    this.#notificationCallbacks = aCallbacks;
  }

  get loadGroup() {
    return this.#loadGroup;
  }

  set loadGroup(aLoadGroup) {
    this.#loadGroup = aLoadGroup;
  }

  get name() {
    return this.#uri.spec;
  }

  isPending() {
    return this.#pendingChannel ? this.#pendingChannel.isPending() : false;
  }

  cancel(status) {
    this.#cancelled = true;
    this.#status = status;

    if (this.#pendingChannel) {
      this.#pendingChannel.cancel(status);
    }
  }

  suspend() {
    if (this.#pendingChannel) {
      this.#pendingChannel.suspend();
    }
  }

  resume() {
    if (this.#pendingChannel) {
      this.#pendingChannel.resume();
    }
  }

  open() {
    throw Components.Exception(
      "moz-cached-ohttp protocol does not support synchronous open",
      Cr.NS_ERROR_NOT_IMPLEMENTED
    );
  }

  asyncOpen(listener) {
    if (this.#cancelled) {
      throw Components.Exception("Channel was cancelled", this.#status);
    }

    this.#listener = listener;

    this.#loadResource().catch(error => {
      console.error("moz-cached-ohttp channel error:", error);
      this.#notifyError(Cr.NS_ERROR_FAILURE);
    });
  }

  async #loadResource() {
    const { resourceURI, host } = this.#extractHostAndResourceURI();
    if (!resourceURI) {
      throw new Error("Invalid moz-cached-ohttp URL format");
    }

    const wasCached = await this.#tryCache(resourceURI);
    if (wasCached) {
      return;
    }

    if (!HOST_MAP.has(host)) {
      throw new Error(`Unrecognized moz-cached-ohttp host: ${host}`);
    }

    await this.#loadViaOHTTP(resourceURI, host);
  }

  #extractHostAndResourceURI() {
    try {
      const url = new URL(this.#uri.spec);
      const host = url.host;
      const searchParams = new URLSearchParams(url.search);
      const resourceURLString = searchParams.get("url");

      if (!resourceURLString) {
        return null;
      }

      const resourceURL = new URL(resourceURLString);
      if (resourceURL.protocol !== "https:") {
        return null;
      }

      return { resourceURI: Services.io.newURI(resourceURLString), host };
    } catch (e) {
      return null;
    }
  }

  async #tryCache(resourceURI) {
    let result;

    if (
      Services.appinfo.processType === Services.appinfo.PROCESS_TYPE_DEFAULT
    ) {
      const [parentProcess] = ChromeUtils.getAllDOMProcesses();
      const parentActor = parentProcess.getActor("MozCachedOHTTP");
      result = await parentActor.tryCache(resourceURI);
    } else {
      const childActor = ChromeUtils.domProcessChild.getActor("MozCachedOHTTP");
      result = await childActor.sendQuery("tryCache", {
        uriString: resourceURI.spec,
      });
    }

    if (result.success) {
      const pump = this.#createInputStreamPump(result.inputStream);
      let headers = new Headers(result.headersObj);
      this.#applyContentHeaders(headers);

      await this.#streamFromCache(pump, result.inputStream);
      return true;
    }

    return false;
  }

  #applyContentHeaders(headers) {
    let contentTypeHeader = headers.get("Content-Type");
    if (contentTypeHeader) {
      let charSet = {};
      let hadCharSet = {};
      this.#contentType = Services.io.parseResponseContentType(
        contentTypeHeader,
        charSet,
        hadCharSet
      );

      if (hadCharSet.value) {
        this.#contentCharset = charSet.value;
      }
    }
  }

  #createInputStreamPump(inputStream) {
    const pump = Cc["@mozilla.org/network/input-stream-pump;1"].createInstance(
      Ci.nsIInputStreamPump
    );
    pump.init(inputStream, 0, 0, false);
    return pump;
  }

  #streamFromCache(pump, inputStream) {
    return new Promise((resolve, reject) => {
      const listener = this.#createCacheStreamListener(
        pump,
        inputStream,
        resolve
      );

      try {
        pump.asyncRead(listener);
      } catch (error) {
        this.#safeCloseStream(inputStream);
        reject(error);
      }
    });
  }

  #createCacheStreamListener(pump, inputStream, resolve) {
    return {
      onStartRequest: () => {
        this.#startedRequest = true;
        this.#pendingChannel = pump;
        this.#listener.onStartRequest(this);
      },

      onDataAvailable: (request, innerInputStream, offset, count) => {
        this.#listener.onDataAvailable(this, innerInputStream, offset, count);
      },

      onStopRequest: (request, status) => {
        this.#pendingChannel = null;
        this.#safeCloseStream(inputStream);

        this.#listener.onStopRequest(this, status);
        resolve(Components.isSuccessCode(status));
      },
    };
  }

  #safeCloseStream(stream) {
    try {
      stream.close();
    } catch (e) {
    }
  }

  async #loadViaOHTTP(resourceURI, host) {
    const { ohttpGatewayConfig, relayURI } =
      await this.#protocolHandler.getOHTTPGatewayConfigAndRelayURI(host);

    const ohttpChannel = this.#createOHTTPChannel(
      relayURI,
      resourceURI,
      ohttpGatewayConfig
    );

    return this.#executeOHTTPRequest(ohttpChannel);
  }

  #createOHTTPChannel(relayURI, resourceURI, ohttpConfig) {
    ChromeUtils.releaseAssert(
      Services.appinfo.processType === Services.appinfo.PROCESS_TYPE_DEFAULT ||
        Services.appinfo.remoteType ===
          lazy.E10SUtils.PRIVILEGEDABOUT_REMOTE_TYPE,
      "moz-cached-ohttp is only allowed in privileged content processes " +
        "or the main process"
    );

    const ohttpChannel = this.#ohttpService.newChannel(
      relayURI,
      resourceURI,
      ohttpConfig
    );

    const loadInfo = lazy.NetUtil.newChannel({
      uri: relayURI,
      loadUsingSystemPrincipal: true,
      contentPolicyType: Ci.nsIContentPolicy.TYPE_OTHER,
    }).loadInfo;

    ohttpChannel.loadInfo = loadInfo;
    ohttpChannel.loadFlags = this.#loadFlags | Ci.nsIRequest.LOAD_ANONYMOUS;

    if (this.#notificationCallbacks) {
      ohttpChannel.notificationCallbacks = this.#notificationCallbacks;
    }

    if (this.#loadGroup) {
      ohttpChannel.loadGroup = this.#loadGroup;
    }

    return ohttpChannel;
  }

  #executeOHTTPRequest(ohttpChannel) {
    this.#pendingChannel = ohttpChannel;

    let cachePipe = Cc["@mozilla.org/pipe;1"].createInstance(Ci.nsIPipe);
    cachePipe.init(
      true ,
      false ,
      0 ,
      0 
    );
    let cacheStreamUpdate = new MessageChannel();

    return new Promise((resolve, reject) => {
      if (
        Services.appinfo.processType === Services.appinfo.PROCESS_TYPE_DEFAULT
      ) {
        const [parentProcess] = ChromeUtils.getAllDOMProcesses();
        const parentActor = parentProcess.getActor("MozCachedOHTTP");
        parentActor.writeCache(
          ohttpChannel.URI,
          cachePipe.inputStream,
          cacheStreamUpdate.port2
        );
      } else {
        const childActor =
          ChromeUtils.domProcessChild.getActor("MozCachedOHTTP");
        childActor.sendAsyncMessage(
          "writeCache",
          {
            uriString: ohttpChannel.URI.spec,
            cacheInputStream: cachePipe.inputStream,
            cacheStreamUpdatePort: cacheStreamUpdate.port2,
          },
          [cacheStreamUpdate.port2]
        );
      }

      const originalListener = this.#createOHTTPResponseListener(
        cacheStreamUpdate.port1,
        cachePipe.outputStream,
        resolve,
        reject
      );

      const finalListener = this.#setupStreamTee(
        originalListener,
        cachePipe.outputStream
      );

      try {
        ohttpChannel.asyncOpen(finalListener);
      } catch (error) {
        this.#cleanupCacheOnError(
          cacheStreamUpdate.port1,
          cachePipe.outputStream
        );
        reject(error);
      }
    });
  }

  #createOHTTPResponseListener(
    cacheStreamUpdatePort,
    cachePipeOutputStream,
    resolve,
    reject
  ) {
    return {
      onStartRequest: request => {
        if (request instanceof Ci.nsIHttpChannel) {
          try {
            const contentType = request.getResponseHeader("content-type");
            if (contentType) {
              let headers = new Headers();
              headers.set("content-type", contentType);
              this.#applyContentHeaders(headers);
            }
          } catch (e) {
          }
        }

        this.#startedRequest = true;
        this.#listener.onStartRequest(this);
        this.#processResponseHeaders(cacheStreamUpdatePort, request);
        this.#processCacheControl(cacheStreamUpdatePort, request);
      },

      onDataAvailable: (request, inputStream, offset, count) => {
        this.#listener.onDataAvailable(this, inputStream, offset, count);
      },

      onStopRequest: (request, status) => {
        this.#pendingChannel = null;
        this.#finalizeCacheEntry(
          cacheStreamUpdatePort,
          cachePipeOutputStream,
          status
        );
        this.#listener.onStopRequest(this, status);

        if (Components.isSuccessCode(status)) {
          resolve();
        } else {
          reject(new Error(`OHTTP request failed with status: ${status}`));
        }
      },
    };
  }

  #setupStreamTee(originalListener, cacheOutputStream) {
    if (!cacheOutputStream) {
      return originalListener;
    }

    try {
      const tee = Cc[
        "@mozilla.org/network/stream-listener-tee;1"
      ].createInstance(Ci.nsIStreamListenerTee);
      tee.init(originalListener, cacheOutputStream, null);
      return tee;
    } catch (error) {
      console.warn(
        "Failed to create stream tee, proceeding without caching:",
        error
      );
      this.#safeCloseStream(cacheOutputStream);
      return originalListener;
    }
  }

  #finalizeCacheEntry(cacheStreamUpdatePort, cacheOutputStream, status) {
    try {
      if (cacheOutputStream) {
        cacheOutputStream.closeWithStatus(Cr.NS_BASE_STREAM_CLOSED);
      }

      if (!Components.isSuccessCode(status)) {
        cacheStreamUpdatePort.postMessage({ name: "DoomCacheEntry" });
      }
    } catch (error) {
      console.warn("Failed to finalize cache entry:", error);
    }
  }

  #cleanupCacheOnError(cacheStreamUpdatePort, cacheOutputStream) {
    try {
      if (cacheOutputStream) {
        cacheOutputStream.closeWithStatus(Cr.NS_BASE_STREAM_CLOSED);
      }
      cacheStreamUpdatePort.postMessage({ name: "DoomCacheEntry" });
    } catch (e) {
    }
  }

  #notifyError(status) {
    this.#status = status;
    if (this.#listener) {
      if (!this.#startedRequest) {
        this.#listener.onStartRequest(this);
      }
      this.#listener.onStopRequest(this, status);
    }
  }

  #processResponseHeaders(cacheStreamUpdatePort, httpChannel) {
    let headersObj = {};
    if (this.#inTestingMode) {
      httpChannel = httpChannel.wrappedJSObject;
    } else if (!(httpChannel instanceof Ci.nsIHttpChannel)) {
      return;
    }

    httpChannel.visitResponseHeaders({
      visitHeader(name, value) {
        headersObj[name] = value;
      },
    });

    cacheStreamUpdatePort.postMessage({
      name: "WriteOriginalResponseHeaders",
      headersObj,
    });
  }

  #processCacheControl(cacheStreamUpdatePort, httpChannel) {
    if (!(httpChannel instanceof Ci.nsIHttpChannel)) {
      return;
    }

    let expirationTime = null;

    let cacheControl = null;
    try {
      cacheControl = httpChannel.getResponseHeader("cache-control");
    } catch (e) {
    }

    if (cacheControl) {
      let cacheControlParseResult =
        Services.io.parseCacheControlHeader(cacheControl);
      if (cacheControlParseResult.maxAge) {
        expirationTime = Date.now() + cacheControlParseResult.maxAge * 1000;
      } else if (
        cacheControlParseResult.noCache ||
        cacheControlParseResult.noStore
      ) {
        cacheStreamUpdatePort.postMessage({ name: "DoomCacheEntry" });
        return;
      }
    }

    if (!expirationTime) {
      try {
        const expires = httpChannel.getResponseHeader("expires");
        if (expires) {
          expirationTime = new Date(expires).getTime();
        }
      } catch (e) {
      }
    }

    expirationTime ??= Date.now() + 24 * 60 * 60 * 1000; 

    cacheStreamUpdatePort.postMessage({
      name: "WriteCacheExpiry",
      expiry: Math.floor(expirationTime / 1000),
    });
  }

  QueryInterface = ChromeUtils.generateQI(["nsIChannel"]);
}
