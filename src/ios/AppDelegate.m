#import "AppDelegate.h"
#import "TerminalViewController.h"
#import "Settings.h"

@implementation AppDelegate

- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)options
{
    TermSettings *st = [TermSettings shared];
    [st reload];
    [st loadConfigFile];

    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    self.window.backgroundColor = [UIColor blackColor];
    self.window.rootViewController = [[TerminalViewController alloc] init];
    [self.window makeKeyAndVisible];
    return YES;
}

- (void)applicationDidEnterBackground:(UIApplication *)app
{
    [[TermSettings shared] persist];
}

- (void)applicationWillTerminate:(UIApplication *)app
{
    [[TermSettings shared] persist];
}

@end
