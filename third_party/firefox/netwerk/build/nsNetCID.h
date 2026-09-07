/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsNetCID_h_
#define nsNetCID_h_


#define NS_IOSERVICE_CONTRACTID "@mozilla.org/network/io-service;1"
#define NS_IOSERVICE_CID                      \
  { \
   0x9ac9e770,                                \
   0x18bc,                                    \
   0x11d3,                                    \
   {0x93, 0x37, 0x00, 0x10, 0x4b, 0xa0, 0xfd, 0x40}}

#define NS_NETUTIL_CONTRACTID "@mozilla.org/network/util;1"

#define NS_SERIALIZATION_HELPER_CONTRACTID \
  "@mozilla.org/network/serialization-helper;1"
#define NS_SERIALIZATION_HELPER_CID           \
  { \
   0xd6ef593d,                                \
   0xa429,                                    \
   0x4b14,                                    \
   {0xa8, 0x87, 0xd9, 0xe2, 0xf7, 0x65, 0xd9, 0xed}}

#define NS_PROTOCOLPROXYSERVICE_CONTRACTID \
  "@mozilla.org/network/protocol-proxy-service;1"
#define NS_PROTOCOLPROXYSERVICE_CID           \
  { \
   0xe9b301c0,                                \
   0xe0e4,                                    \
   0x11d3,                                    \
   {0xa1, 0xa8, 0x0, 0x50, 0x4, 0x1c, 0xaf, 0x44}}

#define NS_LOADGROUP_CONTRACTID "@mozilla.org/network/load-group;1"
#define NS_LOADGROUP_CID                      \
  { \
   0xe1c61582,                                \
   0x2a84,                                    \
   0x11d3,                                    \
   {0x8c, 0xce, 0x00, 0x60, 0xb0, 0xfc, 0x14, 0xa3}}

#define NS_SIMPLEURI_CID                      \
  { \
   0xe0da1d70,                                \
   0x2f7b,                                    \
   0x11d3,                                    \
   {0x8c, 0xd0, 0x00, 0x60, 0xb0, 0xfc, 0x14, 0xa3}}

#define NS_ICONURI_CID                        \
  { \
   0x1460df3b,                                \
   0x774c,                                    \
   0x4205,                                    \
   {0x83, 0x49, 0x83, 0x8e, 0x50, 0x7c, 0x3e, 0xf9}}

#define NS_SIMPLEURIMUTATOR_CONTRACTID \
  "@mozilla.org/network/simple-uri-mutator;1"
#define NS_SIMPLEURIMUTATOR_CID               \
  { \
   0x2be14592,                                \
   0x28d4,                                    \
   0x4a83,                                    \
   {0x8f, 0xe9, 0x08, 0xe7, 0x78, 0x84, 0x9f, 0x6e}}

#define NS_SIMPLENESTEDURI_CID                \
  { \
   0x56388dad,                                \
   0x287b,                                    \
   0x4240,                                    \
   {0xa7, 0x85, 0x85, 0xc3, 0x94, 0x01, 0x25, 0x03}}

#define NS_SIMPLENESTEDURIMUTATOR_CID         \
  { \
   0x9c4e9d49,                                \
   0xce64,                                    \
   0x4ca3,                                    \
   {0xac, 0xef, 0x30, 0x75, 0xc5, 0xe5, 0xab, 0xa7}}

#define NS_NESTEDABOUTURI_CID                 \
  { \
   0x2f277c00,                                \
   0x0eaf,                                    \
   0x4ddb,                                    \
   {0xb9, 0x36, 0x41, 0x32, 0x6b, 0xa4, 0x8a, 0xae}}

#define NS_NESTEDABOUTURIMUTATOR_CID          \
  { \
   0xb0054ef3,                                \
   0xb096,                                    \
   0x483d,                                    \
   {0x82, 0x42, 0x4e, 0xe3, 0x6b, 0x7b, 0x21, 0x15}}

#define NS_STANDARDURL_CID                    \
  { \
   0xde9472d0,                                \
   0x8034,                                    \
   0x11d3,                                    \
   {0x93, 0x99, 0x00, 0x10, 0x4b, 0xa0, 0xfd, 0x40}}

