#pragma once

#include <Geode/Geode.hpp>
#include <Geode/ui/NineSlice.hpp>
#include <Geode/ui/TextInput.hpp>

#include "BrowserHost.hpp"

#include <string>
#include <vector>

struct BrowserListRow;

using namespace geode::prelude;

class BrowserWindow final : public CCNode, public CCTouchDelegate {
public:
    static BrowserWindow* create();
    ~BrowserWindow() override;

    bool init() override;
    void onEnter() override;
    void onExit() override;
    void update(float) override;
    void setContentSize(CCSize const& size) override;
    void setPosition(CCPoint const& position) override;

    bool ccTouchBegan(CCTouch* touch, CCEvent* event) override;
    void ccTouchMoved(CCTouch* touch, CCEvent* event) override;
    void ccTouchEnded(CCTouch* touch, CCEvent* event) override;

    void showBrowser(bool visible);
    void syncFromBrowser();
    void updateNativeBounds();

private:
    CCMenuItemSpriteExtra* makeButton(char const* text, SEL_MenuHandler callback, float scale = .42f);
    void rebuildTabs();
    void layoutChrome();
    void layoutPageSprite();
    void uploadFrame();
    bool inside(CCNode* node, CCPoint const& world) const;
    CCPoint worldToClientPixels(CCPoint const& world) const;
    CCPoint clientPixelsToWorld(int clientX, int clientY) const;
    bool closeTabAtClientPixels(int clientX, int clientY);

    void onClose(CCObject*);
    void onNewTab(CCObject*);
    void onTab(CCObject*);
    void onCloseTab(CCObject*);
    void onBack(CCObject*);
    void onForward(CCObject*);
    void onReload(CCObject*);
    void onGo(CCObject*);
    void onClearUrl(CCObject*);
    void onCopyUrl(CCObject*);
    void onMute(CCObject*);
    void onMenu(CCObject*);
    void onDownloads(CCObject*);
    void onHistory(CCObject*);
    void onSettings(CCObject*);
    void onDevTools(CCObject*);
    void showListPopup(char const* title, std::vector<BrowserListRow> rows);
    void showContextMenu(int clientX, int clientY, std::vector<BrowserHost::ContextMenuItem> items);
    void hideContextMenu();
    void onContextItem(CCObject*);

    geode::NineSlice* m_background = nullptr;
    geode::NineSlice* m_border = nullptr;
    CCClippingNode* m_topBar = nullptr;      // clips the fill to rounded top corners
    CCLayerColor* m_topBarFill = nullptr;    // the indigo colour
    CCScale9Sprite* m_topBarRound = nullptr; // stencil: rounded shape
    CCLayerColor* m_topBarSquare = nullptr;  // stencil: squares off the bottom corners
    CCScale9Sprite* m_resizeHandle = nullptr;
    CCMenu* m_tabMenu = nullptr;
    CCMenu* m_toolMenu = nullptr;
    CCMenu* m_dropMenu = nullptr;   // the "..." dropdown
    CCNode* m_contextMenu = nullptr;  // in-page right-click menu
    TextInput* m_address = nullptr;
    CCMenuItemSpriteExtra* m_clearButton = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCMenuItemSpriteExtra* m_muteButton = nullptr;

    std::string m_fullUrl;       // active tab's full URL
    std::string m_addrDisplay;   // possibly-truncated text shown when unfocused
    bool m_addrWasFocused = false;
    bool m_addrCleared = false;  // X pressed: don't let focus-gain refill the URL
    CCSprite* m_spinner = nullptr;
    CCLabelBMFont* m_toast = nullptr;

    void showToast(std::string const& text);

    // Off-screen-rendered page, uploaded as a GL texture every frame.
    CCSprite* m_pageSprite = nullptr;
    CCTexture2D* m_pageTexture = nullptr;
    std::vector<uint8_t> m_frame;
    std::vector<uint8_t> m_rowScratch;
    int m_textureWidth = 0;
    int m_textureHeight = 0;

    CCPoint m_touchStart;
    CCPoint m_startPosition;
    CCSize m_startSize;
    bool m_dragging = false;
    bool m_resizing = false;
    bool m_pageTouch = false;
    bool m_browserVisible = false;
    bool m_tabsDirty = false;
};

class BrowserWindowManager final {
public:
    static BrowserWindowManager& get();
    void toggle();
    void show();
    void hide();
    void destroyFrame();
    [[nodiscard]] bool isOpen() const;

private:
    Ref<BrowserWindow> m_window;
};
