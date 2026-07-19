#include "BrowserWindow.hpp"

#include <Geode/binding/CCTextInputNode.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/utils/general.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/Scrollbar.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>

// A row in the downloads/history list popup.
struct BrowserListRow {
    std::string title;
    std::string subtitle;
    std::function<void()> onOpen;    // primary click (navigate / open folder)
    std::function<void()> onRemove;  // optional X action
};

// A dark, scrollable, Chrome-inspired list panel (downloads / history).
class BrowserListPopup : public geode::Popup {
protected:
    void build(std::string const& title, std::vector<BrowserListRow> const& rows) {
        auto size = m_size;
        // The rounded frame acts as a darker blue border; an exact-colour
        // CCLayerColor fills the interior with the same indigo as the window
        // (tinting the dark panel texture can't reach a bright colour).
        if (m_bgSprite) { m_bgSprite->setColor({34, 44, 96}); m_bgSprite->setZOrder(-2); }
        auto* fill = CCLayerColor::create({51, 68, 153, 255}, size.width - 8.f, size.height - 8.f);
        fill->ignoreAnchorPointForPosition(true);
        fill->setPosition({4.f, 4.f});
        // Above the background, below the title/close button/scroll (all z >= 0).
        m_mainLayer->addChild(fill, -1);

        this->setTitle(title.c_str());
        float pad = 18.f;
        float rowH = 42.f;
        CCSize area{size.width - pad * 2.f - 8.f, size.height - 58.f};

        auto scroll = geode::ScrollLayer::create(area);
        scroll->setPosition({pad, 14.f});
        m_mainLayer->addChild(scroll);

        // Visible scrollbar down the right edge of the list.
        auto* bar = geode::Scrollbar::create(scroll);
        bar->setPosition({pad + area.width + 10.f, 14.f + area.height / 2.f});
        m_mainLayer->addChild(bar);

        float totalH = std::max(area.height, rows.size() * (rowH + 6.f) + 6.f);
        scroll->m_contentLayer->setContentSize({area.width, totalH});

        float y = totalH - rowH - 6.f;
        for (auto const& row : rows) {
            auto* card = CCScale9Sprite::create("square02_001.png");
            card->setContentSize({area.width, rowH});
            card->setAnchorPoint({0.f, 0.f});
            card->setPosition({0.f, y});
            card->setColor({28, 30, 38});
            card->setOpacity(235);
            scroll->m_contentLayer->addChild(card);

            auto* menu = CCMenu::create();
            menu->setPosition({0.f, 0.f});
            card->addChild(menu);

            auto* title = CCLabelBMFont::create(row.title.c_str(), "bigFont.fnt");
            title->setAnchorPoint({0.f, 0.5f});
            title->setScale(.34f);
            title->limitLabelWidth(area.width - 70.f, .34f, .1f);
            auto* titleBtn = CCMenuItemSpriteExtra::create(title, this,
                menu_selector(BrowserListPopup::onRowAction));
            titleBtn->setPosition({12.f + title->getScaledContentSize().width / 2.f, rowH - 13.f});
            menu->addChild(titleBtn);
            if (row.onOpen) m_buttonMap[titleBtn] = row.onOpen;

            if (!row.subtitle.empty()) {
                auto* sub = CCLabelBMFont::create(row.subtitle.c_str(), "chatFont.fnt");
                sub->setAnchorPoint({0.f, 0.5f});
                sub->setScale(.5f);
                sub->setColor({150, 160, 175});
                sub->limitLabelWidth(area.width - 70.f, .5f, .1f);
                sub->setPosition({12.f, 11.f});
                card->addChild(sub);
            }

            if (row.onRemove) {
                auto* x = CCLabelBMFont::create("X", "bigFont.fnt");
                x->setScale(.4f);
                auto* xBtn = CCMenuItemSpriteExtra::create(x, this,
                    menu_selector(BrowserListPopup::onRowAction));
                xBtn->setPosition({area.width - 18.f, rowH / 2.f});
                menu->addChild(xBtn);
                m_buttonMap[xBtn] = row.onRemove;
            }
            y -= rowH + 6.f;
        }
        scroll->scrollToTop();
    }

    void onRowAction(CCObject* sender) {
        auto it = m_buttonMap.find(sender);
        if (it != m_buttonMap.end() && it->second) it->second();
    }

public:
    static BrowserListPopup* create(std::string const& title, std::vector<BrowserListRow> const& rows) {
        auto* ret = new BrowserListPopup();
        if (ret->init(380.f, 280.f, "square02_001.png")) {
            ret->autorelease();
            ret->build(title, rows);
            return ret;
        }
        delete ret;
        return nullptr;
    }

private:
    std::map<CCObject*, std::function<void()>> m_buttonMap;
};

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

namespace {
constexpr float kHeaderHeight = 48.f;
constexpr float kInset = 4.f;
constexpr float kMinWidth = 300.f;
constexpr float kMinHeight = 170.f;

std::string shortTitle(std::string title) {
    if (title.empty()) return "New tab";
    if (title.size() > 15) title = title.substr(0, 14) + "...";
    return title;
}
}

