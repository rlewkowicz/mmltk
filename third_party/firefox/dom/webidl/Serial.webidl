/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The origin of this IDL file is
 * https://wicg.github.io/serial/
 */

typedef (DOMString or unsigned long) BluetoothServiceUUID;

dictionary SerialPortRequestOptions {
  sequence<SerialPortFilter> filters;
  sequence<BluetoothServiceUUID> allowedBluetoothServiceClassIds;
};

dictionary SerialPortFilter {
  unsigned short usbVendorId;
  unsigned short usbProductId;
  BluetoothServiceUUID bluetoothServiceClassId;
};

[SecureContext, Pref="dom.webserial.enabled",
 Exposed=(Window,DedicatedWorker)]
interface Serial : EventTarget {
  attribute EventHandler onconnect;
  attribute EventHandler ondisconnect;
  [Throws] Promise<sequence<SerialPort>> getPorts();
  [Exposed=Window, Throws] Promise<SerialPort> requestPort(optional SerialPortRequestOptions options = {});
};
