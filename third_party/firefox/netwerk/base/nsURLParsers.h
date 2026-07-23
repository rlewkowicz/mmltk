/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(nsURLParsers_h_)
#define nsURLParsers_h_

#include "nsIURLParser.h"


class nsBaseURLParser : public nsIURLParser {
 public:
  NS_DECL_NSIURLPARSER

  nsBaseURLParser() = default;

 protected:
  virtual void ParseAfterScheme(const char* spec, int32_t specLen,
                                uint32_t* authPos, int32_t* authLen,
                                uint32_t* pathPos, int32_t* pathLen) = 0;
};


class nsNoAuthURLParser final : public nsBaseURLParser {
  ~nsNoAuthURLParser() = default;

 public:
  NS_DECL_THREADSAFE_ISUPPORTS


  NS_IMETHOD ParseAuthority(const char* auth, int32_t authLen,
                            uint32_t* usernamePos, int32_t* usernameLen,
                            uint32_t* passwordPos, int32_t* passwordLen,
                            uint32_t* hostnamePos, int32_t* hostnameLen,
                            int32_t* port) override;

  void ParseAfterScheme(const char* spec, int32_t specLen, uint32_t* authPos,
                        int32_t* authLen, uint32_t* pathPos,
                        int32_t* pathLen) override;
};


class nsAuthURLParser : public nsBaseURLParser {
 protected:
  virtual ~nsAuthURLParser() = default;

 public:
  NS_DECL_THREADSAFE_ISUPPORTS

  NS_IMETHOD ParseAuthority(const char* auth, int32_t authLen,
                            uint32_t* usernamePos, int32_t* usernameLen,
                            uint32_t* passwordPos, int32_t* passwordLen,
                            uint32_t* hostnamePos, int32_t* hostnameLen,
                            int32_t* port) override;

  NS_IMETHOD ParseUserInfo(const char* userinfo, int32_t userinfoLen,
                           uint32_t* usernamePos, int32_t* usernameLen,
                           uint32_t* passwordPos,
                           int32_t* passwordLen) override;

  NS_IMETHOD ParseServerInfo(const char* serverinfo, int32_t serverinfoLen,
                             uint32_t* hostnamePos, int32_t* hostnameLen,
                             int32_t* port) override;

  void ParseAfterScheme(const char* spec, int32_t specLen, uint32_t* authPos,
                        int32_t* authLen, uint32_t* pathPos,
                        int32_t* pathLen) override;
};


class nsStdURLParser : public nsAuthURLParser {
  virtual ~nsStdURLParser() = default;

 public:
  void ParseAfterScheme(const char* spec, int32_t specLen, uint32_t* authPos,
                        int32_t* authLen, uint32_t* pathPos,
                        int32_t* pathLen) override;
};

#endif
