#include "BrowserWindow.hpp"

#include <Geode/binding/CCTextInputNode.hpp>
#include <Geode/ui/GeodeUI.hpp>

#include <algorithm>
#include <cstring>

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
    m_border->setColor({120, 155, 220});
    m_border->setOpacity(190);
    addChild(m_border, -2);

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
    m_muteButton = makeButton("Sound", menu_selector(BrowserWindow::onMute), .34f);
    m_toolMenu->addChild(m_muteButton);
    m_toolMenu->addChild(makeButton("...", menu_selector(BrowserWindow::onMenu), .42f));

    // The "..." dropdown, hidden until opened.
    m_dropMenu = CCMenu::create();
    m_dropMenu->setAnchorPoint({0.f, 1.f});
    m_dropMenu->setVisible(false);
    addChild(m_dropMenu, 7);
    struct { char const* label; SEL_MenuHandler cb; } items[] = {
        {"Downloads", menu_selector(BrowserWindow::onDownloads)},
        {"History", menu_selector(BrowserWindow::onHistory)},
        {"Settings", menu_selector(BrowserWindow::onSettings)},
        {"DevTools", menu_selector(BrowserWindow::onDevTools)},
    };
    float dy = 0.f;
    for (auto const& item : items) {
        auto* btn = makeButton(item.label, item.cb, .40f);
        btn->setPosition({45.f, dy});
        m_dropMenu->addChild(btn);
        dy -= 22.f;
    }

    m_address = TextInput::create(250.f, "URL or search");
    m_address->setCommonFilter(CommonFilter::Any);
    m_address->setScale(.62f);
    m_address->setID("address-input");
    addChild(m_address, 5);

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
        tools[0]->setPosition({x, size.height - 36.f}); x += 24.f;
        tools[1]->setPosition({x, size.height - 36.f}); x += 24.f;
        tools[2]->setPosition({x, size.height - 36.f}); x += 16.f;
        m_spinner->setPosition({x, size.height - 36.f}); x += 16.f;

        float rightTools = 112.f;
        tools[3]->setPosition({size.width - rightTools - 18.f, size.height - 36.f});
        tools[4]->setPosition({size.width - 72.f, size.height - 36.f});
        tools[5]->setPosition({size.width - 27.f, size.height - 36.f});
        if (m_dropMenu) m_dropMenu->setPosition({size.width - 92.f, size.height - 46.f});

        float inputWidth = std::max(80.f, size.width - x - rightTools);
        m_address->setContentWidth(inputWidth / m_address->getScale());
        m_address->setPosition({x + inputWidth / 2.f, size.height - 36.f});
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
        // Only update the field when the user is not editing it, so typing is
        // never overwritten by background URL changes during page loads.
        auto* inputNode = m_address->getInputNode();
        if ((!inputNode || !inputNode->m_selected) && m_address->getString() != tab->url) {
            m_address->setString(tab->url);
        }
        auto* sprite = static_cast<ButtonSprite*>(m_muteButton->getNormalImage());
        sprite->setString(tab->muted ? "Muted" : "Sound");
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
    auto world = touch->getLocation();
    auto local = convertToNodeSpace(world);
    if (local.x < 0.f || local.y < 0.f || local.x > getContentWidth() || local.y > getContentHeight()) return false;
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
    return false;
}

void BrowserWindow::ccTouchMoved(CCTouch* touch, CCEvent*) {
    auto delta = touch->getLocation() - m_touchStart;
    if (m_dragging) setPosition(m_startPosition + delta);
    if (m_resizing) {
        setContentSize(m_startSize + CCSize{delta.x, -delta.y});
        setPosition({m_startPosition.x, m_startPosition.y + m_startSize.height - getContentHeight()});
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
    BrowserHost::get().navigate(m_address->getString());
}
void BrowserWindow::onMute(CCObject*) { BrowserHost::get().toggleMute(); }

void BrowserWindow::onMenu(CCObject*) {
    if (m_dropMenu) m_dropMenu->setVisible(!m_dropMenu->isVisible());
}

void BrowserWindow::showListPopup(char const* title, std::string body) {
    if (m_dropMenu) m_dropMenu->setVisible(false);
    if (body.empty()) body = "Nothing here yet.";
    auto* popup = FLAlertLayer::create(title, body, "OK");
    popup->show();
    if (auto* parent = popup->getParent(); parent && parent != OverlayManager::get()) {
        popup->retain();
        popup->removeFromParentAndCleanup(false);
        OverlayManager::get()->addChild(popup, 20000);
        popup->release();
    }
}

void BrowserWindow::onDownloads(CCObject*) {
    std::string body;
    for (auto const& d : BrowserHost::get().downloads()) {
        std::string status = d.canceled ? "canceled" : d.complete ? "done" : std::to_string(d.percent) + "%";
        body += d.name + "  (" + status + ")\n";
    }
    showListPopup("Downloads", body);
}

void BrowserWindow::onHistory(CCObject*) {
    std::string body;
    auto const& hist = BrowserHost::get().history();
    // Most recent first, capped so the popup stays readable.
    size_t shown = std::min<size_t>(hist.size(), 30);
    for (size_t i = 0; i < shown; ++i) body += hist[hist.size() - 1 - i] + "\n";
    showListPopup("History", body);
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
