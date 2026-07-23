
#ifndef UWS_HTTP_ERRORS
#define UWS_HTTP_ERRORS

#include <string_view>

namespace uWS {
enum HttpError {
    HTTP_ERROR_505_HTTP_VERSION_NOT_SUPPORTED = 1,
    HTTP_ERROR_431_REQUEST_HEADER_FIELDS_TOO_LARGE = 2,
    HTTP_ERROR_400_BAD_REQUEST = 3
};

#ifndef UWS_HTTPRESPONSE_NO_WRITEMARK

static const std::string_view httpErrorResponses[] = {
    "",
    "HTTP/1.1 505 HTTP Version Not Supported\r\nConnection: close\r\n\r\n<h1>HTTP Version Not Supported</h1><p>This "
    "server does not support HTTP/1.0.</p><hr><i>uWebSockets/20 Server</i>",
    "HTTP/1.1 431 Request Header Fields Too Large\r\nConnection: close\r\n\r\n<h1>Request Header Fields Too "
    "Large</h1><hr><i>uWebSockets/20 Server</i>",
    "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n<h1>Bad Request</h1><hr><i>uWebSockets/20 Server</i>",
};

#else
static const std::string_view httpErrorResponses[] = {
    "", "HTTP/1.1 505 HTTP Version Not Supported\r\nConnection: close\r\n\r\n",
    "HTTP/1.1 431 Request Header Fields Too Large\r\nConnection: close\r\n\r\n",
    "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n"};
#endif

}  

#endif