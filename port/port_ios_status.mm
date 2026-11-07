/* iOS: a plain status screen (text + spinner) shown during first-launch
 * setup, before SDL creates its window. Needs no input, unlike an alert.
 * Call from the main thread; port_ios.cpp keeps the run loop turning.
 * Built with ARC (-fobjc-arc). */
#import <UIKit/UIKit.h>

static UIWindow* sStatusWindow = nil;
static UILabel* sStatusTitle = nil;
static UILabel* sStatusBody = nil;

extern "C" void port_ios_status_show(const char* title, const char* text) {
    if (sStatusWindow != nil) {
        // Already showing: just update the text.
        sStatusTitle.text = [NSString stringWithUTF8String:title];
        sStatusBody.text = [NSString stringWithUTF8String:text];
        return;
    }
    UIWindowScene* scene = nil;
    for (UIScene* s in UIApplication.sharedApplication.connectedScenes) {
        if ([s isKindOfClass:[UIWindowScene class]]) {
            scene = (UIWindowScene*)s;
            break;
        }
    }
    if (scene == nil) {
        return;
    }
    UIViewController* vc = [[UIViewController alloc] init];
    vc.view.backgroundColor = UIColor.blackColor;

    UILabel* titleLabel = [[UILabel alloc] init];
    titleLabel.text = [NSString stringWithUTF8String:title];
    titleLabel.textColor = UIColor.whiteColor;
    titleLabel.font = [UIFont boldSystemFontOfSize:22];

    UILabel* body = [[UILabel alloc] init];
    body.text = [NSString stringWithUTF8String:text];
    body.textColor = [UIColor colorWithWhite:0.8 alpha:1.0];
    body.font = [UIFont systemFontOfSize:16];
    body.numberOfLines = 0;
    body.textAlignment = NSTextAlignmentCenter;

    UIActivityIndicatorView* spinner =
        [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleLarge];
    spinner.color = UIColor.whiteColor;
    [spinner startAnimating];

    UIStackView* stack = [[UIStackView alloc] initWithArrangedSubviews:@[ titleLabel, body, spinner ]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.alignment = UIStackViewAlignmentCenter;
    stack.spacing = 16;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [vc.view addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.centerXAnchor constraintEqualToAnchor:vc.view.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:vc.view.centerYAnchor],
        [stack.widthAnchor constraintLessThanOrEqualToAnchor:vc.view.widthAnchor multiplier:0.8],
    ]];

    sStatusTitle = titleLabel;
    sStatusBody = body;
    sStatusWindow = [[UIWindow alloc] initWithWindowScene:scene];
    sStatusWindow.rootViewController = vc;
    sStatusWindow.windowLevel = UIWindowLevelNormal + 1;  // above the game's window, if it exists
    [sStatusWindow makeKeyAndVisible];
}

extern "C" void port_ios_status_hide(void) {
    if (sStatusWindow == nil) {
        return;
    }
    UIWindowScene* scene = sStatusWindow.windowScene;
    sStatusWindow.hidden = YES;
    sStatusWindow = nil;
    sStatusTitle = nil;
    sStatusBody = nil;
    // Give keyboard/controller focus back to the game's window.
    for (UIWindow* w in scene.windows) {
        if (!w.hidden) {
            [w makeKeyWindow];
            break;
        }
    }
}
