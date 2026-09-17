#import <Foundation/Foundation.h>
#import "vt.h"

@class TerminalSession;

@protocol TerminalSessionDelegate <NSObject>
- (void)sessionDidUpdate:(TerminalSession *)s;                 /* 有新输出或光标动了 */
- (void)session:(TerminalSession *)s didChangeTitle:(NSString *)title;
- (void)session:(TerminalSession *)s didExitWithStatus:(int)status;
- (void)sessionDidRingBell:(TerminalSession *)s;
- (void)session:(TerminalSession *)s notify:(NSString *)title body:(NSString *)body;
@end

@interface TerminalSession : NSObject
@property (nonatomic, weak) id<TerminalSessionDelegate> delegate;
@property (nonatomic, readonly) Vt *vt;
@property (nonatomic, readonly) BOOL running;
@property (nonatomic, copy) NSString *title;         /* 窗口标题(OSC 0/2 或上次跑的命令) */
@property (nonatomic, readonly) pid_t pid;

- (instancetype)initWithCols:(int)cols rows:(int)rows;
- (void)start;                                       /* 起 shell */
- (void)sendBytes:(const void *)bytes length:(NSUInteger)len;
- (void)sendUTF8String:(NSString *)str;
- (void)resizeToCols:(int)cols rows:(int)rows;
- (void)setPixelSize:(int)w height:(int)h;
- (void)terminate;
/* 剪贴板：OSC52 读的时候要 */
@property (nonatomic, copy) NSString *pendingClipboard;   /* 供 OSC52 查询返回 */
@end
