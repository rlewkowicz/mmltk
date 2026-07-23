
#ifndef UWS_MULTIPART_H
#define UWS_MULTIPART_H

#include "MessageParser.h"

#include <string_view>
#include <optional>
#include <cstring>
#include <utility>
#include <cctype>

namespace uWS {

struct ParameterParser {
    ParameterParser(std::string_view line) {
        remainingLine = line;
    }

    std::pair<std::string_view, std::string_view> getKeyValue() {
        auto key = getToken();
        auto op = getToken();

        if (!op.length()) {
            return {key, ""};
        }

        if (op[0] != ';') {
            auto value = getToken();
            getToken();
            return {key, value};
        }

        return {key, ""};
    }

   private:
    std::string_view remainingLine;

    std::string_view getToken() {
        while (remainingLine.length() && isspace(remainingLine[0])) {
            remainingLine.remove_prefix(1);
        }

        if (!remainingLine.length()) {
            return {};
        } else {
            if (remainingLine[0] == ';' || remainingLine[0] == '=') {
                auto op = remainingLine.substr(0, 1);
                remainingLine.remove_prefix(1);
                return op;
            } else {
                if (remainingLine[0] == '\"') {
                    remainingLine.remove_prefix(1);
                    auto quote = remainingLine;
                    int quoteLength = 0;

                    while (remainingLine.length() && remainingLine[0] != '\"') {
                        remainingLine.remove_prefix(1);
                        quoteLength++;
                    }

                    if (!remainingLine.length()) {
                        return {};
                    }

                    remainingLine.remove_prefix(1);
                    return quote.substr(0, quoteLength);
                } else {
                    std::string_view token = remainingLine;

                    int tokenLength = 0;
                    while (remainingLine.length() && remainingLine[0] != ';' && remainingLine[0] != '=' &&
                           !isspace(remainingLine[0])) {
                        remainingLine.remove_prefix(1);
                        tokenLength++;
                    }

                    return token.substr(0, tokenLength);
                }
            }
        }

        return "";
    }
};

struct MultipartParser {
    char prependedBoundaryBuffer[72];
    std::string_view prependedBoundary;
    std::string_view remainingBody;
    bool first = true;

    MultipartParser(std::string_view contentType) {
        if (contentType.length() < 10 || contentType.substr(0, 10) != "multipart/") {
            return;
        }

        auto equalToken = contentType.find('=', 10);
        if (equalToken != std::string_view::npos) {
            std::string_view boundary = contentType.substr(equalToken + 1);
            if (!boundary.length() || boundary.length() > 70) {
                return;
            }

            prependedBoundaryBuffer[0] = prependedBoundaryBuffer[1] = '-';
            memcpy(&prependedBoundaryBuffer[2], boundary.data(), boundary.length());

            prependedBoundary = {prependedBoundaryBuffer, boundary.length() + 2};
        }
    }

    bool isValid() {
        return prependedBoundary.length() != 0;
    }

    void setBody(std::string_view body) {
        remainingBody = body;
    }

    std::optional<std::string_view> getNextPart(std::pair<std::string_view, std::string_view>* headers) {
        if (remainingBody.length() < prependedBoundary.length()) {
            return std::nullopt;
        }

        if (first) {
            auto nextBoundary = remainingBody.find(prependedBoundary);
            if (nextBoundary == std::string_view::npos) {
                return std::nullopt;
            }

            remainingBody.remove_prefix(nextBoundary + prependedBoundary.length());
            first = false;
        }

        auto nextEndBoundary = remainingBody.find(prependedBoundary);
        if (nextEndBoundary == std::string_view::npos) {
            return std::nullopt;
        }

        std::string_view part = remainingBody.substr(0, nextEndBoundary);
        remainingBody.remove_prefix(nextEndBoundary + prependedBoundary.length());

        if (part.length() < 4) {
            return std::nullopt;
        }
        part.remove_prefix(2);
        part.remove_suffix(2);

        memset((char*)part.data() + part.length(), '\r', 1);

        int consumed = getHeaders((char*)part.data(), (char*)part.data() + part.length(), headers);

        if (!consumed) {
            return std::nullopt;
        }

        part.remove_prefix(consumed);

        return part;
    }
};

}  

#endif
