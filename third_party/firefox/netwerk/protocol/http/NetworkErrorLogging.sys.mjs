/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

function policyExpired(policy) {
  let currentDate = new Date();
  return (currentDate - policy.creation) / 1_000 > policy.nel.max_age;
}

function errorType(aChannel) {
  switch (aChannel.status) {
    case Cr.NS_ERROR_UNKNOWN_HOST:
      return "dns.name_not_resolved";
    case Cr.NS_ERROR_REDIRECT_LOOP:
      return "http.response.redirect_loop";
    case Cr.NS_BINDING_REDIRECTED:
      return "ok";
    case Cr.NS_ERROR_NET_TIMEOUT:
      return "tcp.timed_out";
    case Cr.NS_ERROR_NET_RESET:
      return "tcp.reset";
    case Cr.NS_ERROR_CONNECTION_REFUSED:
      return "tcp.refused";
    default:
      break;
  }

  if (
    aChannel.status == Cr.NS_OK &&
    (aChannel.responseStatus / 100 == 2 || aChannel.responseStatus == 304)
  ) {
    return "ok";
  }

  if (
    aChannel.status == Cr.NS_OK &&
    aChannel.responseStatus >= 400 &&
    aChannel.responseStatus <= 599
  ) {
    return "http.error";
  }
  return "unknown" + aChannel.status;
}

function channelPhase(aChannel) {
  const NS_NET_STATUS_RESOLVING_HOST = 0x4b0003;
  const NS_NET_STATUS_RESOLVED_HOST = 0x4b000b;
  const NS_NET_STATUS_CONNECTING_TO = 0x4b0007;
  const NS_NET_STATUS_CONNECTED_TO = 0x4b0004;
  const NS_NET_STATUS_TLS_HANDSHAKE_STARTING = 0x4b000c;
  const NS_NET_STATUS_TLS_HANDSHAKE_ENDED = 0x4b000d;
  const NS_NET_STATUS_SENDING_TO = 0x4b0005;
  const NS_NET_STATUS_WAITING_FOR = 0x4b000a;
  const NS_NET_STATUS_RECEIVING_FROM = 0x4b0006;
  const NS_NET_STATUS_READING = 0x4b0008;
  const NS_NET_STATUS_WRITING = 0x4b0009;

  let lastStatus = aChannel.QueryInterface(
    Ci.nsIHttpChannelInternal
  ).lastTransportStatus;

  switch (lastStatus) {
    case NS_NET_STATUS_RESOLVING_HOST:
    case NS_NET_STATUS_RESOLVED_HOST:
      return "dns";
    case NS_NET_STATUS_CONNECTING_TO:
    case NS_NET_STATUS_CONNECTED_TO: 
      return "connection";
    case NS_NET_STATUS_TLS_HANDSHAKE_STARTING:
    case NS_NET_STATUS_TLS_HANDSHAKE_ENDED:
      return "connection";
    case NS_NET_STATUS_SENDING_TO:
    case NS_NET_STATUS_WAITING_FOR:
    case NS_NET_STATUS_RECEIVING_FROM:
    case NS_NET_STATUS_READING:
    case NS_NET_STATUS_WRITING:
      return "application";
    default:
      return "dns";
  }
}

export class NetworkErrorLogging {
  constructor() {}

  policyCache = {};

  registerPolicy(aChannel) {
    if (
      !Services.scriptSecurityManager.getChannelResultPrincipal(aChannel)
        .isOriginPotentiallyTrustworthy
    ) {
      return;
    }

    let list = [];
    aChannel.getOriginalResponseHeader("NEL", {
      QueryInterface: ChromeUtils.generateQI(["nsIHttpHeaderVisitor"]),
      visitHeader: (aHeader, aValue) => {
        list.push(aValue);
      },
    });

    if (!list.length) {
      return;
    }

    let origin =
      Services.scriptSecurityManager.getChannelResultPrincipal(aChannel).origin;

    let key = Services.io.originAttributesForNetworkState(aChannel);

    let item = JSON.parse(list[0]);

    if (!item.max_age || !Number.isInteger(item.max_age)) {
      return;
    }

    if (!item.max_age) {
      delete this.policyCache[String([key, origin])];
      return;
    }

    if (!item.report_to || typeof item.report_to != "string") {
      return;
    }

    if (
      item.success_fraction &&
      (typeof item.success_fraction != "number" ||
        item.success_fraction < 0 ||
        item.success_fraction > 1)
    ) {
      return;
    }

    if (
      item.failure_fraction &&
      (typeof item.failure_fraction != "number" ||
        item.failure_fraction < 0 ||
        item.success_fraction > 1)
    ) {
      return;
    }

    if (
      item.request_headers &&
      !Array.isArray(
        item.request_headers ||
          !item.request_headers.every(e => typeof e == "string")
      )
    ) {
      return;
    }

    if (
      item.response_headers &&
      !Array.isArray(
        item.response_headers ||
          !item.response_headers.every(e => typeof e == "string")
      )
    ) {
      return;
    }

    let policy = {};

    try {
      policy.ip_address = aChannel.QueryInterface(
        Ci.nsIHttpChannelInternal
      ).remoteAddress;
    } catch (e) {
      return;
    }

    policy.origin = origin;

    if (item.include_subdomains) {
      policy.subdomains = true;
    }

    policy.request_headers = item.request_headers;
    policy.response_headers = item.response_headers;
    policy.ttl = item.max_age;
    policy.creation = new Date();
    policy.successful_sampling_rate = item.success_fraction || 0.0;
    policy.failure_sampling_rate = item.failure_fraction || 1.0;

    policy.nel = item;

    this.policyCache[String([key, origin])] = policy;
  }

