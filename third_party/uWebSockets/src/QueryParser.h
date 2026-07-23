
#ifndef UWS_QUERYPARSER_H
#define UWS_QUERYPARSER_H

#include <string_view>

namespace uWS {

static inline std::string_view getDecodedQueryValue(std::string_view key, std::string_view rawQuery) {
    if (!key.length()) {
        return {};
    }

    std::string_view queryString = rawQuery;

    while (queryString.length()) {
        std::string_view statement = queryString.substr(1, queryString.find('&', 1) - 1);

        if (statement.length() && statement[0] == key[0]) {
            auto equality = statement.find('=');
            if (equality != std::string_view::npos) {
                std::string_view statementKey = statement.substr(0, equality);
                std::string_view statementValue = statement.substr(equality + 1);

                if (key == statementKey) {
                    char* in = (char*)statementValue.data();

                    unsigned int out = 0;

                    for (unsigned int i = 0; i < statementValue.length() && in[i]; i++) {
                        if (in[i] == '%') {
                            if (i + 2 >= statementValue.length()) {
                                return {};
                            }

                            int hex1 = in[i + 1] - '0';
                            if (hex1 > 9) {
                                hex1 &= 223;
                                hex1 -= 7;
                            }

                            int hex2 = in[i + 2] - '0';
                            if (hex2 > 9) {
                                hex2 &= 223;
                                hex2 -= 7;
                            }

                            *((unsigned char*)&in[out]) = (unsigned char)(hex1 * 16 + hex2);
                            i += 2;
                        } else {
                            if (in[i] == '+') {
                                in[out] = ' ';
                            } else {
                                in[out] = in[i];
                            }
                        }

                        out++;
                    }

                    if (out < statementValue.length()) {
                        in[out] = 0;
                    }

                    return statementValue.substr(0, out);
                }
            } else {
                return {nullptr, 0};
            }
        }

        queryString.remove_prefix(statement.length() + 1);
    }

    return {nullptr, 0};
}

}  

#endif