#define NS_STANDARDURLMUTATOR_CONTRACTID \
  "@mozilla.org/network/standard-url-mutator;1"
#define NS_STANDARDURLMUTATOR_CID             \
  { \
   0xce7d7da0,                                \
   0xfb28,                                    \
   0x44a3,                                    \
   {0x8c, 0x7b, 0x00, 0x0c, 0x16, 0x59, 0x18, 0xf4}}

#define NS_NOAUTHURLPARSER_CONTRACTID \
  "@mozilla.org/network/url-parser;1?auth=no"
#define NS_NOAUTHURLPARSER_CID                \
  { \
   0x78804a84,                                \
   0x8173,                                    \
   0x42b6,                                    \
   {0xbb, 0x94, 0x78, 0x9f, 0x08, 0x16, 0xa8, 0x10}}

#define NS_AUTHURLPARSER_CONTRACTID "@mozilla.org/network/url-parser;1?auth=yes"
#define NS_AUTHURLPARSER_CID                  \
  { \
   0x275d800e,                                \
   0x3f60,                                    \
   0x4896,                                    \
   {0xad, 0xb7, 0xd7, 0xf3, 0x90, 0xce, 0x0e, 0x42}}

#define NS_STDURLPARSER_CONTRACTID \
  "@mozilla.org/network/url-parser;1?auth=maybe"
#define NS_STDURLPARSER_CID                   \
  { \
   0xff41913b,                                \
   0x546a,                                    \
   0x4bff,                                    \
   {0x92, 0x01, 0xdc, 0x9b, 0x2c, 0x03, 0x2e, 0xba}}

#define NS_SIMPLESTREAMLISTENER_CONTRACTID \
  "@mozilla.org/network/simple-stream-listener;1"
#define NS_SIMPLESTREAMLISTENER_CID           \
  { \
   0xfb8cbf4e,                                \
   0x4701,                                    \
   0x4ba1,                                    \
   {0xb1, 0xd6, 0x53, 0x88, 0xe0, 0x41, 0xfb, 0x67}}

#define NS_STREAMLISTENERTEE_CONTRACTID \
  "@mozilla.org/network/stream-listener-tee;1"
#define NS_STREAMLISTENERTEE_CID              \
  { \
   0x831f8f13,                                \
   0x7aa8,                                    \
   0x485f,                                    \
   {0xb0, 0x2e, 0x77, 0xc8, 0x81, 0xcc, 0x57, 0x73}}

#define NS_ASYNCSTREAMCOPIER_CONTRACTID \
  "@mozilla.org/network/async-stream-copier;1"
#define NS_ASYNCSTREAMCOPIER_CID              \
  { \
   0xe746a8b1,                                \
   0xc97a,                                    \
   0x4fc5,                                    \
   {0xba, 0xa4, 0x66, 0x60, 0x75, 0x21, 0xbd, 0x08}}

#define NS_INPUTSTREAMPUMP_CONTRACTID "@mozilla.org/network/input-stream-pump;1"
#define NS_INPUTSTREAMPUMP_CID                \
  { \
   0xccd0e960,                                \
   0x7947,                                    \
   0x4635,                                    \
   {0xb7, 0x0e, 0x4c, 0x66, 0x1b, 0x63, 0xd6, 0x75}}

#define NS_INPUTSTREAMCHANNEL_CONTRACTID \
  "@mozilla.org/network/input-stream-channel;1"
#define NS_INPUTSTREAMCHANNEL_CID             \
  { \
   0x6ddb050c,                                \
   0x0d04,                                    \
   0x11d4,                                    \
   {0x98, 0x6e, 0x00, 0xc0, 0x4f, 0xa0, 0xcf, 0x4a}}

#define NS_STREAMLOADER_CONTRACTID "@mozilla.org/network/stream-loader;1"
#define NS_STREAMLOADER_CID                   \
  { \
   0x9879908a,                                \
   0x2972,                                    \
   0x40c0,                                    \
   {0x89, 0x0b, 0xa9, 0x1d, 0xd7, 0xdf, 0xb9, 0x54}}

#define NS_INCREMENTALSTREAMLOADER_CONTRACTID \
  "@mozilla.org/network/incremental-stream-loader;1"
