/* iOS: the voice chat button shown during online play (n64net_voice.h).
 *
 * A small round button in the top-right corner, in its own window above the
 * game that lets every other touch through and never becomes key (so the
 * controller keeps talking to the game). Open mic: tap to mute or unmute.
 * Push to talk: hold to talk. Its ring turns green while this phone sends
 * speech; a speaker badge shows while the other player is talking.
 * Called on the main thread (the game loop runs there). Built with ARC. */
#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>

#include "n64net_voice.h"

extern "C" void port_log(const char* fmt, ...);

@interface PortVoicePassthroughWindow : UIWindow
@end
@implementation PortVoicePassthroughWindow
- (UIView*)hitTest:(CGPoint)point withEvent:(UIEvent*)event {
    UIView* hit = [super hitTest:point withEvent:event];
    return (hit == self || hit == self.rootViewController.view) ? nil : hit;  // only the button
}
@end

@interface PortVoiceController : UIViewController
@property(nonatomic, strong) UIButton* button;
@property(nonatomic, strong) UIImageView* remoteBadge;
@property(nonatomic, strong) NSTimer* timer;
@end
@implementation PortVoiceController
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.clearColor;

    UIButton* b = [UIButton buttonWithType:UIButtonTypeCustom];
    b.translatesAutoresizingMaskIntoConstraints = NO;
    b.layer.cornerRadius = 26;
    b.layer.borderWidth = 3;
    b.tintColor = UIColor.whiteColor;
    b.accessibilityLabel = @"Voice chat";
    [b addTarget:self action:@selector(down) forControlEvents:UIControlEventTouchDown];
    [b addTarget:self action:@selector(up) forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside |
                                                             UIControlEventTouchCancel];
    [self.view addSubview:b];
    self.button = b;

    UIImageView* badge = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"speaker.wave.2.fill"]];
    badge.translatesAutoresizingMaskIntoConstraints = NO;
    badge.tintColor = UIColor.systemGreenColor;
    badge.hidden = YES;
    [self.view addSubview:badge];
    self.remoteBadge = badge;

    // Right edge, halfway down: the battle timer is at the top right and the
    // damage meters along the bottom. The "other player talking" badge sits
    // just above the button.
    UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [b.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor],
        [b.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-8],
        [b.widthAnchor constraintEqualToConstant:52],
        [b.heightAnchor constraintEqualToConstant:52],
        [badge.centerXAnchor constraintEqualToAnchor:b.centerXAnchor],
        [badge.bottomAnchor constraintEqualToAnchor:b.topAnchor constant:-6],
    ]];
    [self refresh];
    __weak PortVoiceController* w = self;
    self.timer = [NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES block:^(NSTimer*) { [w refresh]; }];
}
- (void)down {
    if (n64net_voice_is_push_to_talk()) {
        n64net_voice_set_talking(1);
    } else {
        n64net_voice_set_muted(!n64net_voice_is_muted());
    }
    [self refresh];
}
- (void)up {
    if (n64net_voice_is_push_to_talk()) {
        n64net_voice_set_talking(0);
    }
    [self refresh];
}
- (void)refresh {
    bool ptt = n64net_voice_is_push_to_talk();
    bool sending = ptt ? self.button.isTracking : !n64net_voice_is_muted();
    [self.button setImage:[UIImage systemImageNamed:sending ? @"mic.fill" : @"mic.slash.fill"]
                 forState:UIControlStateNormal];
    self.button.backgroundColor = sending ? [UIColor colorWithWhite:0.15 alpha:0.75]
                                          : [UIColor colorWithRed:0.6 green:0.1 blue:0.1 alpha:0.75];
    self.button.layer.borderColor =
        (n64net_voice_local_speaking() ? UIColor.systemGreenColor : [UIColor colorWithWhite:1 alpha:0.35]).CGColor;
    self.remoteBadge.hidden = !n64net_voice_remote_speaking();
}
- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    return UIInterfaceOrientationMaskLandscape;
}
- (BOOL)prefersStatusBarHidden {
    return YES;
}
- (BOOL)prefersHomeIndicatorAutoHidden {
    return YES;
}
- (void)dealloc {
    [self.timer invalidate];
}
@end

static UIWindow* sVoiceWindow = nil;

extern "C" void port_ios_voice_button_show(void) {
    if (sVoiceWindow != nil) {
        return;
    }
    // (Echo cancellation: n64net sets the voice-chat audio mode when the mic opens.)
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
    sVoiceWindow = [[PortVoicePassthroughWindow alloc] initWithWindowScene:scene];
    sVoiceWindow.rootViewController = [PortVoiceController new];
    sVoiceWindow.windowLevel = UIWindowLevelNormal + 2;
    sVoiceWindow.backgroundColor = UIColor.clearColor;
    sVoiceWindow.hidden = NO;  // visible, but never key: the game keeps the controller
}

extern "C" void port_ios_voice_button_hide(void) {
    if (sVoiceWindow == nil) {
        return;
    }
    sVoiceWindow.hidden = YES;
    sVoiceWindow = nil;
}
