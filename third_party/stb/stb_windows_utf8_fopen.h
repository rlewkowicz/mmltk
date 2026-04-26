#ifndef STB_WINDOWS_UTF8_FOPEN_H
#define STB_WINDOWS_UTF8_FOPEN_H

#if defined(_WIN32)
static inline FILE* stb__fopen_utf8_impl(const char* filename, const char* mode,
                                         int (*convert_utf8_to_wchar)(wchar_t*, int, const char*)) {
    FILE* f;
    wchar_t wMode[64];
    wchar_t wFilename[1024];
    if (0 == convert_utf8_to_wchar(wFilename, sizeof(wFilename) / sizeof(*wFilename), filename))
        return 0;
    if (0 == convert_utf8_to_wchar(wMode, sizeof(wMode) / sizeof(*wMode), mode))
        return 0;

#if defined(_MSC_VER) && _MSC_VER >= 1400
    if (0 != _wfopen_s(&f, wFilename, wMode))
        return 0;
    return f;
#else
    return _wfopen(wFilename, wMode);
#endif
}
#endif

#endif