#define NS_INCREMENTALSTREAMLOADER_CID        \
  { \
   0x5d6352a3,                                \
   0xb9c3,                                    \
   0x4fa3,                                    \
   {0x87, 0xaa, 0xb2, 0xa3, 0xc6, 0xe5, 0xa5, 0x01}}

#define NS_UNICHARSTREAMLOADER_CONTRACTID \
  "@mozilla.org/network/unichar-stream-loader;1"
#define NS_UNICHARSTREAMLOADER_CID            \
  { \
   0x9445791f,                                \
   0xfa4c,                                    \
   0x4669,                                    \
   {0xb1, 0x74, 0xdf, 0x50, 0x32, 0xbb, 0x67, 0xb3}}

#define NS_DOWNLOADER_CONTRACTID "@mozilla.org/network/downloader;1"
#define NS_DOWNLOADER_CID                     \
  { \
   0x510a86bb,                                \
   0x6019,                                    \
   0x4ed1,                                    \
   {0xbb, 0x4f, 0x96, 0x5c, 0xff, 0xd2, 0x3e, 0xce}}

#define NS_BACKGROUNDFILESAVEROUTPUTSTREAM_CONTRACTID \
  "@mozilla.org/network/background-file-saver;1?mode=outputstream"
#define NS_BACKGROUNDFILESAVEROUTPUTSTREAM_CID \
  {  \
   0x62147d1e,                                 \
   0xef6a,                                     \
   0x40e8,                                     \
   {0xaa, 0xf8, 0xd0, 0x39, 0xf5, 0xca, 0xaa, 0x81}}

#define NS_BACKGROUNDFILESAVERSTREAMLISTENER_CONTRACTID \
  "@mozilla.org/network/background-file-saver;1?mode=streamlistener"
#define NS_BACKGROUNDFILESAVERSTREAMLISTENER_CID \
  {    \
   0x208de7fc,                                   \
   0xa781,                                       \
   0x4031,                                       \
   {0xbb, 0xae, 0xcc, 0x0d, 0xe5, 0x39, 0xf6, 0x1a}}

#define NS_INCREMENTALDOWNLOAD_CONTRACTID \
  "@mozilla.org/network/incremental-download;1"

#define NS_SYSTEMPROXYSETTINGS_CONTRACTID "@mozilla.org/system-proxy-settings;1"

#define NS_DHCPCLIENT_CONTRACTID "@mozilla.org/dhcp-client;1"

#define NS_STREAMTRANSPORTSERVICE_CONTRACTID \
  "@mozilla.org/network/stream-transport-service;1"
#define NS_STREAMTRANSPORTSERVICE_CID         \
  { \
   0x0885d4f8,                                \
   0xf7b8,                                    \
   0x4cda,                                    \
   {0x90, 0x2e, 0x94, 0xba, 0x38, 0xbc, 0x25, 0x6e}}

#define NS_SOCKETTRANSPORTSERVICE_CONTRACTID \
  "@mozilla.org/network/socket-transport-service;1"
#define NS_SOCKETTRANSPORTSERVICE_CID         \
  { \
   0xad56b25f,                                \
   0xe6bb,                                    \
   0x4db3,                                    \
   {0x9f, 0x7b, 0x5b, 0x7d, 0xb3, 0x3f, 0xd2, 0xb1}}

#define NS_SERVERSOCKET_CONTRACTID "@mozilla.org/network/server-socket;1"
#define NS_SERVERSOCKET_CID                   \
  { \
   0x2ec62893,                                \
   0x3b35,                                    \
   0x48fa,                                    \
   {0xab, 0x1d, 0x5e, 0x68, 0xa9, 0xf4, 0x5f, 0x08}}

#define NS_TLSSERVERSOCKET_CONTRACTID "@mozilla.org/network/tls-server-socket;1"
#define NS_TLSSERVERSOCKET_CID                \
  { \
   0x1813cbb4,                                \
   0xc98e,                                    \
   0x4622,                                    \
   {0x8c, 0x7d, 0x83, 0x91, 0x67, 0xf3, 0xf2, 0x72}}

#define NS_UDPSOCKET_CONTRACTID "@mozilla.org/network/udp-socket;1"
#define NS_UDPSOCKET_CID                      \
  { \
   0xc9f74572,                                \
   0x7b8e,                                    \
   0x4fec,                                    \
   {0xbb, 0x4a, 0x03, 0xc0, 0xd3, 0x02, 0x1b, 0xd6}}

