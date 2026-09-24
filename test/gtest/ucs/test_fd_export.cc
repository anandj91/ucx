/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#include <common/test.h>
extern "C" {
#include <ucs/sys/fd_export.h>
}
#include <fcntl.h>
#include <sched.h>
#include <sys/wait.h>

class test_fd_export : public ucs::test {
protected:
    void init() override {
        ucs::test::init();
        m_active = false;
        ASSERT_EQ(0, pipe(m_pipe));
        ASSERT_UCS_OK(ucs_fd_export_init(&m_exporter, m_pipe[0], "/tmp"));
        m_active = true;
    }

    void cleanup() override {
        if (m_active) {
            ucs_fd_export_cleanup(&m_exporter);
        }
        close(m_pipe[0]);
        close(m_pipe[1]);
        ucs::test::cleanup();
    }

    void import_child(int ns_flags) {
        ASSERT_EQ(1, write(m_pipe[1], "x", 1));
        pid_t child = fork();
        ASSERT_GE(child, 0);
        if (child == 0) {
            close(m_pipe[0]);
            close(m_pipe[1]);
            close(m_exporter.listen_fd);
            if (ns_flags != 0) {
                if (unshare(ns_flags) < 0) {
                    _exit((errno == EPERM) ? 77 : 1);
                }
                pid_t inner = fork();
                if (inner < 0) {
                    _exit(1);
                }
                if (inner > 0) {
                    int status;
                    _exit((waitpid(inner, &status, 0) == inner) &&
                          WIFEXITED(status) ? WEXITSTATUS(status) : 1);
                }
            }
            int fd;
            char byte;
            if (ucs_fd_import(m_exporter.path, &fd) != UCS_OK) {
                _exit(1);
            }
            bool ok = (fcntl(fd, F_GETFD) & FD_CLOEXEC) &&
                      (read(fd, &byte, 1) == 1) && (byte == 'x');
            close(fd);
            _exit(ok ? 0 : 1);
        }
        int status;
        ASSERT_EQ(child, waitpid(child, &status, 0));
        ASSERT_TRUE(WIFEXITED(status));
        if (WEXITSTATUS(status) == 77) {
            UCS_TEST_SKIP_R("PID/network namespace creation is not permitted");
        }
        EXPECT_EQ(0, WEXITSTATUS(status));
    }

    ucs_fd_export_t m_exporter;
    int m_pipe[2] = {-1, -1};
    bool m_active;
};

UCS_TEST_F(test_fd_export, process)
{
    import_child(0);
    import_child(0);
}

UCS_TEST_F(test_fd_export, pid_and_net_namespace)
{
    import_child(CLONE_NEWPID | CLONE_NEWNET);
}

UCS_TEST_F(test_fd_export, cleanup)
{
    std::string path = m_exporter.path;
    ucs_fd_export_cleanup(&m_exporter);
    m_active = false;
    EXPECT_EQ(-1, access(path.c_str(), F_OK));
    int fd = -1;
    EXPECT_NE(UCS_OK, ucs_fd_import(path.c_str(), &fd));
    EXPECT_EQ(-1, fd);
}
