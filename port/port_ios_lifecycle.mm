/* iOS: pause the game while the app isn't active (switched away, Control
 * Center, a call, the lock screen).
 *
 * With the UIScene lifecycle (required by the iOS 27 SDK) UIKit never calls
 * the app-delegate callbacks SDL2 listens to, so SDL never reports going to
 * the background and the game kept rendering while hidden. GPU work from the
 * background fails, and the drawables tied up in those failed frames left
 * the game at about one frame per second after coming back. Instead, the
 * main loop now waits here (pumping the run loop, without rendering) until
 * the scene is active again. Online, the other player's game holds for us
 * meanwhile (netpeer). Built with ARC; everything runs on the main thread. */
#import <UIKit/UIKit.h>

#include <SDL.h>

#include <chrono>

extern "C" void port_log(const char* fmt, ...);
extern "C" void port_watchdog_note_yield(void);
extern "C" int32_t AudioPlayerBuffered(void);

static bool sActive = true;
static int sFramesSinceResume = -1;  // logs the audio queue for a few seconds after resuming

__attribute__((constructor)) static void PortIosLifecycleInstall(void) {
    NSNotificationCenter* nc = NSNotificationCenter.defaultCenter;
    [nc addObserverForName:UISceneWillDeactivateNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification*) {
                  sActive = false;
                }];
    [nc addObserverForName:UISceneDidActivateNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification*) {
                  sActive = true;
                }];
}

/* Called at the top of every frame: returns at once while the app is
 * active, otherwise waits until it is. */
extern "C" void port_ios_wait_while_inactive(void) {
    if (sActive) {
        if (sFramesSinceResume >= 0 && (++sFramesSinceResume % 60) == 0) {
            // Playing: stays around the game's target. Stuck output: grows.
            port_log("ios: %d s after resuming, audio queued=%d frames\n", sFramesSinceResume / 60,
                     (int)AudioPlayerBuffered());
            if (sFramesSinceResume >= 300) {
                sFramesSinceResume = -1;
            }
        }
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    port_log("ios: app inactive, game paused\n");
    while (!sActive) {
        SDL_PumpEvents();  // runs the main run loop: UIKit's notifications arrive here
        if (!sActive) {
            SDL_Delay(20);
        }
        port_watchdog_note_yield();
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    port_log("ios: app active again after %lld ms, game resumed (audio queued=%d frames)\n", (long long)ms.count(),
             (int)AudioPlayerBuffered());
    sFramesSinceResume = 0;
}