BrowserWindow* BrowserWindow::create() {
    auto* result = new BrowserWindow();
    if (result->init()) {
        result->autorelease();
        return result;
    }
    delete result;
    return nullptr;
}

BrowserWindow::~BrowserWindow() {
    BrowserHost::get().onStateChanged = {};
    BrowserHost::get().onMiddleClick = {};
    BrowserHost::get().onDownloadNotice = {};
    BrowserHost::get().onContextMenu = {};
    BrowserHost::get().contextMenuCancel();
    CC_SAFE_RELEASE(m_pageTexture);
}

bool BrowserWindow::init() {
    if (!CCNode::init()) return false;
    setAnchorPoint({0.f, 0.f});

    m_background = geode::NineSlice::create("square02_001.png");
    m_background->setColor({10, 12, 16});
    m_background->setOpacity(242);
    addChild(m_background, -1);

    m_border = geode::NineSlice::create("square02_001.png");
    m_border->setColor({51, 68, 153});
    m_border->setOpacity(255);
    addChild(m_border, -2);

    // Colored strip behind the toolbar/tabs (the "top bar"). An exact RGB fill
    // (CCLayerColor) clipped by a rounded stencil so the top corners match the
    // window's rounded corners; the bottom is squared off where it meets the
    // page. The stencil = a rounded scale9 sprite (top) OR'd with a plain rect
    // (bottom); tint of the stencil is irrelevant since only its alpha is used.
    m_topBar = CCClippingNode::create();
    m_topBar->setAlphaThreshold(0.05f);
    auto* stencil = CCNode::create();
    m_topBarRound = CCScale9Sprite::create("square02_001.png");
    m_topBarRound->setAnchorPoint({0.f, 0.f});
    stencil->addChild(m_topBarRound);
    m_topBarSquare = CCLayerColor::create({255, 255, 255, 255});
    m_topBarSquare->ignoreAnchorPointForPosition(true);
    stencil->addChild(m_topBarSquare);
    m_topBar->setStencil(stencil);
    m_topBarFill = CCLayerColor::create({51, 68, 153, 255});
    m_topBarFill->ignoreAnchorPointForPosition(true);
    m_topBar->addChild(m_topBarFill);
    addChild(m_topBar, 0);

    m_pageSprite = CCSprite::create();
    m_pageSprite->setAnchorPoint({0.f, 0.f});
    m_pageSprite->setPosition({kInset, kInset});
    m_pageSprite->setVisible(false);
    addChild(m_pageSprite, 1);

    m_tabMenu = CCMenu::create();
    m_tabMenu->setAnchorPoint({0.f, 0.f});
    m_tabMenu->setPosition({0.f, 0.f});
    addChild(m_tabMenu, 4);

    m_toolMenu = CCMenu::create();
    m_toolMenu->setAnchorPoint({0.f, 0.f});
    m_toolMenu->setPosition({0.f, 0.f});
    addChild(m_toolMenu, 4);

    m_toolMenu->addChild(makeButton("<", menu_selector(BrowserWindow::onBack)));
    m_toolMenu->addChild(makeButton(">", menu_selector(BrowserWindow::onForward)));
    m_toolMenu->addChild(makeButton("R", menu_selector(BrowserWindow::onReload)));
    m_toolMenu->addChild(makeButton("Go", menu_selector(BrowserWindow::onGo)));
    m_toolMenu->addChild(makeButton("...", menu_selector(BrowserWindow::onMenu), .42f));

    // The "..." dropdown, hidden until opened.
    m_dropMenu = CCMenu::create();
    m_dropMenu->setAnchorPoint({0.f, 1.f});
    m_dropMenu->setVisible(false);
    addChild(m_dropMenu, 7);
    struct { char const* label; SEL_MenuHandler cb; } items[] = {
        {"Copy URL", menu_selector(BrowserWindow::onCopyUrl)},
        {"Downloads", menu_selector(BrowserWindow::onDownloads)},
        {"History", menu_selector(BrowserWindow::onHistory)},
        {"Mute", menu_selector(BrowserWindow::onMute)},
        {"Settings", menu_selector(BrowserWindow::onSettings)},
        {"DevTools", menu_selector(BrowserWindow::onDevTools)},
    };
    float dy = 0.f;
    for (auto const& item : items) {
        auto* btn = makeButton(item.label, item.cb, .40f);
        btn->setPosition({45.f, dy});
        m_dropMenu->addChild(btn);
        if (std::string(item.label) == "Mute") m_muteButton = btn;
        dy -= 22.f;
    }

    m_address = TextInput::create(250.f, "URL or search");
    m_address->setCommonFilter(CommonFilter::Any);
    m_address->setScale(.62f);
    m_address->setID("address-input");
    addChild(m_address, 5);

    // Clear (X) button at the right end of the address bar.
    auto* clearSpr = CCSprite::createWithSpriteFrameName("GJ_deleteIcon_001.png");
    clearSpr->setScale(.5f);
    m_clearButton = CCMenuItemSpriteExtra::create(clearSpr, this, menu_selector(BrowserWindow::onClearUrl));
    m_toolMenu->addChild(m_clearButton);

    m_toast = CCLabelBMFont::create("", "bigFont.fnt");
    m_toast->setScale(.5f);
    m_toast->setOpacity(0);
    m_toast->setID("download-toast");
    addChild(m_toast, 8);

    m_spinner = CCSprite::create("loadingCircle.png");
    m_spinner->setScale(.18f);
    m_spinner->setBlendFunc({GL_SRC_ALPHA, GL_ONE});
    m_spinner->setVisible(false);
    addChild(m_spinner, 5);

    m_status = CCLabelBMFont::create("Starting Chromium...", "chatFont.fnt");
    m_status->setScale(.55f);
    m_status->setOpacity(180);
    m_status->setID("browser-status");
    addChild(m_status, 2);

    m_resizeHandle = CCScale9Sprite::create("square02_001.png");
    m_resizeHandle->setContentSize({10.f, 10.f});
    m_resizeHandle->setColor({160, 190, 255});
    m_resizeHandle->setOpacity(210);
    addChild(m_resizeHandle, 6);

    auto win = CCDirector::get()->getWinSize();
    auto width = Mod::get()->getSavedValue<float>("window-width", std::min(520.f, win.width - 20.f));
    auto height = Mod::get()->getSavedValue<float>("window-height", std::min(290.f, win.height - 20.f));
    setContentSize({width, height});
    setPosition({
        Mod::get()->getSavedValue<float>("window-x", (win.width - width) / 2.f),
        Mod::get()->getSavedValue<float>("window-y", (win.height - height) / 2.f)
    });

    scheduleUpdate();
    WeakRef<BrowserWindow> self(this);
    BrowserHost::get().onStateChanged = [self] {
        geode::queueInMainThread([self] {
            if (auto window = self.lock()) window->syncFromBrowser();
        });
    };
    BrowserHost::get().onMiddleClick = [self](int x, int y) -> bool {
        auto window = self.lock();
        return window && window->closeTabAtClientPixels(x, y);
    };
    BrowserHost::get().onDownloadNotice = [self](std::string msg) {
        geode::queueInMainThread([self, msg = std::move(msg)] {
            if (auto window = self.lock()) window->showToast(msg);
        });
    };
    BrowserHost::get().onContextMenu = [self](int x, int y, std::vector<BrowserHost::ContextMenuItem> items) {
        if (auto window = self.lock()) window->showContextMenu(x, y, std::move(items));
        else BrowserHost::get().contextMenuCancel();
    };
    BrowserHost::get().initialize();
    if (BrowserHost::get().tabCount() == 0) BrowserHost::get().addTab();
    rebuildTabs();
    return true;
}

