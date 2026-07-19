#include <Geode/Geode.hpp>
#include <Geode/loader/GameEvent.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

#include "BrowserWindow.hpp"

#include <set>

using namespace geode::prelude;

// Geode feeds the game's keyboard through its raw-input pump, which bypasses
// the window-message hook in BrowserHost. Swallow game key dispatch while the
// page has keyboard focus so Space etc. does not also control the game.
// (Esc always returns focus to the game.) Runs before other dispatch hooks
// (e.g. custom-keybinds) so they never see the swallowed keys.
struct BrowserKeySwallow : Modify<BrowserKeySwallow, CCKeyboardDispatcher> {
    static void onModify(auto& self) {
        (void)self.setHookPriority("cocos2d::CCKeyboardDispatcher::dispatchKeyboardMSG", -10000);
    }

    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
        // Swallow game keys only while the page is focused or hovered, so the
        // rest of GD's keyboard keeps working normally.
        auto& host = BrowserHost::get();
        if (host.hasKeyboardFocus() || host.isCursorOverWindow()) return true;
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }
};

// Gameplay input (jump on click/space) also arrives via raw-input paths that
// skip cocos touches and the keyboard dispatcher entirely — including the
// click-between-frames mod. Block it at the gameplay level while typing in
// the page or clicking on it.
struct BrowserGameplayBlock : Modify<BrowserGameplayBlock, GJBaseGameLayer> {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", -10000);
    }

    void handleButton(bool down, int button, bool isPlayer2) {
        auto& host = BrowserHost::get();
        // Track which presses we swallowed so we can also swallow their
        // matching release (otherwise a stray release can resume a paused
        // level), while never eating a release for a press that got through.
        static std::set<int> blocked;
        if (down) {
            if (host.hasKeyboardFocus() || host.isCursorOverWindow()) {
                blocked.insert(button);
                return;
            }
        } else if (blocked.erase(button)) {
            return;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer2);
    }
};

// While the page has keyboard focus, don't let key presses reach the pause
// menu. The debug log proved Space in the pause menu funnels *straight through*
// PauseLayer::onResume (not keyDown, not dispatchKeyboardMSG), which is why
// every earlier guard missed it. So block onResume itself while typing in the
// browser: to resume, the user clicks out of the browser (which drops focus)
// or presses Esc (which returns focus to the game) first.
struct BrowserPauseKeyBlock : Modify<BrowserPauseKeyBlock, PauseLayer> {
    void keyDown(enumKeyCodes key, double timestamp) {
        if (BrowserHost::get().hasKeyboardFocus()) return;
        PauseLayer::keyDown(key, timestamp);
    }
    void onResume(CCObject* sender) {
        if (BrowserHost::get().hasKeyboardFocus()) return;
        PauseLayer::onResume(sender);
    }
};

$on_mod(Loaded) {
    listenForKeybindSettingPresses("toggle-keybind", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) BrowserWindowManager::get().toggle();
    });

    ButtonSettingPressedEvent(Mod::get(), "toggle-browser").listen([](auto) {
        BrowserWindowManager::get().toggle();
    }).leak();
}

$on_game(TexturesLoaded) {
    if (BrowserWindowManager::get().isOpen()) BrowserWindowManager::get().show();
}

$on_game(TexturesUnloaded) {
    BrowserWindowManager::get().destroyFrame();
}
