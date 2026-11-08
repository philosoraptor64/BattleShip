/* iOS start menu (touch): local play, or online play as host or joiner.
 *
 * Shown by port_ios_prepare before the game starts (unless an invite link or
 * a scripted netplay.cfg already decided). Runs its own UIKit window and
 * returns once the player has chosen; port_ios.cpp turns the choice into the
 * SSB64_NETPLAY_* settings. Built with ARC.
 */
#import <UIKit/UIKit.h>

#include "port_ios_menu.h"

#include <cstdio>
#include <cstring>

static int sMenuChoice = -1;  // PORT_IOS_MENU_* once chosen
static PortIosMenuState* sState = nullptr;
static UIWindow* sMenuWindow = nil;

static NSString* Str(const char* s) {
    return [NSString stringWithUTF8String:(s != nullptr) ? s : ""];
}

static void Copy(char* dst, size_t cap, NSString* s) {
    std::snprintf(dst, cap, "%s", s.UTF8String ? s.UTF8String : "");
}

static NSString* NewRoomCode(void) {
    static const char* kAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // no look-alikes
    char code[5] = {};
    for (int i = 0; i < 4; i++) {
        code[i] = kAlphabet[arc4random_uniform((uint32_t)std::strlen(kAlphabet))];
    }
    return Str(code);
}

static NSString* InviteLink(NSString* room) {
    // The part after '#' never reaches the web server (docs/join/index.html
    // hands it to battleship-net://join?...).
    NSURLComponents* q = [NSURLComponents new];
    q.queryItems = @[
        [NSURLQueryItem queryItemWithName:@"s" value:Str(sState->server)],
        [NSURLQueryItem queryItemWithName:@"r" value:room],
        [NSURLQueryItem queryItemWithName:@"p" value:Str(sState->password)],
    ];
    return [@"https://philosoraptor64.github.io/BattleShip/join/#" stringByAppendingString:q.percentEncodedQuery];
}

// ---------------------------------------------------------------- building blocks

@interface PortMenuScreen : UIViewController
@property(nonatomic, strong) UIStackView* stack;
- (UILabel*)addTitle:(NSString*)text;
- (UILabel*)addText:(NSString*)text;
- (UIButton*)addButton:(NSString*)title primary:(BOOL)primary action:(void (^)(void))action;
@end

@implementation PortMenuScreen
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    UIScrollView* scroll = [UIScrollView new];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.keyboardDismissMode = UIScrollViewKeyboardDismissModeInteractive;
    [self.view addSubview:scroll];
    self.stack = [UIStackView new];
    self.stack.axis = UILayoutConstraintAxisVertical;
    self.stack.alignment = UIStackViewAlignmentFill;
    self.stack.spacing = 14;
    self.stack.translatesAutoresizingMaskIntoConstraints = NO;
    [scroll addSubview:self.stack];
    UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:safe.topAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [self.stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:16],
        [self.stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor constant:-16],
        [self.stack.centerXAnchor constraintEqualToAnchor:scroll.centerXAnchor],
        [self.stack.widthAnchor constraintEqualToConstant:360],
        [self.stack.heightAnchor constraintGreaterThanOrEqualToAnchor:scroll.frameLayoutGuide.heightAnchor
                                                             constant:-32],
    ]];
}
- (UILabel*)addTitle:(NSString*)text {
    UILabel* l = [UILabel new];
    l.text = text;
    l.textColor = UIColor.whiteColor;
    l.font = [UIFont boldSystemFontOfSize:26];
    l.textAlignment = NSTextAlignmentCenter;
    [self.stack addArrangedSubview:l];
    return l;
}
- (UILabel*)addText:(NSString*)text {
    UILabel* l = [UILabel new];
    l.text = text;
    l.textColor = [UIColor colorWithWhite:0.75 alpha:1];
    l.font = [UIFont systemFontOfSize:15];
    l.numberOfLines = 0;
    l.textAlignment = NSTextAlignmentCenter;
    [self.stack addArrangedSubview:l];
    return l;
}
- (UIButton*)addButton:(NSString*)title primary:(BOOL)primary action:(void (^)(void))action {
    UIButtonConfiguration* c = primary ? [UIButtonConfiguration filledButtonConfiguration]
                                       : [UIButtonConfiguration grayButtonConfiguration];
    c.title = title;
    c.cornerStyle = UIButtonConfigurationCornerStyleLarge;
    c.contentInsets = NSDirectionalEdgeInsetsMake(12, 16, 12, 16);
    if (primary) {
        c.baseBackgroundColor = [UIColor colorWithRed:0.84 green:0.18 blue:0.18 alpha:1];
    }
    UIButton* b = [UIButton buttonWithConfiguration:c
                                      primaryAction:[UIAction actionWithHandler:^(UIAction*) { action(); }]];
    [self.stack addArrangedSubview:b];
    return b;
}
- (UITextField*)addField:(NSString*)placeholder text:(NSString*)text secure:(BOOL)secure {
    UITextField* f = [UITextField new];
    f.placeholder = placeholder;
    f.text = text;
    f.secureTextEntry = secure;
    f.borderStyle = UITextBorderStyleRoundedRect;
    f.autocorrectionType = UITextAutocorrectionTypeNo;
    f.autocapitalizationType = UITextAutocapitalizationTypeNone;
    f.spellCheckingType = UITextSpellCheckingTypeNo;
    f.font = [UIFont systemFontOfSize:18];
    [self.stack addArrangedSubview:f];
    return f;
}
- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    return UIInterfaceOrientationMaskLandscape;
}
@end