#define NS_LOCALFILEINPUTSTREAM_CONTRACTID \
  "@mozilla.org/network/file-input-stream;1"
#define NS_LOCALFILEINPUTSTREAM_CID           \
  { \
   0xbe9a53ae,                                \
   0xc7e9,                                    \
   0x11d3,                                    \
   {0x8c, 0xda, 0x00, 0x60, 0xb0, 0xfc, 0x14, 0xa3}}

#define NS_LOCALFILEOUTPUTSTREAM_CONTRACTID \
  "@mozilla.org/network/file-output-stream;1"
#define NS_LOCALFILEOUTPUTSTREAM_CID          \
  { \
   0xc272fee0,                                \
   0xc7e9,                                    \
   0x11d3,                                    \
   {0x8c, 0xda, 0x00, 0x60, 0xb0, 0xfc, 0x14, 0xa3}}

#define NS_LOCALFILERANDOMACCESSSTREAM_CONTRACTID \
  "@mozilla.org/network/file-random-access-stream;1"
#define NS_LOCALFILERANDOMACCESSSTREAM_CID    \
  { \
   0x648705e9,                                \
   0x757a,                                    \
   0x4d4b,                                    \
   {0xa5, 0xbF, 0x02, 0x48, 0xe5, 0x12, 0xc3, 0x09}}

#define NS_BUFFEREDINPUTSTREAM_CONTRACTID \
  "@mozilla.org/network/buffered-input-stream;1"
#define NS_BUFFEREDINPUTSTREAM_CID            \
  { \
   0x9226888e,                                \
   0xda08,                                    \
   0x11d3,                                    \
   {0x8c, 0xda, 0x00, 0x60, 0xb0, 0xfc, 0x14, 0xa3}}

#define NS_BUFFEREDOUTPUTSTREAM_CONTRACTID \
  "@mozilla.org/network/buffered-output-stream;1"
#define NS_BUFFEREDOUTPUTSTREAM_CID           \
  { \
   0x9868b4ce,                                \
   0xda08,                                    \
   0x11d3,                                    \
   {0x8c, 0xda, 0x00, 0x60, 0xb0, 0xfc, 0x14, 0xa3}}

#define NS_ATOMICLOCALFILEOUTPUTSTREAM_CONTRACTID \
  "@mozilla.org/network/atomic-file-output-stream;1"
#define NS_ATOMICLOCALFILEOUTPUTSTREAM_CID    \
  { \
   0x6EAE857E,                                \
   0x4BA9,                                    \
   0x11E3,                                    \
   {0x9b, 0x39, 0xb4, 0x03, 0x61, 0x88, 0x70, 0x9b}}

#define NS_SAFELOCALFILEOUTPUTSTREAM_CONTRACTID \
  "@mozilla.org/network/safe-file-output-stream;1"
#define NS_SAFELOCALFILEOUTPUTSTREAM_CID      \
  { \
   0xa181af0d,                                \
   0x68b8,                                    \
   0x4308,                                    \
   {0x94, 0xdb, 0xd4, 0xf8, 0x59, 0x05, 0x82, 0x15}}

#define NS_URICLASSIFIERSERVICE_CONTRACTID "@mozilla.org/uriclassifierservice"

#define NS_CAPTIVEPORTAL_CONTRACTID \
  "@mozilla.org/network/captive-portal-service;1"
#define NS_CAPTIVEPORTAL_CID                  \
  { \
   0xbdbe0555,                                \
   0xfc3d,                                    \
   0x4f7b,                                    \
   {0x92, 0x05, 0xc3, 0x09, 0xce, 0xb2, 0xd6, 0x41}}

#define NS_NETWORKCONNECTIVITYSERVICE_CONTRACTID \
  "@mozilla.org/network/network-connectivity-service;1"
#define NS_NETWORKCONNECTIVITYSERVICE_CID     \
  { \
   0x2693457e,                                \
   0x3ba5,                                    \
   0x4455,                                    \
   {0x99, 0x1f, 0x53, 0x50, 0x94, 0x6a, 0xdb, 0x12}}


