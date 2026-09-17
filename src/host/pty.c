#include "pty.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <util.h>

int pty_spawn(Pty *p, const char *shell, int cols, int rows, char *const *envp)
{
    memset(p, 0, sizeof(*p));
    p->fd = -1;
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    int fd = -1;
    char shellbuf[512];
    snprintf(shellbuf, sizeof(shellbuf), "%s", shell ? shell : "/bin/sh");
    const char *base = strrchr(shellbuf, '/');
    base = base ? base + 1 : shellbuf;
    /* 登录 shell：argv[0] 前面加 '-' */
    char arg0[520];
    snprintf(arg0, sizeof(arg0), "-%s", base);
    char *argv[3];
    argv[0] = arg0;
    argv[1] = NULL;
    argv[2] = NULL;
    pid_t pid = forkpty(&fd, NULL, NULL, &ws);
    if (pid < 0) return -1;
    if (pid == 0) {
        /* 子进程里只能用 async-signal-safe 的函数 */
        if (envp) {
            execve(shellbuf, argv, (char *const *)envp);
        } else {
            char *e[2];
            e[0] = NULL;
            execve(shellbuf, argv, e);
        }
        _exit(127);
    }
    p->fd = fd;
    p->pid = pid;
    p->alive = 1;
    return 0;
}

int pty_write(Pty *p, const char *data, size_t len)
{
    if (p->fd < 0 || len == 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(p->fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) { usleep(1000); continue; }
            return -1;
        }
        off += (size_t)n;
    }
    return (int)off;
}

void pty_resize(Pty *p, int cols, int rows)
{
    if (p->fd < 0) return;
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    ioctl(p->fd, TIOCSWINSZ, &ws);
}

int pty_read(Pty *p, char *buf, size_t len)
{
    if (p->fd < 0) return -1;
    for (;;) {
        ssize_t n = read(p->fd, buf, len);
        if (n > 0) return (int)n;
        if (n == 0) return 0;
        if (errno == EINTR) continue;
        return -1;
    }
}

void pty_set_nonblock(Pty *p, int on)
{
    if (p->fd < 0) return;
    int fl = fcntl(p->fd, F_GETFL, 0);
    if (on) fl |= O_NONBLOCK;
    else fl &= ~O_NONBLOCK;
    fcntl(p->fd, F_SETFL, fl);
}

void pty_close(Pty *p)
{
    if (p->fd >= 0) {
        close(p->fd);
        p->fd = -1;
    }
    if (p->pid > 0) {
        kill(p->pid, SIGHUP);
        kill(p->pid, SIGCONT);
        /* 收尸：不 wait 的话每关一个标签就留一个僵尸 */
        int reaped = 0;
        for (int i = 0; i < 100; i++) {
            int st = 0;
            pid_t r = waitpid(p->pid, &st, WNOHANG);
            if (r == p->pid) {
                p->last_status = st;
                reaped = 1;
                break;
            }
            if (r < 0 && errno == ECHILD) { reaped = 1; break; }
            usleep(20000);
        }
        if (!reaped) {
            int st = 0;
            kill(p->pid, SIGKILL);
            waitpid(p->pid, &st, 0);
            p->last_status = st;
        }
        p->pid = -1;
    }
    p->alive = 0;
}

int pty_wait(Pty *p, int *status)
{
    if (p->pid <= 0) return p->last_status;
    int st = 0;
    for (int i = 0; i < 150; i++) {
        pid_t r = waitpid(p->pid, &st, WNOHANG);
        if (r == p->pid) {
            p->last_status = st;
            p->alive = 0;
            p->pid = -1;
            if (status) *status = st;
            return st;
        }
        if (r < 0 && errno == ECHILD) break;
        usleep(10000);
    }
    if (status) *status = p->last_status;
    return p->last_status;
}
