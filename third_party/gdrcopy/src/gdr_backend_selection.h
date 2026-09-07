#ifndef GDR_BACKEND_SELECTION_H
#define GDR_BACKEND_SELECTION_H

#include <errno.h>

/* Inputs are normalized errno outcomes, with zero denoting a usable backend.
 * A deliberately skipped backend has ENOTSUP as its selection outcome. */
static inline int gdr_backend_selection_error(int gdrdrv_error, int dmabuf_error)
{
    if (gdrdrv_error == 0 || dmabuf_error == 0)
        return 0;
    if (gdrdrv_error != ENOTSUP)
        return gdrdrv_error;
    return dmabuf_error;
}

#endif