#define NS_CACHESERVICE_CONTRACTID "@mozilla.org/network/cache-service;1"
#define NS_CACHESERVICE_CID                   \
  { \
   0x6c84aec9,                                \
   0x29a5,                                    \
   0x4264,                                    \
   {0x8f, 0xbc, 0xbe, 0xe8, 0xf9, 0x22, 0xea, 0x67}}


#define NS_HTTPPROTOCOLHANDLER_CID            \
  { \
   0x4f47e42e,                                \
   0x4d23,                                    \
   0x4dd3,                                    \
   {0xbf, 0xda, 0xeb, 0x29, 0x25, 0x5e, 0x9e, 0xa3}}

#define NS_HTTPSPROTOCOLHANDLER_CID           \
  { \
   0xdccbe7e4,                                \
   0x7750,                                    \
   0x466b,                                    \
   {0xa5, 0x57, 0x5e, 0xa3, 0x6c, 0x8f, 0xf2, 0x4e}}

#define NS_HTTPBASICAUTH_CID                  \
  { \
   0xfca3766a,                                \
   0x434a,                                    \
   0x4ae7,                                    \
   {0x83, 0xcf, 0x09, 0x09, 0xe1, 0x8a, 0x09, 0x3a}}

#define NS_HTTPDIGESTAUTH_CID                 \
  { \
   0x17491ba4,                                \
   0x1dd2,                                    \
   0x11b2,                                    \
   {0xaa, 0xe3, 0xde, 0x6b, 0x92, 0xda, 0xb6, 0x20}}

#define NS_HTTPNTLMAUTH_CID                   \
  { \
   0xbbef8185,                                \
   0xc628,                                    \
   0x4cc1,                                    \
   {0xb5, 0x3e, 0xe6, 0x1e, 0x74, 0xc2, 0x45, 0x1a}}

#define NS_HTTPAUTHMANAGER_CONTRACTID "@mozilla.org/network/http-auth-manager;1"
#define NS_HTTPAUTHMANAGER_CID                \
  { \
   0x36b63ef3,                                \
   0xe0fa,                                    \
   0x4c49,                                    \
   {0x9f, 0xd4, 0xe0, 0x65, 0xe8, 0x55, 0x68, 0xf4}}

#define NS_HTTPACTIVITYDISTRIBUTOR_CONTRACTID \
  "@mozilla.org/network/http-activity-distributor;1"
#define NS_HTTPACTIVITYDISTRIBUTOR_CID        \
  { \
   0x15629ada,                                \
   0xa41c,                                    \
   0x4a09,                                    \
   {0x96, 0x1f, 0x65, 0x53, 0xcd, 0x60, 0xb1, 0xa2}}

#define NS_THROTTLEQUEUE_CONTRACTID "@mozilla.org/network/throttlequeue;1"
#define NS_THROTTLEQUEUE_CID                  \
  { \
   0x4c39159c,                                \
   0xcd90,                                    \
   0x4dd3,                                    \
   {0x97, 0xa7, 0x06, 0xaf, 0x5e, 0x6d, 0x84, 0xc4}}


#define NS_RESPROTOCOLHANDLER_CID             \
  { \
   0xe64f152a,                                \
   0x9f07,                                    \
   0x11d3,                                    \
   {0x8c, 0xda, 0x00, 0x60, 0xb0, 0xfc, 0x14, 0xa3}}

#define NS_EXTENSIONPROTOCOLHANDLER_CID       \
  { \
   0xaea16cd0,                                \
   0xf020,                                    \
   0x4138,                                    \
   {0xb0, 0x68, 0x07, 0x16, 0xc4, 0xa1, 0x5b, 0x5a}}

#define NS_SUBSTITUTINGURL_CID \
  {0xdea9657c, 0x18cf, 0x4984, {0xbd, 0xe9, 0xcc, 0xef, 0x5d, 0x8a, 0xb4, 0x73}}

#define NS_SUBSTITUTINGURLMUTATOR_CID \
  {0xb3cfeb91, 0x332a, 0x46c9, {0xad, 0x97, 0x93, 0xff, 0x39, 0x84, 0x14, 0x94}}

