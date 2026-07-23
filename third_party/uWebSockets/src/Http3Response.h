extern "C" {
#include "quic.h"
}

#include "Http3ResponseData.h"

namespace uWS {

struct Http3Response {
    void close() {}

    void endWithoutBody(std::optional<size_t> reportedContentLength = std::nullopt, bool closeConnection = false) {}

    Http3Response* writeStatus(std::string_view status) {
        Http3ResponseData* responseData = (Http3ResponseData*)us_quic_stream_ext((us_quic_stream_t*)this);

        if (responseData->headerOffset == 0) {
            us_quic_socket_context_set_header(nullptr, 0, (char*)":status", 7, status.data(), status.length());
            responseData->headerOffset = 1;
        }

        return this;
    }

    Http3Response* writeHeader(std::string_view key, std::string_view value) {
        Http3ResponseData* responseData = (Http3ResponseData*)us_quic_stream_ext((us_quic_stream_t*)this);

        writeStatus("200 OK");

        us_quic_socket_context_set_header(nullptr, responseData->headerOffset++, key.data(), key.length(), value.data(),
                                          value.length());

        return this;
    }

    std::pair<bool, bool> tryEnd(std::string_view data, uintmax_t totalSize = 0) {
        Http3ResponseData* responseData = (Http3ResponseData*)us_quic_stream_ext((us_quic_stream_t*)this);

        writeStatus("200 OK");

        us_quic_socket_context_send_headers(nullptr, (us_quic_stream_t*)this, responseData->headerOffset,
                                            data.length() > 0);

        unsigned int written = us_quic_stream_write((us_quic_stream_t*)this, (char*)data.data(), (int)data.length());

        if (written == data.length()) {
            return {true, true};
        } else {
            responseData->offset = written;

            return {true, false};
        }

        return {true, true};
    }

    Http3Response* write(std::string_view data) {
        return this;
    }

    void end(std::string_view data = {}, bool closeConnection = false) {
        Http3ResponseData* responseData = (Http3ResponseData*)us_quic_stream_ext((us_quic_stream_t*)this);

        writeStatus("200 OK");

        us_quic_socket_context_send_headers(nullptr, (us_quic_stream_t*)this, responseData->headerOffset,
                                            data.length() > 0);

        unsigned int written = us_quic_stream_write((us_quic_stream_t*)this, (char*)data.data(), (int)data.length());

        if (written != data.length()) {
            responseData->backpressure.append(data.data() + written, data.length() - written);
        } else {
            us_quic_stream_shutdown((us_quic_stream_t*)this);
        }
    }

    Http3Response* onAborted(MoveOnlyFunction<void()>&& handler) {
        Http3ResponseData* responseData = (Http3ResponseData*)us_quic_stream_ext((us_quic_stream_t*)this);

        responseData->onAborted = std::move(handler);
        return this;
    }

    Http3Response* onData(MoveOnlyFunction<void(std::string_view, bool)>&& handler) {
        Http3ResponseData* responseData = (Http3ResponseData*)us_quic_stream_ext((us_quic_stream_t*)this);

        responseData->onData = std::move(handler);
        return this;
    }

    Http3Response* onWritable(MoveOnlyFunction<bool(uintmax_t)>&& handler) {
        Http3ResponseData* responseData = (Http3ResponseData*)us_quic_stream_ext((us_quic_stream_t*)this);

        responseData->onWritable = std::move(handler);
        return this;
    }
};

}  