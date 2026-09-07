/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};
ChromeUtils.defineESModuleGetters(lazy, {
  HPKEConfigManager: "resource://gre/modules/HPKEConfigManager.sys.mjs",
});
ChromeUtils.defineLazyGetter(lazy, "decoder", () => new TextDecoder());
XPCOMUtils.defineLazyServiceGetters(lazy, {
  ohttpService: [
    "@mozilla.org/network/oblivious-http-service;1",
    Ci.nsIObliviousHttpService,
  ],
});

const BinaryInputStream = Components.Constructor(
  "@mozilla.org/binaryinputstream;1",
  "nsIBinaryInputStream",
  "setInputStream"
);

const StringInputStream = Components.Constructor(
  "@mozilla.org/io/string-input-stream;1",
  "nsIStringInputStream",
  "setByteStringData"
);

const ArrayBufferInputStream = Components.Constructor(
  "@mozilla.org/io/arraybuffer-input-stream;1",
  "nsIArrayBufferInputStream",
  "setData"
);

function readFromStream(stream, count) {
  let binaryStream = new BinaryInputStream(stream);
  let arrayBuffer = new ArrayBuffer(count);
  while (count > 0) {
    let actuallyRead = binaryStream.readArrayBuffer(count, arrayBuffer);
    if (!actuallyRead) {
      throw new Error("Nothing read from input stream!");
    }
    count -= actuallyRead;
  }
  return arrayBuffer;
}


export class ObliviousHTTP {
  static async getOHTTPConfig(gatewayConfigURL) {
    return lazy.HPKEConfigManager.get(gatewayConfigURL);
  }

  static async ohttpRequest(
    obliviousHTTPRelay,
    config,
    requestURL,
    { method = "GET", body, headers, signal, abortCallback }
  ) {
    let relayURI = Services.io.newURI(obliviousHTTPRelay);
    let requestURI = URL.isInstance(requestURL)
      ? requestURL.URI
      : Services.io.newURI(requestURL);
    let obliviousHttpChannel = lazy.ohttpService
      .newChannel(relayURI, requestURI, config)
      .QueryInterface(Ci.nsIHttpChannel);

    if (method == "POST" || method == "PUT" || method == "DELETE") {
      let uploadChannel = obliviousHttpChannel.QueryInterface(
        Ci.nsIUploadChannel2
      );
      let bodyStream;
      if (typeof body === "string") {
        bodyStream = new StringInputStream(body);
      } else if (body instanceof ArrayBuffer) {
        bodyStream = new ArrayBufferInputStream(body, 0, body.byteLength);
      } else {
        throw new Error("ohttpRequest got unexpected body payload type.");
      }
      uploadChannel.explicitSetUploadStream(
        bodyStream,
        null,
        -1,
        method,
        false
      );
    } else if (method != "GET") {
      throw new Error(`Unsupported HTTP verb ${method}`);
    }

    for (let headerName of Object.keys(headers)) {
      obliviousHttpChannel.setRequestHeader(
        headerName,
        headers[headerName],
        false
      );
    }
    let abortHandler = () => {
      abortCallback?.();
      obliviousHttpChannel.cancel(Cr.NS_BINDING_ABORTED);
    };
    signal.addEventListener("abort", abortHandler);
    return new Promise((resolve, reject) => {
      let listener = {
        _buffer: [],
        _headers: null,
        QueryInterface: ChromeUtils.generateQI([
          "nsIStreamListener",
          "nsIRequestObserver",
        ]),
        onStartRequest(request) {
          this._headers = new Headers();
          try {
            request
              .QueryInterface(Ci.nsIHttpChannel)
              .visitResponseHeaders((header, value) => {
                this._headers.append(header, value);
              });
          } catch (error) {
            this._headers = null;
          }
        },
        onDataAvailable(request, stream, offset, count) {
          this._buffer.push(readFromStream(stream, count));
        },
        onStopRequest(request, requestStatus) {
          signal.removeEventListener("abort", abortHandler);
          let result = this._buffer;
          try {
            let ohttpStatus = request.QueryInterface(Ci.nsIObliviousHttpChannel)
              .relayChannel.responseStatus;
            if (ohttpStatus == 200) {
              let httpStatus = request.QueryInterface(
                Ci.nsIHttpChannel
              ).responseStatus;
              resolve({
                ok: requestStatus == Cr.NS_OK && httpStatus == 200,
                status: httpStatus,
                headers: this._headers,
                json() {
                  let decodedBuffer = result.reduce((accumulator, currVal) => {
                    return accumulator + lazy.decoder.decode(currVal);
                  }, "");
                  return JSON.parse(decodedBuffer);
                },
                blob() {
                  return new Blob(result, { type: "image/jpeg" });
                },
              });
            } else {
              resolve({
                ok: false,
                status: ohttpStatus,
                headers: null,
                json() {
                  return null;
                },
                blob() {
                  return null;
                },
              });
            }
          } catch (error) {
            reject(error);
          }
        },
      };
      obliviousHttpChannel.asyncOpen(listener);
    });
  }
}