CCMenuItemSpriteExtra* BrowserWindow::makeButton(char const* text, SEL_MenuHandler callback, float scale) {
    auto* sprite = ButtonSprite::create(text);
    sprite->setScale(scale);
    return CCMenuItemSpriteExtra::create(sprite, this, callback);
}

void BrowserWindow::onEnter() {
    CCNode::onEnter();
    CCTouchDispatcher::get()->addTargetedDelegate(this, 10000, true);
}

void BrowserWindow::onExit() {
    CCTouchDispatcher::get()->removeDelegate(this);
    CCNode::onExit();
}

void BrowserWindow::update(float dt) {
    if (!m_browserVisible || !isRunning()) return;
    // Rebuilding the tab bar destroys its buttons; doing that while a click
    // is between press and release crashes CCMenu. Wait until the mouse is up.
    if (m_tabsDirty && !(GetKeyState(VK_LBUTTON) & 0x8000)) {
        m_tabsDirty = false;
        rebuildTabs();
    }

    // When the address bar gains focus show the full URL for editing; when it
    // loses focus restore the (possibly truncated) display. Swap only on the
    // transition so typing is never interrupted.
    auto* inputNode = m_address->getInputNode();
    bool focused = inputNode && inputNode->m_selected;
    // While editing the URL, take keyboard focus away from the CEF page so
    // typed characters reach the cocos input (the WndProc hook only forwards
    // keys to Chromium while the page holds focus).
    if (focused && BrowserHost::get().hasKeyboardFocus()) {
        BrowserHost::get().setKeyboardFocus(false);
    }
    if (focused && !m_addrWasFocused) {
        // Gained focus: show the full URL for editing — unless the user just
        // pressed X to clear it, in which case leave the field empty.
        if (!m_addrCleared) m_address->setString(m_fullUrl);
    }
    // Note: we deliberately do NOT reset the field text when it loses focus.
    // Clicking "Go" blurs the input on touch-down, and resetting here would
    // clobber the typed URL before onGo() (fired on touch-up) can read it.
    // The field re-syncs to the real URL via syncFromBrowser() on navigation.
    if (focused) m_addrCleared = false;
    m_addrWasFocused = focused;

    // Loading spinner, Chrome style.
    auto const* tab = BrowserHost::get().activeTab();
    bool loading = tab && tab->loading;
    m_spinner->setVisible(loading);
    if (loading) m_spinner->setRotation(m_spinner->getRotation() + dt * 540.f);

    updateNativeBounds();
    uploadFrame();
}

