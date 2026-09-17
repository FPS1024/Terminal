#import "TerminalSession.h"
#import "Settings.h"
#import "platform.h"
#import "pty.h"
#import <UIKit/UIKit.h>
#import <string.h>
#import <errno.h>
#import <poll.h>
#import <unistd.h>

@interface TerminalSession ()
- (void)markNeedsUpdate;
- (void)handleBell;
@end

/* Vt 回调：ud 就是 session 自己 */
static void vt_on_write(void *ud, const char *d, size_t n)
{
    TerminalSession *s = (__bridge TerminalSession *)ud;
    [s sendBytes:d length:n];
}

static void vt_on_bell(void *ud)
{
    TerminalSession *s = (__bridge TerminalSession *)ud;
    [s performSelectorOnMainThread:@selector(handleBell) withObject:nil waitUntilDone:NO];
}

static void vt_on_title(void *ud, const char *t)
{
    TerminalSession *s = (__bridge TerminalSession *)ud;
    NSString *str = [NSString stringWithUTF8String:t ? t : ""];
    dispatch_async(dispatch_get_main_queue(), ^{
        s.title = str;
        [s.delegate session:s didChangeTitle:str];
    });
}

static void vt_on_clipboard(void *ud, const char *d, size_t n)
{
    TerminalSession *s = (__bridge TerminalSession *)ud;
    NSString *str = [[NSString alloc] initWithBytes:d length:n encoding:NSUTF8StringEncoding];
    if (!str) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        [UIPasteboard generalPasteboard].string = str;
        s.pendingClipboard = str;
    });
}

static void vt_on_notify(void *ud, const char *title, const char *body)
{
    TerminalSession *s = (__bridge TerminalSession *)ud;
    NSString *t = [NSString stringWithUTF8String:title ? title : ""];
    NSString *b = [NSString stringWithUTF8String:body ? body : ""];
    dispatch_async(dispatch_get_main_queue(), ^{
        [s.delegate session:s notify:t body:b];
    });
}

static void vt_on_dirty(void *ud)
{
    TerminalSession *s = (__bridge TerminalSession *)ud;
    [s markNeedsUpdate];
}

@interface TerminalSession ()
@property (nonatomic, assign) Pty pty;
@property (nonatomic, assign) BOOL updatePending;
@property (nonatomic, assign) BOOL stopRequested;
@property (nonatomic, assign) BOOL readLoopDone;
@end

@implementation TerminalSession

- (instancetype)initWithCols:(int)cols rows:(int)rows
{
    self = [super init];
    if (self) {
        TermSettings *st = [TermSettings shared];
        _vt = vt_new(cols, rows, (int)st.scrollbackLines);
        _title = @"终端";
        _running = NO;
        memset(&_pty, 0, sizeof(_pty));
        _pty.fd = -1;
        _vt->ud = (__bridge void *)self;
        _vt->write_cb = vt_on_write;
        _vt->bell_cb = vt_on_bell;
        _vt->title_cb = vt_on_title;
        _vt->clipboard_cb = vt_on_clipboard;
        _vt->notify_cb = vt_on_notify;
        _vt->dirty_cb = vt_on_dirty;
    }
    return self;
}

- (void)dealloc
{
    [self terminate];
    if (_vt) {
        vt_free(_vt);
        _vt = NULL;
    }
}

- (void)handleBell
{
    [self.delegate sessionDidRingBell:self];
}

- (void)markNeedsUpdate
{
    /* 核心已经在主线程(输入)或读线程里调这里：统一走主线程 + 合并 */
    if ([NSThread isMainThread]) {
        if (self.updatePending) return;
        self.updatePending = YES;
        __weak TerminalSession *weakSelf = self;
        dispatch_async(dispatch_get_main_queue(), ^{
            TerminalSession *s = weakSelf;
            if (!s) return;
            s.updatePending = NO;
            [s.delegate sessionDidUpdate:s];
        });
    } else {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (self.updatePending) return;
            self.updatePending = YES;
            __weak TerminalSession *weakSelf = self;
            dispatch_async(dispatch_get_main_queue(), ^{
                TerminalSession *s = weakSelf;
                if (!s) return;
                s.updatePending = NO;
                [s.delegate sessionDidUpdate:s];
            });
        });
    }
}