  choosePolicyForRequest(aChannel) {
    let principal =
      Services.scriptSecurityManager.getChannelResultPrincipal(aChannel);
    let origin = principal.origin;
    let key = Services.io.originAttributesForNetworkState(aChannel);

    let policy = this.policyCache[String([key, origin])];
    if (policy) {
      if (!policyExpired(policy)) {
        return { policy, key, origin };
      }
    }

    while (principal.nextSubDomainPrincipal) {
      principal = principal.nextSubDomainPrincipal;
      origin = principal.origin;
      policy = this.policyCache[String([key, origin])];
      if (policy && !policyExpired(policy)) {
        return { policy, key, origin };
      }
    }

    return {};
  }

  generateNELReport(aChannel) {
    if (
      !Services.scriptSecurityManager.getChannelResultPrincipal(aChannel)
        .isOriginPotentiallyTrustworthy
    ) {
      return null;
    }
    let origin =
      Services.scriptSecurityManager.getChannelResultPrincipal(aChannel).origin;

    let {
      policy,
      key,
      origin: policyOrigin,
    } = this.choosePolicyForRequest(aChannel);
    if (!policy) {
      return null;
    }

    let samplingRate = 0.0;
    if (
      aChannel.status == Cr.NS_OK &&
      aChannel.responseStatus >= 200 &&
      aChannel.responseStatus <= 299
    ) {
      samplingRate = policy.successful_sampling_rate || 0.0;
    } else {
      samplingRate = policy.successful_sampling_rate || 1.0;
    }

    if (Math.random() >= samplingRate) {
      return null;
    }


    let phase = channelPhase(aChannel);
    let report_body = {
      sampling_fraction: samplingRate,
      elapsed_time: 1, 
      phase,
      type: errorType(aChannel), 
    };

    if (phase != "dns") {
      report_body.server_ip = aChannel.QueryInterface(
        Ci.nsIHttpChannelInternal
      ).remoteAddress;
      report_body.protocol = aChannel.protocolVersion;
    }

    if (phase != "dns" && phase != "connection") {
      report_body.method = aChannel.requestMethod;
      report_body.status_code = aChannel.responseStatus;
    }

    if (
      origin != policyOrigin &&
      policy.subdomains &&
      report_body.phase != "dns"
    ) {
      return null;
    }

    if (phase != "dns" && report_body.server_ip != policy.ip_address) {
      report_body.phase = "dns";
      report_body.type = "dns.address_changed";
      delete report_body.request_headers;
      delete report_body.response_headers;
      delete report_body.status_code;
      delete report_body.elapsed_time;
    }

    let currentDate = new Date();
    if ((currentDate - policy.creation) / 1_000 > 172800) {
      delete this.policyCache[String([key, policyOrigin])];

    }


    let uriMutator = aChannel.URI.mutate().setRef("");
    if (report_body.phase == "dns" || report_body.phase == "connection") {
      uriMutator.setPathQueryRef("");
    }
    if (aChannel.URI.hasUserPass) {
      uriMutator.setUserPass("");
    }
    let url = uriMutator.finalize().spec;

    let retObj = {
      body: JSON.stringify(report_body),
      group: policy.nel.report_to,
      url,
    };

    return retObj;
  }

  QueryInterface = ChromeUtils.generateQI(["nsINetworkErrorLogging"]);
}
