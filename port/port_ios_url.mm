/* iOS: route invite links (battleship-net://...) to port_ios_handle_open_url.
 *
 * SDL's scene delegate (libultraship sdl2-uikit-scene-lifecycle.patch) does
 * not handle URLs. Rather than growing that patch, this adds the two scene
 * callbacks at load time:
 *  - scene:willConnectToSession:options:  link that launched the app (handled
 *    before SDL_main runs, so port_ios_prepare already sees the new settings)
 *  - scene:openURLContexts:               link tapped while the app runs
 */
#import <UIKit/UIKit.h>
#import <objc/runtime.h>

extern "C" const char* port_ios_handle_open_url(const char* url, int app_running);

static void ShowAlert(UIScene* scene, const char* text) API_AVAILABLE(ios(13.0)) {
    if (![scene isKindOfClass:[UIWindowScene class]]) {
        return;
    }
    UIViewController* vc = nil;
    for (UIWindow* w in ((UIWindowScene*)scene).windows) {
        if (w.rootViewController != nil) {
            vc = w.rootViewController;
            break;
        }
    }
    if (vc == nil) {
        return;
    }
    while (vc.presentedViewController != nil) {
        vc = vc.presentedViewController;
    }
    UIAlertController* alert = [UIAlertController alertControllerWithTitle:@"BattleShip"
                                                                   message:[NSString stringWithUTF8String:text]
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [vc presentViewController:alert animated:YES completion:nil];
}

static void HandleURLContexts(UIScene* scene, NSSet<UIOpenURLContext*>* contexts, BOOL running)
    API_AVAILABLE(ios(13.0)) {
    for (UIOpenURLContext* ctx in contexts) {
        const char* msg = port_ios_handle_open_url(ctx.URL.absoluteString.UTF8String, running ? 1 : 0);
        if (msg != NULL && running) {
            ShowAlert(scene, msg);
        }
    }
}

typedef void (*WillConnectFn)(id, SEL, UIScene*, UISceneSession*, UISceneConnectionOptions*);
static WillConnectFn sOriginalWillConnect = NULL;

static void WillConnect(id self, SEL cmd, UIScene* scene, UISceneSession* session, UISceneConnectionOptions* options)
    API_AVAILABLE(ios(13.0)) {
    HandleURLContexts(scene, options.URLContexts, NO);
    if (sOriginalWillConnect != NULL) {
        sOriginalWillConnect(self, cmd, scene, session, options);
    }
}

static void OpenURLContexts(id self, SEL cmd, UIScene* scene, NSSet<UIOpenURLContext*>* contexts)
    API_AVAILABLE(ios(13.0)) {
    HandleURLContexts(scene, contexts, YES);
}

__attribute__((constructor)) static void InstallInviteLinkHandlers(void) {
    if (@available(iOS 13.0, *)) {
        Class cls = NSClassFromString(@"SDLUIKitSceneDelegate");
        if (cls == Nil) {
            return;
        }
        SEL will = @selector(scene:willConnectToSession:options:);
        Method m = class_getInstanceMethod(cls, will);
        if (m != NULL) {
            sOriginalWillConnect = (WillConnectFn)method_setImplementation(m, (IMP)WillConnect);
        }
        class_addMethod(cls, @selector(scene:openURLContexts:), (IMP)OpenURLContexts, "v@:@@");
    }
}
