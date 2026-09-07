/*
 * Copyright 2006 The Android Open Source Project
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkTypes.h"
#include "src/core/SkOSFile.h"

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>


#if defined(SK_BUILD_FOR_IOS)
#include "src/ports/SkOSFile_ios.h"
#endif


FILE* sk_fopen(const char path[], SkFILE_Flags flags) {
    char    perm[4] = {0, 0, 0, 0};
    char*   p = perm;

    if (flags & kRead_SkFILE_Flag) {
        *p++ = 'r';
    }
    if (flags & kWrite_SkFILE_Flag) {
        *p++ = 'w';
    }
    *p = 'b';

    FILE* file = nullptr;
    file = fopen(path, perm);
#if defined(SK_BUILD_FOR_IOS)
    if (!file && kRead_SkFILE_Flag == flags) {
        SkString bundlePath;
        if (ios_get_path_in_bundle(path, &bundlePath)) {
            file = fopen(bundlePath.c_str(), perm);
        }
    }
#endif

    if (nullptr == file && (flags & kWrite_SkFILE_Flag)) {
        SkDEBUGF("sk_fopen: fopen(\"%s\", \"%s\") returned nullptr (errno:%d): %s\n",
                 path, perm, errno, strerror(errno));
    }
    return file;
}

size_t sk_fgetsize(FILE* f) {
    SkASSERT(f);

    long curr = ftell(f); 
    if (curr < 0) {
        return 0;
    }

    fseek(f, 0, SEEK_END); 
    long size = ftell(f); 
    if (size < 0) {
        size = 0;
    }

    fseek(f, curr, SEEK_SET); 
    return size;
}

size_t sk_fwrite(const void* buffer, size_t byteCount, FILE* f) {
    SkASSERT(f);
    return fwrite(buffer, 1, byteCount, f);
}

void sk_fflush(FILE* f) {
    SkASSERT(f);
    fflush(f);
}

size_t sk_ftell(FILE* f) {
    long curr = ftell(f);
    if (curr < 0) {
        return 0;
    }
    return curr;
}

void sk_fclose(FILE* f) {
    if (f) {
        fclose(f);
    }
}

bool sk_isdir(const char *path) {
    struct stat status = {};
    if (stat(path, &status) == 0) {
        return SkToBool(status.st_mode & S_IFDIR);
    }
#if defined(SK_BUILD_FOR_IOS)
    SkString bundlePath;
    if (!ios_get_path_in_bundle(path, &bundlePath)) {
        return false;
    }
    if (stat(bundlePath.c_str(), &status) == 0) {
        return SkToBool(status.st_mode & S_IFDIR);
    }
#endif
    return false;
}

bool sk_mkdir(const char* path) {
    if (sk_isdir(path)) {
        return true;
    }
    if (sk_exists(path)) {
        fprintf(stderr,
                "sk_mkdir: path '%s' already exists but is not a directory\n",
                path);
        return false;
    }

    int retval;
    retval = mkdir(path, 0777);
    if (retval) {
      perror("mkdir() failed with error: ");
    }
    return 0 == retval;
}
