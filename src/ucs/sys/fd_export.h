/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifndef UCS_SYS_FD_EXPORT_H
#define UCS_SYS_FD_EXPORT_H

#include <ucs/type/status.h>

#define UCS_FD_EXPORT_PATH_MAX 56

typedef struct {
    int  listen_fd;
    int  fd;
    char path[UCS_FD_EXPORT_PATH_MAX];
} ucs_fd_export_t;

/** Serve a borrowed FD from a private directory under dir, independently of
 * application progress. dir must be absolute and visible to the importer.
 * The caller must keep fd open until after cleanup. */
ucs_status_t ucs_fd_export_init(ucs_fd_export_t *exporter, int fd,
                                const char *dir);

/** Stop serving and wait for outstanding callbacks before returning. */
void ucs_fd_export_cleanup(ucs_fd_export_t *exporter);

/** Import a descriptor with close-on-exec set. The caller owns the result. */
ucs_status_t ucs_fd_import(const char *path, int *fd_p);

#endif
