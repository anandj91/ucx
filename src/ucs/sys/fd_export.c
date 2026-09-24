/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "fd_export.h"

#include <ucs/async/async_fwd.h>
#include <ucs/debug/log.h>
#include <ucs/sys/string.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void
ucs_fd_export_accept(int id, ucs_event_set_types_t events, void *arg)
{
    ucs_fd_export_t *exporter = arg;
    char control[CMSG_SPACE(sizeof(int))] UCS_V_ALIGNED(sizeof(size_t));
    char byte = 0;
    struct iovec iov = {&byte, sizeof(byte)};
    struct msghdr msg = {0};
    struct cmsghdr *cmsg;
    int sock;
    ssize_t ret;

    sock = accept4(id, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (sock < 0) {
        if ((errno != EAGAIN) && (errno != EINTR)) {
            ucs_debug("accept4(%d) failed: %m", id);
        }
        return;
    }

    memset(control, 0, sizeof(control));
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = control;
    msg.msg_controllen = sizeof(control);
    cmsg               = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level   = SOL_SOCKET;
    cmsg->cmsg_type    = SCM_RIGHTS;
    cmsg->cmsg_len     = CMSG_LEN(sizeof(exporter->fd));
    memcpy(CMSG_DATA(cmsg), &exporter->fd, sizeof(exporter->fd));
    do {
        ret = sendmsg(sock, &msg, MSG_NOSIGNAL);
    } while ((ret < 0) && (errno == EINTR));
    if (ret < 0) {
        ucs_debug("sendmsg(%d) failed: %m", sock);
    }
    close(sock);
}

static void ucs_fd_export_unlink(ucs_fd_export_t *exporter)
{
    if (unlink(exporter->path) < 0) {
        ucs_warn("unlink(%s) failed: %m", exporter->path);
    }
    *strrchr(exporter->path, '/') = '\0';
    if (rmdir(exporter->path) < 0) {
        ucs_warn("rmdir(%s) failed: %m", exporter->path);
    }
}

ucs_status_t ucs_fd_export_init(ucs_fd_export_t *exporter, int fd,
                                const char *dir)
{
    struct sockaddr_un addr = {0};
    ucs_status_t status;

    /* Reserve room for both the private directory and the socket suffix. */
    if ((dir[0] != '/') ||
        (strlen(dir) + sizeof("/ucx-XXXXXX/fd") > sizeof(exporter->path))) {
        ucs_debug("invalid fd export directory: %s", dir);
        return UCS_ERR_INVALID_PARAM;
    }

    exporter->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK |
                                          SOCK_CLOEXEC, 0);
    if (exporter->listen_fd < 0) {
        ucs_debug("fd export socket failed: %m");
        return UCS_ERR_IO_ERROR;
    }

    ucs_snprintf_safe(exporter->path, sizeof(exporter->path), "%s/ucx-XXXXXX",
                      dir);
    if (mkdtemp(exporter->path) == NULL) {
        ucs_debug("mkdtemp(%s) failed: %m", exporter->path);
        status = UCS_ERR_IO_ERROR;
        goto err_close;
    }

    /* The mode-0700 directory restricts FD access to its owner. */
    strcat(exporter->path, "/fd");
    addr.sun_family = AF_UNIX;
    ucs_strncpy_safe(addr.sun_path, exporter->path, sizeof(addr.sun_path));
    if (bind(exporter->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ucs_debug("bind(%s) failed: %m", exporter->path);
        status = UCS_ERR_IO_ERROR;
        goto err_rmdir;
    }

    if (listen(exporter->listen_fd, SOMAXCONN) < 0) {
        ucs_debug("listen(%s) failed: %m", exporter->path);
        status = UCS_ERR_IO_ERROR;
        goto err_unlink;
    }

    exporter->fd = fd;
    status = ucs_async_set_event_handler(UCS_ASYNC_MODE_THREAD_SPINLOCK,
                                         exporter->listen_fd,
                                         UCS_EVENT_SET_EVREAD,
                                         ucs_fd_export_accept, exporter, NULL);
    if (status == UCS_OK) {
        return UCS_OK;
    }

err_unlink:
    ucs_fd_export_unlink(exporter);
    goto err_close;
err_rmdir:
    *strrchr(exporter->path, '/') = '\0';
    if (rmdir(exporter->path) < 0) {
        ucs_warn("rmdir(%s) failed: %m", exporter->path);
    }
err_close:
    close(exporter->listen_fd);
    return status;
}

void ucs_fd_export_cleanup(ucs_fd_export_t *exporter)
{
    ucs_async_remove_handler(exporter->listen_fd, 1);
    close(exporter->listen_fd);
    ucs_fd_export_unlink(exporter);
}

ucs_status_t ucs_fd_import(const char *path, int *fd_p)
{
    char control[CMSG_SPACE(sizeof(int))] UCS_V_ALIGNED(sizeof(size_t));
    struct sockaddr_un addr = {0};
    struct msghdr msg = {0};
    int received_fd = -1;
    ucs_status_t status = UCS_ERR_IO_ERROR;
    char byte;
    struct iovec iov = {&byte, sizeof(byte)};
    struct cmsghdr *cmsg;
    struct pollfd pfd;
    ssize_t ret;
    size_t offset;
    int fd, count = 0;

    if ((path[0] != '/') ||
        (strnlen(path, UCS_FD_EXPORT_PATH_MAX) == UCS_FD_EXPORT_PATH_MAX)) {
        return UCS_ERR_INVALID_PARAM;
    }

    pfd.fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (pfd.fd < 0) {
        ucs_debug("fd import socket failed: %m");
        return status;
    }

    addr.sun_family = AF_UNIX;
    ucs_strncpy_safe(addr.sun_path, path, sizeof(addr.sun_path));
    if (connect(pfd.fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ucs_debug("connect(%s) failed: %m", path);
        goto out;
    }

    /* A stalled exporter must not block rkey unpack indefinitely. */
    pfd.events = POLLIN;
    ret        = poll(&pfd, 1, 1000);
    if (ret <= 0) {
        ucs_debug("fd import poll returned %zd: %m", ret);
        status = (ret == 0) ? UCS_ERR_TIMED_OUT : UCS_ERR_IO_ERROR;
        goto out;
    }

    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = control;
    msg.msg_controllen = sizeof(control);
    ret = recvmsg(pfd.fd, &msg, MSG_CMSG_CLOEXEC);
    if (ret < 0) {
        ucs_debug("recvmsg(%s) failed: %m", path);
        goto out;
    }

    /* Close every received FD on malformed or truncated replies. */
    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if ((cmsg->cmsg_level != SOL_SOCKET) ||
            (cmsg->cmsg_type != SCM_RIGHTS)) {
            continue;
        }
        for (offset = 0; offset + CMSG_LEN(sizeof(fd)) <= cmsg->cmsg_len;
             offset += sizeof(fd)) {
            memcpy(&fd, CMSG_DATA(cmsg) + offset, sizeof(fd));
            if (count++ == 0) {
                received_fd = fd;
            } else {
                close(fd);
            }
        }
    }

    if ((ret == sizeof(byte)) && (count == 1) &&
        !(msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC))) {
        *fd_p  = received_fd;
        status = UCS_OK;
    } else if (received_fd >= 0) {
        close(received_fd);
    }
out:
    close(pfd.fd);
    return status;
}