static void Finish(int choice) {
    sMenuChoice = choice;
}

static PortMenuScreen* MakeSettings(void);
static PortMenuScreen* MakeJoin(void);
static PortMenuScreen* MakeHost(void);

static bool HasServer(void) {
    return sState->server[0] != '\0';
}

static PortMenuScreen* MakeOnline(void) {
    PortMenuScreen* s = [PortMenuScreen new];
    s.title = @"Online";
    __weak PortMenuScreen* w = s;
    s.view.hidden = NO;  // load
    [s addTitle:@"Play online"];
    [s addButton:@"Host a match" primary:YES action:^{ [w.navigationController pushViewController:MakeHost() animated:YES]; }];
    [s addButton:@"Join a match" primary:YES action:^{ [w.navigationController pushViewController:MakeJoin() animated:YES]; }];
    [s addButton:@"Online settings" primary:NO action:^{ [w.navigationController pushViewController:MakeSettings() animated:YES]; }];
    if (!HasServer()) {
        [s addText:@"No server set yet: open an invite link from a friend once, or enter the server and password under Online settings."];
    }
    return s;
}

static PortMenuScreen* MakeJoin(void) {
    PortMenuScreen* s = [PortMenuScreen new];
    s.title = @"Join";
    s.view.hidden = NO;
    [s addTitle:@"Join a match"];
    [s addText:@"Enter the 4-character room code from the host. When you're connected, both games go to the VS menu together."];
    UITextField* code = [s addField:@"Room code" text:Str(sState->room) secure:NO];
    code.autocapitalizationType = UITextAutocapitalizationTypeAllCharacters;
    code.textAlignment = NSTextAlignmentCenter;
    code.font = [UIFont monospacedSystemFontOfSize:30 weight:UIFontWeightBold];
    UILabel* error = [s addText:@""];
    error.textColor = [UIColor colorWithRed:1 green:0.45 blue:0.45 alpha:1];
    [s addButton:@"Join" primary:YES action:^{
        NSString* r = [[code.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet] uppercaseString];
        NSCharacterSet* bad = [[NSCharacterSet alphanumericCharacterSet] invertedSet];
        if (r.length != 4 || [r rangeOfCharacterFromSet:bad].location != NSNotFound) {
            error.text = @"Room codes are 4 letters or digits.";
            return;
        }
        if (!HasServer()) {
            error.text = @"Set the server first (Online settings), or open an invite link.";
            return;
        }
        Copy(sState->room, sizeof(sState->room), r);
        Finish(PORT_IOS_MENU_JOIN);
    }];
    return s;
}

