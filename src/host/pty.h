/* forkpty 会话封装 */
#ifndef PTY_H
#define PTY_H
#include <stddef.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    int   fd;
    pid_t pid;
    int   alive;
    int   last_status;
} Pty;

/* 成功返回 0。shell 用登录 shell 方式启动。 */
int  pty_spawn(Pty *p, const char *shell, int cols, int rows, char *const *envp);
int  pty_write(Pty *p, const char *data, size_t len);
void pty_resize(Pty *p, int cols, int rows);
int  pty_read(Pty *p, char *buf, size_t len);   /* 返回 >0 数据，0 = EOF，<0 = 错误/EAGAIN */
void pty_set_nonblock(Pty *p, int on);
void pty_close(Pty *p);
/* 子进程结束后取退出码 */
int  pty_wait(Pty *p, int *status);
#ifdef __cplusplus
}
#endif
#endif
