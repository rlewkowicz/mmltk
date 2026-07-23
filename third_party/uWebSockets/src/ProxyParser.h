
#ifndef UWS_PROXY_PARSER_H
#define UWS_PROXY_PARSER_H

#ifdef UWS_WITH_PROXY

namespace uWS {

struct proxy_hdr_v2 {
    uint8_t sig[12];
    uint8_t ver_cmd;
    uint8_t fam;
    uint16_t len;
};

union proxy_addr {
    struct {
        uint32_t src_addr;
        uint32_t dst_addr;
        uint16_t src_port;
        uint16_t dst_port;
    } ipv4_addr;
    struct {
        uint8_t src_addr[16];
        uint8_t dst_addr[16];
        uint16_t src_port;
        uint16_t dst_port;
    } ipv6_addr;
};

template <typename T>
T _cond_byte_swap(T value) {
    uint32_t endian_test = 1;
    if (*((char*)&endian_test)) {
        union {
            T i;
            uint8_t b[sizeof(T)];
        } src = {value}, dst;

        for (unsigned int i = 0; i < sizeof(value); i++) {
            dst.b[i] = src.b[sizeof(value) - 1 - i];
        }

        return dst.i;
    }
    return value;
}

struct ProxyParser {
   private:
    union proxy_addr addr;

    uint8_t family = 0;

   public:
    std::string_view getSourceAddress() {
        if (family == 0) {
            return {};
        }

        if ((family & 0xf0) >> 4 == 1) {
            return {(char*)&addr.ipv4_addr.src_addr, 4};
        } else {
            return {(char*)&addr.ipv6_addr.src_addr, 16};
        }
    }

    unsigned int getSourcePort() {
        if (family == 0) {
            return {};
        }

        if ((family & 0xf0) >> 4 == 1) {
            return addr.ipv4_addr.src_port;
        } else {
            return addr.ipv6_addr.src_port;
        }
    }

    std::pair<bool, unsigned int> parse(std::string_view data) {
        if (data.length() < 4) {
            return {false, 0};
        }

        if (memcmp(data.data(), "\r\n\r\n", 4)) {
            return {true, 0};
        }

        if (data.length() < 16) {
            return {false, 0};
        }

        struct proxy_hdr_v2 header;
        memcpy(&header, data.data(), 16);

        if (memcmp(header.sig, "\x0D\x0A\x0D\x0A\x00\x0D\x0A\x51\x55\x49\x54\x0A", 12)) {
            return {false, 0};
        }

        if ((header.ver_cmd & 0xf0) >> 4 != 2) {
            return {false, 0};
        }

        uint16_t hostLength = _cond_byte_swap<uint16_t>(header.len);

        if (data.length() < 16u + hostLength) {
            return {false, 0};
        }

        if (sizeof(proxy_addr) < hostLength) {
            return {false, 0};
        }

        family = header.fam;

        memcpy(&addr, data.data() + 16, hostLength);

        return {true, 16 + hostLength};
    }
};

}  

#endif

#endif  // UWS_PROXY_PARSER_H