void BrowserWindow::uploadFrame() {
    int width = 0, height = 0;
    if (!BrowserHost::get().copyActiveFrame(m_frame, width, height)) return;

    if (!m_pageTexture || width != m_textureWidth || height != m_textureHeight) {
        CC_SAFE_RELEASE(m_pageTexture);
        m_pageTexture = new CCTexture2D();
        m_pageTexture->initWithData(
            m_frame.data(), kCCTexture2DPixelFormat_RGBA8888,
            static_cast<unsigned>(width), static_cast<unsigned>(height),
            CCSize(static_cast<float>(width), static_cast<float>(height))
        );
        m_pageTexture->setAntiAliasTexParameters();
        m_textureWidth = width;
        m_textureHeight = height;
        m_pageSprite->setTexture(m_pageTexture);
        // setTextureRect is in points; the quad multiplies by the content
        // scale factor to get texels, so divide it back out.
        float csf = CC_CONTENT_SCALE_FACTOR();
        m_pageSprite->setTextureRect({0.f, 0.f, width / csf, height / csf});
        layoutPageSprite();
    }

    // CEF hands us BGRA; desktop GL happily reorders on upload.
    ccGLBindTexture2D(m_pageTexture->getName());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, m_frame.data());

    m_pageSprite->setVisible(true);
    if (m_status->isVisible() && BrowserHost::get().error().empty()) m_status->setVisible(false);
}

void BrowserWindow::layoutPageSprite() {
    if (!m_pageSprite || m_textureWidth <= 0 || m_textureHeight <= 0) return;
    auto sprite = m_pageSprite->getContentSize();
    if (sprite.width <= 0.f || sprite.height <= 0.f) return;
    auto size = getContentSize();
    float areaWidth = size.width - kInset * 2.f;
    float areaHeight = size.height - kHeaderHeight - kInset;
    m_pageSprite->setScaleX(areaWidth / sprite.width);
    m_pageSprite->setScaleY(areaHeight / sprite.height);
}

void BrowserWindow::setContentSize(CCSize const& requested) {
    auto win = CCDirector::get()->getWinSize();
    CCSize size{
        std::clamp(requested.width, std::min(kMinWidth, win.width - 10.f), win.width - 10.f),
        std::clamp(requested.height, std::min(kMinHeight, win.height - 10.f), win.height - 10.f)
    };
    CCNode::setContentSize(size);
    Mod::get()->setSavedValue("window-width", size.width);
    Mod::get()->setSavedValue("window-height", size.height);
    if (m_background) {
        m_background->setContentSize(size - CCSize{2.f, 2.f});
        m_background->setPosition(size / 2.f);
    }
    if (m_border) {
        m_border->setContentSize(size);
        m_border->setPosition(size / 2.f);
    }
    if (m_topBar) {
        float bw = size.width - 2.f;
        m_topBar->setPosition({1.f, size.height - kHeaderHeight - 1.f});
        if (m_topBarRound) {
            m_topBarRound->setContentSize({bw, kHeaderHeight});
            m_topBarRound->setPosition({0.f, 0.f});
        }
        if (m_topBarSquare) {  // covers all but the top ~12px → only top corners stay round
            m_topBarSquare->setContentSize({bw, kHeaderHeight - 12.f});
            m_topBarSquare->setPosition({0.f, 0.f});
        }
        if (m_topBarFill) {
            m_topBarFill->setContentSize({bw, kHeaderHeight});
            m_topBarFill->setPosition({0.f, 0.f});
        }
    }
    layoutChrome();
    layoutPageSprite();
}

void BrowserWindow::setPosition(CCPoint const& requested) {
    auto win = CCDirector::get()->getWinSize();
    auto size = getContentSize();
    CCPoint position{
        std::clamp(requested.x, -size.width + 30.f, win.width - 30.f),
        std::clamp(requested.y, -size.height + 30.f, win.height - 30.f)
    };
    CCNode::setPosition(position);
    Mod::get()->setSavedValue("window-x", position.x);
    Mod::get()->setSavedValue("window-y", position.y);
}

void BrowserWindow::layoutChrome() {
    if (!m_tabMenu || !m_toolMenu) return;
    auto size = getContentSize();

    m_resizeHandle->setPosition({size.width - 7.f, 7.f});
    m_status->setPosition(size / 2.f - CCPoint{0.f, 12.f});

    float x = 16.f;
    auto tools = m_toolMenu->getChildrenExt<CCMenuItemSpriteExtra>();
    if (tools.size() >= 6) {
        float row = size.height - 36.f;
        tools[0]->setPosition({x, row}); x += 24.f;  // <
        tools[1]->setPosition({x, row}); x += 24.f;  // >
        tools[2]->setPosition({x, row}); x += 16.f;  // R
        m_spinner->setPosition({x, row}); x += 16.f;

        float rightTools = 70.f;
        tools[3]->setPosition({size.width - rightTools - 18.f, row});  // Go
        tools[4]->setPosition({size.width - 27.f, row});               // ...
        if (m_dropMenu) m_dropMenu->setPosition({size.width - 92.f, size.height - 46.f});

        // Clear (X) button sits just inside the right edge of the address bar.
        float clearX = size.width - rightTools - 40.f;
        tools[5]->setPosition({clearX, row});

        float inputWidth = std::max(80.f, clearX - 10.f - x);
        m_address->setContentWidth(inputWidth / m_address->getScale());
        m_address->setPosition({x + inputWidth / 2.f, row});
    }
    m_tabsDirty = true;
}