#define NS_SUBSTITUTINGJARURI_CID             \
  { \
   0x50d50ddf,                                \
   0xf16a,                                    \
   0x4652,                                    \
   {0x87, 0x05, 0x93, 0x6b, 0x19, 0xc3, 0x76, 0x3b}}


#define NS_FILEPROTOCOLHANDLER_CID            \
  { \
   0xfbc81170,                                \
   0x1f69,                                    \
   0x11d3,                                    \
   {0x93, 0x44, 0x00, 0x10, 0x4b, 0xa0, 0xfd, 0x40}}


#define NS_DATAPROTOCOLHANDLER_CID              \
  { \
   0xb6ed3030,                                  \
   0x6183,                                      \
   0x11d3,                                      \
   {0xa1, 0x78, 0x00, 0x50, 0x04, 0x1c, 0xaf, 0x44}}


#define NS_VIEWSOURCEHANDLER_CID                  \
  { \
   0x9c7ec5d1,                                    \
   0x23f9,                                        \
   0x11d5,                                        \
   {0xae, 0xa8, 0x8f, 0xcc, 0x07, 0x93, 0xe9, 0x7f}}


#define NS_WEBSOCKETPROTOCOLHANDLER_CID         \
  { \
   0xdc01db59,                                  \
   0xa513,                                      \
   0x4c90,                                      \
   {0x82, 0x4b, 0x08, 0x5c, 0xce, 0x06, 0xc0, 0xaa}}

#define NS_WEBSOCKETSSLPROTOCOLHANDLER_CID      \
  { \
   0xdc01dbbb,                                  \
   0xa5bb,                                      \
   0x4cbb,                                      \
   {0x82, 0xbb, 0x08, 0x5c, 0xce, 0x06, 0xc0, 0xbb}}


#define NS_ABOUTPROTOCOLHANDLER_CID           \
  { \
   0x9e3b6c90,                                \
   0x2f75,                                    \
   0x11d3,                                    \
   {0x8c, 0xd0, 0x00, 0x60, 0xb0, 0xfc, 0x14, 0xa3}}

#define NS_SAFEABOUTPROTOCOLHANDLER_CID       \
  { \
   0x1423e739,                                \
   0x782c,                                    \
   0x4081,                                    \
   {0xb5, 0xd8, 0xfe, 0x6f, 0xba, 0x68, 0xc0, 0xef}}


#define NS_DNSSERVICE_CONTRACTID "@mozilla.org/network/dns-service;1"
#define NS_DNSSERVICE_CID                     \
  { \
   0xb0ff4572,                                \
   0xdae4,                                    \
   0x4bef,                                    \
   {0xa0, 0x92, 0x83, 0xc1, 0xb8, 0x8f, 0x6b, 0xe9}}

#define NS_IDNSERVICE_CONTRACTID "@mozilla.org/network/idn-service;1"
#define NS_IDNSERVICE_CID                     \
  { \
   0x62b778a6,                                \
   0xbce3,                                    \
   0x456b,                                    \
   {0x8c, 0x31, 0x28, 0x65, 0xfb, 0xb6, 0x8c, 0x91}}

#define NS_EFFECTIVETLDSERVICE_CONTRACTID \
  "@mozilla.org/network/effective-tld-service;1"
#define NS_EFFECTIVETLDSERVICE_CID            \
  { \
   0xcb9abbae,                                \
   0x66b6,                                    \
   0x4609,                                    \
   {0x85, 0x94, 0x5c, 0x4f, 0xf3, 0x00, 0x88, 0x8e}}


#define NS_MIMEHEADERPARAM_CID \
  {0x1f4dbcf7, 0x245c, 0x4c8c, {0x94, 0x3d, 0x8a, 0x1d, 0xa0, 0x49, 0x5e, 0x8a}}

#define NS_MIMEHEADERPARAM_CONTRACTID "@mozilla.org/network/mime-hdrparam;1"


#define NS_SOCKSSOCKETPROVIDER_CID            \
  { \
   0x8dbe7246,                                \
   0x1dd2,                                    \
   0x11b2,                                    \
   {0x9b, 0x8f, 0xb9, 0xa8, 0x49, 0xe4, 0x40, 0x3a}}