- (void)start
{
    if (self.running) return;
    TermSettings *st = [TermSettings shared];
    NSString *shell = st.shellPath.length ? st.shellPath : [NSString stringWithUTF8String:default_shell()];
    char **env = build_env([shell UTF8String], "xterm-256color", pick_lang(), _vt->cols, _vt->rows);
    int rc = pty_spawn(&_pty, [shell UTF8String], _vt->cols, _vt->rows, env);
    free_env(env);
    if (rc != 0) {
        const char *msg = "\r\n\xe6\x97\xa0\xe6\xb3\x95\xe5\x90\xaf\xe5\x8a\xa8 shell (forkpty \xe5\xa4\xb1\xe8\xb4\xa5)\r\n";
        vt_input(_vt, msg, strlen(msg));
        [self.delegate sessionDidUpdate:self];
        return;
    }
    _running = YES;
    pty_set_nonblock(&_pty, 1);
    [NSThread detachNewThreadSelector:@selector(readLoop) toTarget:self withObject:nil];
    if (st.startupCommand.length) {
        [self sendUTF8String:[NSString stringWithFormat:@"%@\n", st.startupCommand]];
    }
}

- (void)readLoop
{
    @autoreleasepool {
        char buf[65536];
        while (!self.stopRequested) {
            if (_pty.fd < 0) break;
            /* poll + 超时：这样 stopRequested 一定能被看到，
               也不会出现"主线程 close 掉正在被 poll 的 fd"那种事 */
            struct pollfd pf;
            pf.fd = _pty.fd;
            pf.events = POLLIN;
            pf.revents = 0;
            int pr = poll(&pf, 1, 250);
            if (self.stopRequested) break;
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (pr == 0) continue;
            int n = pty_read(&_pty, buf, sizeof(buf));
            if (n > 0) {
                vt_input(_vt, buf, (size_t)n);
                [self markNeedsUpdate];
                continue;
            }
            if (n == 0) break;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(2000);
                continue;
            }
            break;
        }
    }
    /* fd 让读线程自己关，主线程只负责等，两个线程不抢同一个 fd */
    pty_close(&_pty);
    int status = _pty.last_status;
    _running = NO;
    self.readLoopDone = YES;
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.delegate sessionDidUpdate:self];
        [self.delegate session:self didExitWithStatus:status];
    });
}

- (void)sendBytes:(const void *)bytes length:(NSUInteger)len
{
    if (!self.running || _pty.fd < 0) return;
    pty_write(&_pty, bytes, len);
}

- (void)sendUTF8String:(NSString *)str
{
    if (!str.length) return;
    const char *utf8 = [str UTF8String];
    [self sendBytes:utf8 length:strlen(utf8)];
}

- (void)resizeToCols:(int)cols rows:(int)rows
{
    if (cols < 1 || rows < 1) return;
    if (cols == _vt->cols && rows == _vt->rows) return;
    vt_resize(_vt, cols, rows);
    if (self.running) pty_resize(&_pty, cols, rows);
}

- (void)setPixelSize:(int)w height:(int)h
{
    _vt->pixel_w = w;
    _vt->pixel_h = h;
}

- (void)terminate
{
    self.stopRequested = YES;
    if (!self.readLoopDone && _pty.fd >= 0) {
        for (int i = 0; i < 120 && !self.readLoopDone; i++) usleep(10000);
        if (!self.readLoopDone && _pty.fd >= 0) pty_close(&_pty);
    } else if (_pty.fd >= 0) {
        pty_close(&_pty);
    }
    _running = NO;
}

@end