void BrowserWindow::rebuildTabs() {
    if (!m_tabMenu) return;
    m_tabMenu->removeAllChildrenWithCleanup(true);
    auto& browser = BrowserHost::get();
    auto size = getContentSize();
    float available = std::max(100.f, size.width - 80.f);
    size_t shown = std::min<size_t>(browser.tabCount(), 7);
    size_t first = 0;
    if (browser.activeIndex() >= shown && shown > 0) {
        first = browser.activeIndex() - shown + 1;
    }
    float tabWidth = shown ? std::min(74.f, available / static_cast<float>(shown)) : 74.f;

    for (size_t slot = 0; slot < shown; ++slot) {
        size_t i = first + slot;
        auto const* tab = browser.tabAt(i);
        std::string title = tab ? tab->title : "Tab " + std::to_string(i + 1);
        auto* button = makeButton(shortTitle(title).c_str(), menu_selector(BrowserWindow::onTab), .30f);
        button->setTag(static_cast<int>(i));
        button->setPosition({12.f + tabWidth * (static_cast<float>(slot) + .5f), size.height - 12.f});
        if (i == browser.activeIndex()) button->setColor({150, 200, 255});
        m_tabMenu->addChild(button);
    }
    auto* add = makeButton("+", menu_selector(BrowserWindow::onNewTab), .35f);
    add->setPosition({18.f + tabWidth * static_cast<float>(shown), size.height - 12.f});
    m_tabMenu->addChild(add);
    auto* closeTab = makeButton("x", menu_selector(BrowserWindow::onCloseTab), .32f);
    closeTab->setPosition({size.width - 39.f, size.height - 12.f});
    m_tabMenu->addChild(closeTab);
    auto* close = makeButton("X", menu_selector(BrowserWindow::onClose), .32f);
    close->setPosition({size.width - 14.f, size.height - 12.f});
    m_tabMenu->addChild(close);
}

void BrowserWindow::syncFromBrowser() {
    auto& browser = BrowserHost::get();
    if (!browser.error().empty()) {
        m_status->setString(browser.error().c_str());
        m_status->setVisible(true);
    } else {
        m_status->setVisible(browser.tabCount() == 0 || !browser.activeTab() || !browser.activeTab()->ready);
        if (m_status->isVisible()) m_status->setString("Starting Chromium...");
    }
    if (auto* tab = browser.activeTab()) {
        m_fullUrl = tab->url;
        // Truncate very long URLs for display so they stay readable; the full
        // URL is shown when the bar is focused for editing.
        m_addrDisplay = m_fullUrl.size() > 52 ? m_fullUrl.substr(0, 50) + "..." : m_fullUrl;
        // Only update the field when the user is not editing it.
        auto* inputNode = m_address->getInputNode();
        if ((!inputNode || !inputNode->m_selected) && m_address->getString() != m_addrDisplay) {
            m_address->setString(m_addrDisplay);
        }
        auto* sprite = static_cast<ButtonSprite*>(m_muteButton->getNormalImage());
        sprite->setString(tab->muted ? "Unmute" : "Mute");
    }
    m_tabsDirty = true;
}

void BrowserWindow::showBrowser(bool visible) {
    m_browserVisible = visible;
    setVisible(visible);
    BrowserHost::get().setVisible(visible);
    if (visible) updateNativeBounds();
}

void BrowserWindow::updateNativeBounds() {
    auto win = CCDirector::get()->getWinSize();
    auto origin = convertToWorldSpace({kInset, kInset});
    CCSize browserSize{getContentWidth() - kInset * 2.f, getContentHeight() - kHeaderHeight - kInset};

    HWND hwnd = BrowserHost::get().parentWindow();
    if (!hwnd) return;
    hwnd = GetAncestor(hwnd, GA_ROOT);
    RECT client{};
    GetClientRect(hwnd, &client);
    float sx = static_cast<float>(client.right - client.left) / win.width;
    float sy = static_cast<float>(client.bottom - client.top) / win.height;

    RECT bounds{
        static_cast<LONG>(origin.x * sx),
        static_cast<LONG>((win.height - origin.y - browserSize.height) * sy),
        static_cast<LONG>((origin.x + browserSize.width) * sx),
        static_cast<LONG>((win.height - origin.y) * sy)
    };
    BrowserHost::get().setBounds(bounds);

    // Also report the whole window (including chrome) so game input is blocked
    // anywhere over the window, not just over the rendered page.
    auto winOrigin = convertToWorldSpace({0.f, 0.f});
    CCSize full{getContentWidth(), getContentHeight()};
    RECT windowBounds{
        static_cast<LONG>(winOrigin.x * sx),
        static_cast<LONG>((win.height - winOrigin.y - full.height) * sy),
        static_cast<LONG>((winOrigin.x + full.width) * sx),
        static_cast<LONG>((win.height - winOrigin.y) * sy)
    };
    BrowserHost::get().setWindowBounds(windowBounds);
}