#define NS_SOCKS4SOCKETPROVIDER_CID           \
  { \
   0xf7c9f5f4,                                \
   0x4451,                                    \
   0x41c3,                                    \
   {0xa2, 0x8a, 0x5b, 0xa2, 0x44, 0x7f, 0xba, 0xce}}

#define NS_UDPSOCKETPROVIDER_CID              \
  { \
   0x320706d2,                                \
   0x2e81,                                    \
   0x42c6,                                    \
   {0x89, 0xc3, 0x8d, 0x83, 0xb1, 0x7d, 0x3f, 0xb4}}

#define NS_DASHBOARD_CONTRACTID "@mozilla.org/network/dashboard;1"
#define NS_DASHBOARD_CID                     \
  { \
   0xc79eb3c6,                               \
   0x091a,                                   \
   0x45a6,                                   \
   {0x85, 0x44, 0x5a, 0x8d, 0x1a, 0xb7, 0x95, 0x37}}


#define NS_COOKIEMANAGER_CONTRACTID "@mozilla.org/cookiemanager;1"
#define NS_COOKIEMANAGER_CID                  \
  { \
   0xaaab6710,                                \
   0x0f2c,                                    \
   0x11d5,                                    \
   {0xa5, 0x3b, 0x00, 0x10, 0xa4, 0x01, 0xeb, 0x10}}

#define NS_COOKIESERVICE_CONTRACTID "@mozilla.org/cookieService;1"
#define NS_COOKIESERVICE_CID                  \
  { \
   0xc375fa80,                                \
   0x150f,                                    \
   0x11d6,                                    \
   {0xa6, 0x18, 0x00, 0x10, 0xa4, 0x01, 0xeb, 0x10}}

#ifdef NECKO_WIFI
#  define NS_WIFI_MONITOR_CONTRACTID "@mozilla.org/wifi/monitor;1"

#  define NS_WIFI_MONITOR_COMPONENT_CID \
    {0x3FF8FB9F,                        \
     0xEE63,                            \
     0x48DF,                            \
     {0x89, 0xF0, 0xDA, 0xCE, 0x02, 0x42, 0xFD, 0x82}}
#endif


#define NS_STREAMCONVERTERSERVICE_CONTRACTID "@mozilla.org/streamConverters;1"
#define NS_STREAMCONVERTERSERVICE_CID         \
  { \
   0x892ffeb0,                                \
   0x3f80,                                    \
   0x11d3,                                    \
   {0xa1, 0x6c, 0x00, 0x50, 0x04, 0x1c, 0xaf, 0x44}}

#define NS_BINARYDETECTOR_CONTRACTID "@mozilla.org/network/binary-detector;1"


#define NS_NETWORK_LINK_SERVICE_CID \
  {0x75a500a2, 0x0030, 0x40f7, {0x86, 0xf8, 0x63, 0xf2, 0x25, 0xb9, 0x40, 0xae}}


#define NS_NETWORK_LINK_SERVICE_CONTRACTID \
  "@mozilla.org/network/network-link-service;1"

#define NS_AUTHPROMPT_ADAPTER_FACTORY_CONTRACTID \
  "@mozilla.org/network/authprompt-adapter-factory;1"

#define NS_CRYPTO_HASH_CONTRACTID "@mozilla.org/security/hash;1"

#define NS_CHANNEL_EVENT_SINK_CATEGORY "net-channel-event-sinks"

#define NS_CONTENT_SNIFFER_CATEGORY "net-content-sniffers"

#define NS_DATA_SNIFFER_CATEGORY "content-sniffing-services"

#define NS_ORB_SNIFFER_CATEGORY "orb-content-sniffers"

#define NS_CONTENT_AND_ORB_SNIFFER_CATEGORY "net-and-orb-content-sniffers"

#define NS_NSS_ERRORS_SERVICE_CONTRACTID "@mozilla.org/nss_errors_service;1"

#define NS_NSILOADCONTEXTINFOFACTORY_CONTRACTID \
  "@mozilla.org/load-context-info-factory;1"
#define NS_NSILOADCONTEXTINFOFACTORY_CID      \
  { \
   0x62d4b190,                                \
   0x3642,                                    \
   0x4450,                                    \
   {0xb0, 0x19, 0xd1, 0xc1, 0xfb, 0xa5, 0x60, 0x25}}

#endif  // nsNetCID_h_
