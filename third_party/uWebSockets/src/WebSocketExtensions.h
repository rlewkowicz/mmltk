/*
 * Authored by Alex Hultman, 2018-2021.
 * Intellectual property of third-party.

 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at

 *     http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef UWS_WEBSOCKETEXTENSIONS_H
#define UWS_WEBSOCKETEXTENSIONS_H

#include <climits>
#include <cctype>
#include <string>
#include <string_view>
#include <tuple>

namespace uWS {

enum ExtensionTokens {
    TOK_PERMESSAGE_DEFLATE = 1838,
    TOK_SERVER_NO_CONTEXT_TAKEOVER = 2807,
    TOK_CLIENT_NO_CONTEXT_TAKEOVER = 2783,
    TOK_SERVER_MAX_WINDOW_BITS = 2372,
    TOK_CLIENT_MAX_WINDOW_BITS = 2348,
    TOK_X_WEBKIT_DEFLATE_FRAME = 2149,
    TOK_NO_CONTEXT_TAKEOVER = 2049,
    TOK_MAX_WINDOW_BITS = 1614
};

struct ExtensionsParser {
   private:
    int* lastInteger = nullptr;

   public:
    bool perMessageDeflate = false;
    bool serverNoContextTakeover = false;
    bool clientNoContextTakeover = false;
    int serverMaxWindowBits = 0;
    int clientMaxWindowBits = 0;

    bool xWebKitDeflateFrame = false;
    bool noContextTakeover = false;
    int maxWindowBits = 0;

    int getToken(const char*& in, const char* stop) {
        while (in != stop && !isalnum(*in)) {
            in++;
        }

        static_assert(SHRT_MIN > INT_MIN, "Integer overflow fix is invalid for this platform, report this as a bug!");

        int hashedToken = 0;
        while (in != stop && (isalnum(*in) || *in == '-' || *in == '_')) {
            if (isdigit(*in)) {
                if (hashedToken > SHRT_MIN && hashedToken < SHRT_MAX) {
                    hashedToken = hashedToken * 10 - (*in - '0');
                }
            } else {
                hashedToken += *in;
            }
            in++;
        }
        return hashedToken;
    }

    ExtensionsParser(const char* data, size_t length) {
        const char* stop = data + length;
        int token = 1;

        for (; token && token != TOK_PERMESSAGE_DEFLATE && token != TOK_X_WEBKIT_DEFLATE_FRAME;
             token = getToken(data, stop))
            ;

        perMessageDeflate = (token == TOK_PERMESSAGE_DEFLATE);
        xWebKitDeflateFrame = (token == TOK_X_WEBKIT_DEFLATE_FRAME);

        while ((token = getToken(data, stop))) {
            switch (token) {
                case TOK_X_WEBKIT_DEFLATE_FRAME:
                    return;
                case TOK_NO_CONTEXT_TAKEOVER:
                    noContextTakeover = true;
                    break;
                case TOK_MAX_WINDOW_BITS:
                    maxWindowBits = 1;
                    lastInteger = &maxWindowBits;
                    break;
                case TOK_PERMESSAGE_DEFLATE:
                    return;
                case TOK_SERVER_NO_CONTEXT_TAKEOVER:
                    serverNoContextTakeover = true;
                    break;
                case TOK_CLIENT_NO_CONTEXT_TAKEOVER:
                    clientNoContextTakeover = true;
                    break;
                case TOK_SERVER_MAX_WINDOW_BITS:
                    serverMaxWindowBits = 1;
                    lastInteger = &serverMaxWindowBits;
                    break;
                case TOK_CLIENT_MAX_WINDOW_BITS:
                    clientMaxWindowBits = 1;
                    lastInteger = &clientMaxWindowBits;
                    break;
                default:
                    if (token < 0 && lastInteger) {
                        *lastInteger = -token;
                    }
                    break;
            }
        }
    }
};

static inline std::tuple<bool, int, int, std::string_view> negotiateCompression(bool wantCompression,
                                                                                int wantedCompressionWindow,
                                                                                int wantedInflationWindow,
                                                                                std::string_view offer) {
    if (!wantCompression) {
        return {false, 0, 0, ""};
    }

    ExtensionsParser ep(offer.data(), offer.length());

    static thread_local std::string response;
    response = "";

    int compressionWindow = wantedCompressionWindow;
    int inflationWindow = wantedInflationWindow;
    bool compression = false;

    if (ep.xWebKitDeflateFrame) {
        compression = true;
        response = "x-webkit-deflate-frame";

        if (ep.noContextTakeover) {
#ifndef UWS_ALLOW_SHARED_AND_DEDICATED_COMPRESSOR_MIX
            if (wantedCompressionWindow != 0) {
                return {false, 0, 0, ""};
            }
#endif

            compressionWindow = 0;
        }

        if (ep.maxWindowBits && ep.maxWindowBits < compressionWindow) {
            compressionWindow = ep.maxWindowBits;
#ifndef UWS_ALLOW_8_WINDOW_BITS
            if (compressionWindow == 8) {
                return {false, 0, 0, ""};
            }
#endif
        }

        if (wantedInflationWindow < 15) {
            if (!wantedInflationWindow) {
                response += "; no_context_takeover";
            } else {
                response += "; max_window_bits=" + std::to_string(wantedInflationWindow);
            }
        }
    } else if (ep.perMessageDeflate) {
        compression = true;
        response = "permessage-deflate";

        if (ep.clientNoContextTakeover) {
            inflationWindow = 0;
        } else if (ep.clientMaxWindowBits && ep.clientMaxWindowBits != 1) {
            inflationWindow = std::min<int>(ep.clientMaxWindowBits, inflationWindow);
        }

        if (inflationWindow < 15) {
            if (!inflationWindow || !ep.clientMaxWindowBits) {
                response += "; client_no_context_takeover";
                inflationWindow = 0;
            } else {
                response += "; client_max_window_bits=" + std::to_string(inflationWindow);
            }
        }

        if (ep.serverNoContextTakeover) {
#ifdef UWS_ALLOW_SHARED_AND_DEDICATED_COMPRESSOR_MIX
            compressionWindow = 0;
#endif
        } else if (ep.serverMaxWindowBits) {
            compressionWindow = std::min<int>(ep.serverMaxWindowBits, compressionWindow);
#ifndef UWS_ALLOW_8_WINDOW_BITS
            if (compressionWindow == 8) {
                compressionWindow = 9;
            }
#endif
        }

        if (compressionWindow < 15) {
            if (!compressionWindow) {
                response += "; server_no_context_takeover";
            } else {
                response += "; server_max_window_bits=" + std::to_string(compressionWindow);
            }
        }
    }

    if ((compressionWindow && compressionWindow < 8) || compressionWindow > 15 ||
        (inflationWindow && inflationWindow < 8) || inflationWindow > 15) {
        return {false, 0, 0, ""};
    }

    return {compression, compressionWindow, inflationWindow, response};
}

}  // namespace uWS

#endif  // UWS_WEBSOCKETEXTENSIONS_H