CCPoint BrowserWindow::worldToClientPixels(CCPoint const& world) const {
    auto win = CCDirector::get()->getWinSize();
    HWND hwnd = BrowserHost::get().parentWindow();
    if (!hwnd) return world;
    hwnd = GetAncestor(hwnd, GA_ROOT);
    RECT client{};
    GetClientRect(hwnd, &client);
    float sx = static_cast<float>(client.right - client.left) / win.width;
    float sy = static_cast<float>(client.bottom - client.top) / win.height;
    return {world.x * sx, (win.height - world.y) * sy};
}

CCPoint BrowserWindow::clientPixelsToWorld(int clientX, int clientY) const {
    auto win = CCDirector::get()->getWinSize();
    HWND hwnd = BrowserHost::get().parentWindow();
    if (!hwnd) return {static_cast<float>(clientX), static_cast<float>(clientY)};
    hwnd = GetAncestor(hwnd, GA_ROOT);
    RECT client{};
    GetClientRect(hwnd, &client);
    float sx = static_cast<float>(client.right - client.left) / win.width;
    float sy = static_cast<float>(client.bottom - client.top) / win.height;
    return {clientX / sx, win.height - clientY / sy};
}

bool BrowserWindow::closeTabAtClientPixels(int clientX, int clientY) {
    if (!m_browserVisible || !m_tabMenu) return false;
    auto world = clientPixelsToWorld(clientX, clientY);
    auto local = m_tabMenu->convertToNodeSpace(world);
    for (auto* button : m_tabMenu->getChildrenExt<CCMenuItemSpriteExtra>()) {
        // Only real tab buttons carry a tag (>= 0); +, x, X do not.
        if (button->getTag() >= 0 && button->boundingBox().containsPoint(local)) {
            BrowserHost::get().closeTab(static_cast<size_t>(button->getTag()));
            return true;
        }
    }
    return false;
}

bool BrowserWindow::inside(CCNode* node, CCPoint const& world) const {
    auto point = node->getParent()->convertToNodeSpace(world);
    return node->boundingBox().containsPoint(point);
}

bool BrowserWindow::ccTouchBegan(CCTouch* touch, CCEvent*) {
    if (!m_browserVisible) return false;
    // A click that reaches here wasn't on a context-menu item (CCMenu eats
    // those first), so dismiss any open context menu.
    if (m_contextMenu) {
        hideContextMenu();
        BrowserHost::get().contextMenuCancel();
    }
    auto world = touch->getLocation();
    auto local = convertToNodeSpace(world);
    if (local.x < 0.f || local.y < 0.f || local.x > getContentWidth() || local.y > getContentHeight()) {
        // Clicking outside the browser window (on the game) releases the
        // keyboard back to GD; clicking anywhere on the window keeps it.
        BrowserHost::get().setKeyboardFocus(false);
        return false;
    }
    m_touchStart = world;
    m_startPosition = getPosition();
    m_startSize = getContentSize();
    if (inside(m_resizeHandle, world)) {
        m_resizing = true;
        return true;
    }
    if (local.y >= getContentHeight() - 24.f) {
        m_dragging = true;
        return true;
    }
    // Inside the rendered page: forward the click to Chromium.
    if (local.x >= kInset && local.x <= getContentWidth() - kInset
        && local.y >= kInset && local.y <= getContentHeight() - kHeaderHeight) {
        m_pageTouch = true;
        auto client = worldToClientPixels(world);
        BrowserHost::get().setKeyboardFocus(true);
        BrowserHost::get().sendMouseButton(
            static_cast<int>(client.x), static_cast<int>(client.y), false, 1);
        return true;
    }
    // URL bar: focus it directly. The text-input node's own touch handling
    // intermittently misses, so this guarantees a click here starts editing.
    if (m_address && inside(m_address, world)) {
        BrowserHost::get().setKeyboardFocus(false);  // keys go to the URL bar, not the page
        m_address->focus();
        return true;  // consume so the click can't fall through to the game
    }
    // Any other click inside the window (toolbar, tabs, empty chrome) is
    // swallowed so it can't fall through to the game underneath. Raw-input
    // jump/pause is separately blocked via isCursorOverWindow().
    return true;
}

void BrowserWindow::ccTouchMoved(CCTouch* touch, CCEvent*) {
    auto delta = touch->getLocation() - m_touchStart;
    if (m_dragging) setPosition(m_startPosition + delta);
    if (m_resizing) {
        setContentSize(m_startSize + CCSize{delta.x, -delta.y});
        setPosition({m_startPosition.x, m_startPosition.y + m_startSize.height - getContentHeight()});
        // Reposition the tab bar live instead of deferring to mouse-up. Safe
        // here because the touch being dragged is the resize handle, not a tab
        // button (the case the deferral protects against).
        if (m_tabsDirty) {
            m_tabsDirty = false;
            rebuildTabs();
        }
    }
    if (m_pageTouch) {
        auto client = worldToClientPixels(touch->getLocation());
        BrowserHost::get().sendMouseDrag(static_cast<int>(client.x), static_cast<int>(client.y));
    }
}

