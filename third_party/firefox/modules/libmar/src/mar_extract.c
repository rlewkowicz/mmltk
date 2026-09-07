/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include "mar_private.h"
#include "mar.h"


static int mar_ensure_parent_dir(const char* path) {
  char* slash = strrchr(path, '/');
  if (slash) {
    *slash = '\0';
    mar_ensure_parent_dir(path);
    mkdir(path, 0755);
    *slash = '/';
  }
  return 0;
}

static int mar_test_callback(MarFile* mar, const MarItem* item, void* unused) {
  FILE* fp;
  uint8_t buf[BLOCKSIZE];
  int fd, len, offset = 0;

  if (mar_ensure_parent_dir(item->name)) {
    return -1;
  }

  fd = creat(item->name, item->flags);
  if (fd == -1) {
    fprintf(stderr, "ERROR: could not create file in mar_test_callback()\n");
    perror(item->name);
    return -1;
  }

  fp = fdopen(fd, "wb");
  if (!fp) {
    return -1;
  }

  while ((len = mar_read(mar, item, offset, buf, sizeof(buf))) > 0) {
    if (fwrite(buf, len, 1, fp) != 1) {
      break;
    }
    offset += len;
  }

  fclose(fp);
  return len == 0 ? 0 : -1;
}

int mar_extract(const char* path) {
  MarFile* mar;
  int rv;

  MarReadResult result = mar_open(path, &mar);
  if (result != MAR_READ_SUCCESS) {
    return -1;
  }

  rv = mar_enum_items(mar, mar_test_callback, NULL);

  mar_close(mar);
  return rv;
}
