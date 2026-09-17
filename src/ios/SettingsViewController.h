#import <UIKit/UIKit.h>

@interface SettingsViewController : UITableViewController
/* 任何设置改动后回调，控制器拿去重新套用 */
@property (nonatomic, copy) void (^onChanged)(void);
@end