void BrowserWindow::ccTouchEnded(CCTouch* touch, CCEvent*) {
    if (m_pageTouch) {
        auto client = worldToClientPixels(touch->getLocation());
        BrowserHost::get().sendMouseButton(
            static_cast<int>(client.x), static_cast<int>(client.y), true, 1);
    }
    m_dragging = false;
    m_resizing = false;
    m_pageTouch = false;
    updateNativeBounds();
}

void BrowserWindow::onClose(CCObject*) { BrowserWindowManager::get().hide(); }
void BrowserWindow::onNewTab(CCObject*) { BrowserHost::get().addTab(); }
void BrowserWindow::onTab(CCObject* sender) { BrowserHost::get().selectTab(static_cast<CCNode*>(sender)->getTag()); }
void BrowserWindow::onCloseTab(CCObject*) { BrowserHost::get().closeActiveTab(); }
void BrowserWindow::onBack(CCObject*) { BrowserHost::get().goBack(); }
void BrowserWindow::onForward(CCObject*) { BrowserHost::get().goForward(); }
void BrowserWindow::onReload(CCObject*) { BrowserHost::get().reload(); }
void BrowserWindow::onGo(CCObject*) {
    // If the bar still shows the truncated display (not edited), go to the
    // real full URL instead of the truncated text.
    std::string text = m_address->getString();
    if (text == m_addrDisplay && m_addrDisplay != m_fullUrl) text = m_fullUrl;
    BrowserHost::get().navigate(text);
}

void BrowserWindow::onClearUrl(CCObject*) {
    m_address->setString("");
    m_addrCleared = true;    // suppress the focus-gain URL refill
    m_addrWasFocused = false;
    m_address->focus();      // ready for immediate typing
}

void BrowserWindow::onCopyUrl(CCObject*) {
    if (m_dropMenu) m_dropMenu->setVisible(false);
    geode::utils::clipboard::write(m_fullUrl);
    showToast("URL copied");
}
void BrowserWindow::onMute(CCObject*) {
    if (m_dropMenu) m_dropMenu->setVisible(false);
    BrowserHost::get().toggleMute();
}

void BrowserWindow::onMenu(CCObject*) {
    if (m_dropMenu) m_dropMenu->setVisible(!m_dropMenu->isVisible());
}

void BrowserWindow::hideContextMenu() {
    if (m_contextMenu) {
        m_contextMenu->removeFromParent();
        m_contextMenu = nullptr;
    }
}

void BrowserWindow::showContextMenu(int clientX, int clientY, std::vector<BrowserHost::ContextMenuItem> items) {
    hideContextMenu();

    // Clean the labels: drop the '&' mnemonic markers and blank entries, and
    // collapse runs of separators.
    struct Row { std::string label; int commandId; bool enabled; };
    std::vector<Row> rows;
    for (auto const& item : items) {
        if (item.separator) {
            if (!rows.empty() && !rows.back().label.empty()) rows.push_back({"", 0, false});
            continue;
        }
        std::string label;
        for (char c : item.label) if (c != '&') label += c;
        if (label.empty()) continue;
        rows.push_back({label, item.commandId, item.enabled});
    }
    while (!rows.empty() && rows.back().label.empty()) rows.pop_back();
    if (rows.empty()) { BrowserHost::get().contextMenuCancel(); return; }

    constexpr float rowH = 13.f, sepH = 5.f, fontScale = .6f, padX = 9.f;
    char const* font = "chatFont.fnt";
    float w = 80.f, h = 4.f;
    for (auto const& r : rows) {
        h += r.label.empty() ? sepH : rowH;
        if (!r.label.empty()) {
            auto* probe = CCLabelBMFont::create(r.label.c_str(), font);
            w = std::max(w, probe->getContentSize().width * fontScale + padX * 2.f);
        }
    }
    w = std::min(w, 200.f);

    auto local = convertToNodeSpace(clientPixelsToWorld(clientX, clientY));
    local.x = std::clamp(local.x, 2.f, getContentWidth() - w - 2.f);
    local.y = std::clamp(local.y, h + 2.f, getContentHeight() - 2.f);

    m_contextMenu = CCNode::create();
    m_contextMenu->setContentSize({w, h});
    m_contextMenu->setPosition({local.x, local.y - h});
    addChild(m_contextMenu, 9);

    auto* bg = CCScale9Sprite::create("square02_001.png");
    bg->setContentSize({w, h});
    bg->setAnchorPoint({0.f, 0.f});
    bg->setColor({22, 24, 32});
    bg->setOpacity(250);
    m_contextMenu->addChild(bg);

    auto* menu = CCMenu::create();
    menu->setPosition({0.f, 0.f});
    menu->setContentSize({w, h});
    m_contextMenu->addChild(menu);

    float y = h - 2.f;
    for (auto const& r : rows) {
        if (r.label.empty()) {  // separator line
            auto* line = CCLayerColor::create({255, 255, 255, 40}, w - 12.f, 1.f);
            line->setPosition({6.f, y - sepH / 2.f});
            m_contextMenu->addChild(line);
            y -= sepH;
            continue;
        }
        y -= rowH;
        auto* lbl = CCLabelBMFont::create(r.label.c_str(), font);
        lbl->setScale(fontScale);
        if (!r.enabled) lbl->setOpacity(110);
        auto* btn = CCMenuItemSpriteExtra::create(lbl, this,
            r.enabled ? menu_selector(BrowserWindow::onContextItem) : nullptr);
        btn->setTag(r.commandId);
        // Left-align: place the button so its left edge sits at padX.
        btn->setPosition({padX + lbl->getScaledContentSize().width / 2.f, y + rowH / 2.f});
        menu->addChild(btn);
    }
}