static PortMenuScreen* MakeHost(void) {
    PortMenuScreen* s = [PortMenuScreen new];
    s.title = @"Host";
    s.view.hidden = NO;
    __block NSString* room = (sState->room[0] != '\0' && sState->last_was_host) ? Str(sState->room) : NewRoomCode();
    [s addTitle:@"Host a match"];
    UILabel* code = [s addTitle:room];
    code.font = [UIFont monospacedSystemFontOfSize:44 weight:UIFontWeightBold];
    [s addText:@"Your room code. Send your friend the invite link (or just the code), then open the room. "
               @"When they join, both games go to the VS menu together: you're player 1, they're player 2, "
               @"and you pick characters, CPUs, teams and the stage just like on one console."];
    UILabel* error = [s addText:@""];
    error.textColor = [UIColor colorWithRed:1 green:0.45 blue:0.45 alpha:1];
    __weak PortMenuScreen* w = s;
    [s addButton:@"Share invite link" primary:NO action:^{
        if (!HasServer()) {
            error.text = @"Set the server first (Online settings).";
            return;
        }
        UIActivityViewController* share =
            [[UIActivityViewController alloc] initWithActivityItems:@[ [NSURL URLWithString:InviteLink(room)] ]
                                              applicationActivities:nil];
        share.popoverPresentationController.sourceView = w.view;
        [w presentViewController:share animated:YES completion:nil];
    }];
    [s addButton:@"New room code" primary:NO action:^{
        room = NewRoomCode();
        code.text = room;
    }];
    [s addButton:@"Open the room" primary:YES action:^{
        if (!HasServer()) {
            error.text = @"Set the server first (Online settings).";
            return;
        }
        Copy(sState->room, sizeof(sState->room), room);
        Finish(PORT_IOS_MENU_HOST);
    }];
    return s;
}

static PortMenuScreen* MakeSettings(void) {
    PortMenuScreen* s = [PortMenuScreen new];
    s.title = @"Settings";
    s.view.hidden = NO;
    [s addTitle:@"Online settings"];
    [s addText:@"Usually filled in by an invite link. Server is address:port."];
    UITextField* server = [s addField:@"Server (e.g. 203.0.113.5:7780)" text:Str(sState->server) secure:NO];
    server.keyboardType = UIKeyboardTypeURL;
    UITextField* password = [s addField:@"Password" text:Str(sState->password) secure:YES];
    [s addText:@"Voice chat with the other player (headphones avoid echo):"];
    UISegmentedControl* voice = [[UISegmentedControl alloc] initWithItems:@[ @"Off", @"Open mic", @"Push to talk" ]];
    voice.selectedSegmentIndex = sState->voice;
    [s.stack addArrangedSubview:voice];
    __weak PortMenuScreen* w = s;
    [s addButton:@"Save" primary:YES action:^{
        Copy(sState->server, sizeof(sState->server),
             [server.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet]);
        Copy(sState->password, sizeof(sState->password), password.text);
        sState->voice = (int)voice.selectedSegmentIndex;
        sState->settings_changed = 1;
        [w.navigationController popToRootViewControllerAnimated:YES];
    }];
    return s;
}

static PortMenuScreen* MakeStart(void) {
    PortMenuScreen* s = [PortMenuScreen new];
    s.title = @"BattleShip";
    s.view.hidden = NO;
    __weak PortMenuScreen* w = s;
    [s addTitle:@"BattleShip"];
    [s addButton:@"Play locally" primary:YES action:^{ Finish(PORT_IOS_MENU_LOCAL); }];
    [s addButton:@"Play online" primary:YES action:^{ [w.navigationController pushViewController:MakeOnline() animated:YES]; }];
    return s;
}

extern "C" int port_ios_start_menu(PortIosMenuState* state) {
    UIWindowScene* scene = nil;
    for (UIScene* sc in UIApplication.sharedApplication.connectedScenes) {
        if ([sc isKindOfClass:[UIWindowScene class]]) {
            scene = (UIWindowScene*)sc;
            break;
        }
    }
    if (scene == nil) {
        return PORT_IOS_MENU_LOCAL;
    }
    sState = state;
    sMenuChoice = -1;
    UINavigationController* nav = [[UINavigationController alloc] initWithRootViewController:MakeStart()];
    nav.navigationBar.barStyle = UIBarStyleBlack;
    nav.navigationBar.tintColor = UIColor.whiteColor;
    nav.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
    sMenuWindow = [[UIWindow alloc] initWithWindowScene:scene];
    sMenuWindow.rootViewController = nav;
    [sMenuWindow makeKeyAndVisible];

    while (sMenuChoice < 0) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
    }
    sMenuWindow.hidden = YES;
    sMenuWindow = nil;
    sState = nullptr;
    return sMenuChoice;
}