void BrowserWindow::onContextItem(CCObject* sender) {
    int commandId = static_cast<CCNode*>(sender)->getTag();
    hideContextMenu();
    BrowserHost::get().contextMenuCommand(commandId);
}

void BrowserWindow::showToast(std::string const& text) {
    if (!m_toast) return;
    m_toast->setString(text.c_str());
    // Bottom-center of the window, drawn above the page.
    m_toast->setPosition({getContentWidth() / 2.f, 20.f});
    m_toast->stopAllActions();
    m_toast->setOpacity(255);
    m_toast->runAction(CCSequence::create(
        CCDelayTime::create(2.5f),
        CCFadeOut::create(0.5f),
        nullptr));
}

void BrowserWindow::showListPopup(char const* title, std::vector<BrowserListRow> rows) {
    if (m_dropMenu) m_dropMenu->setVisible(false);
    auto* popup = BrowserListPopup::create(title, std::move(rows));
    if (!popup) return;
    popup->show();
    // Reparent above the browser window (which lives high on the overlay).
    if (auto* parent = popup->getParent(); parent && parent != OverlayManager::get()) {
        popup->retain();
        popup->removeFromParentAndCleanup(false);
        OverlayManager::get()->addChild(popup, 20000);
        popup->release();
    }
}

void BrowserWindow::onDownloads(CCObject*) {
    std::vector<BrowserListRow> rows;
    auto const& downloads = BrowserHost::get().downloads();
    // Most recent first, like Chrome.
    for (auto it = downloads.rbegin(); it != downloads.rend(); ++it) {
        auto const& d = *it;
        std::string status = d.canceled ? "Canceled"
            : d.complete ? "Done" : std::to_string(d.percent) + "%";
        std::string path = d.path;
        rows.push_back({d.name, status,
            [path] {
                // Open Explorer with the file selected.
                std::wstring arg = L"/select,\"" +
                    std::filesystem::path(path).wstring() + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
            },
            {}});
    }
    showListPopup("Downloads", std::move(rows));
}

void BrowserWindow::onHistory(CCObject*) {
    std::vector<BrowserListRow> rows;
    auto const& hist = BrowserHost::get().history();
    size_t shown = std::min<size_t>(hist.size(), 50);
    for (size_t i = 0; i < shown; ++i) {
        std::string url = hist[hist.size() - 1 - i];
        rows.push_back({url, "", [url] { BrowserHost::get().navigate(url); }, {}});
    }
    showListPopup("History", std::move(rows));
}

void BrowserWindow::onSettings(CCObject*) {
    if (m_dropMenu) m_dropMenu->setVisible(false);
    geode::openSettingsPopup(Mod::get());
}

void BrowserWindow::onDevTools(CCObject*) {
    if (m_dropMenu) m_dropMenu->setVisible(false);
    BrowserHost::get().openDevTools();
}

BrowserWindowManager& BrowserWindowManager::get() {
    static BrowserWindowManager instance;
    return instance;
}

bool BrowserWindowManager::isOpen() const {
    return Mod::get()->getSavedValue<bool>("browser-open", false);
}

void BrowserWindowManager::show() {
    Mod::get()->setSavedValue("browser-open", true);
    if (!m_window) m_window = BrowserWindow::create();
    if (!m_window->getParent()) OverlayManager::get()->addChild(m_window, 10000);
    m_window->showBrowser(true);
}

void BrowserWindowManager::hide() {
    Mod::get()->setSavedValue("browser-open", false);
    if (!m_window) return;
    m_window->showBrowser(false);
    m_window->removeFromParentAndCleanup(false);
    if (!Mod::get()->getSettingValue<bool>("keep-running-hidden")) {
        BrowserHost::get().shutdown();
        m_window = nullptr;
    }
}

void BrowserWindowManager::destroyFrame() {
    BrowserHost::get().setVisible(false);
    if (!m_window) return;
    m_window->removeFromParent();
    m_window = nullptr;
}

void BrowserWindowManager::toggle() {
    if (isOpen()) hide();
    else show();
}
