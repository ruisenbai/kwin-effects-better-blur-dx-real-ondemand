#include "window_manager.hpp"

#include "blur.h"
#include "blurconfig.h"
#include "kwin_compat.hpp"
#include "utils.h"
#include "window.hpp"

#include <core/outputlayer.h>
#include <core/pixelgrid.h>
#include <core/renderviewport.h>
#include <effect/effecthandler.h>
#include <effect/effectwindow.h>
#include <qloggingcategory.h>
#include <scene/borderradius.h>
#include <scene/windowitem.h>
#include <window.h>

#if KWIN_VERSION >= KWIN_VERSION_CODE(6, 5, 80)
#  include <core/output.h>
#  include <core/region.h>
#endif

#include <QList>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QString>

#include <memory>
#include <utility>
#include <vector>

Q_LOGGING_CATEGORY(WINDOW_MANAGER, "kwin_effect_better_blur_dx.window_manager", QtInfoMsg)

BBDXReal::WindowManager::WindowManager(BBDXReal::BlurEffect *effect) {
    m_effect = effect;

    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(0);
    m_settleTimer.setSingleShot(true);
    m_settleTimer.setInterval(120);
    connect(&m_refreshTimer, &QTimer::timeout, this, &WindowManager::refreshOnDemandCaches);
    connect(&m_settleTimer, &QTimer::timeout, this, &WindowManager::refreshOnDemandCaches);
    connect(KWin::effects, &KWin::EffectsHandler::windowActivated, this, [this](KWin::EffectWindow *w) {
        if (!isInteractionWindow(w)) {
            ++m_ignoredAuxiliaryEvents;
            return;
        }
        // 菜单或输入法临时取得焦点后返回同一个应用，不刷新整批缓存。
        if (m_lastInteractionWindow == w) {
            return;
        }
        m_lastInteractionWindow = w;
        requestWindowRefresh(w, QStringLiteral("window activated"));
    });
    connect(KWin::effects, &KWin::EffectsHandler::stackingOrderChanged, this, [this]() {
        updateInteractionStack();
    });
    connect(KWin::effects, &KWin::EffectsHandler::activeFullScreenEffectChanged, this, [this]() {
        requestOnDemandRefresh(QStringLiteral("fullscreen effect changed"));
    });

    // add existing windows
    for (const auto &window : KWin::effects->stackingOrder()) {
        slotWindowAdded(window);
    }
    updateInteractionStack();
    if (isInteractionWindow(KWin::effects->activeWindow())) {
        m_lastInteractionWindow = KWin::effects->activeWindow();
    }

    connect(KWin::effects, &KWin::EffectsHandler::windowAdded, this, &WindowManager::slotWindowAdded);
    connect(KWin::effects, &KWin::EffectsHandler::windowDeleted, this, &WindowManager::slotWindowDeleted);
}

void BBDXReal::WindowManager::slotWindowAdded(KWin::EffectWindow *w) {
    auto window = std::make_unique<BBDXReal::Window>(this, w);

    m_windows.insert_or_assign(w, std::move(window));
    requestWindowRefresh(w, QStringLiteral("window added"));

    if (w->isDock()) {
        m_docks.insert(w);
        refreshMaximizedStateAll();
    }

    qCDebug(WINDOW_MANAGER) << BBDXReal::LOG_PREFIX << "Window added:" << *(m_windows[w]);
}

void BBDXReal::WindowManager::slotWindowDeleted(KWin::EffectWindow *w) {
    requestWindowRefresh(w, QStringLiteral("window removed"));
    if (const auto it = m_windows.find(w); it != m_windows.end()) {
        qCDebug(WINDOW_MANAGER) << BBDXReal::LOG_PREFIX << "Window removed:" << *(it->second);
        m_windows.erase(it);
    }

    if (const auto it = m_docks.find(w); it != m_docks.end()) {
        m_docks.erase(it);
        refreshMaximizedStateAll();
    }
}

BBDXReal::Window* BBDXReal::WindowManager::findWindow(const KWin::EffectWindow *w) const {
    if (const auto it = m_windows.find(w); it != m_windows.end()) {
        return it->second.get();
    }
    return nullptr;
}

std::vector<BBDXReal::Window *> BBDXReal::WindowManager::windowsByStackingOrder() const {
    std::vector<BBDXReal::Window *> windows{};

    for (const auto &[kWindow, bbdxWindow] : m_windows) {

        // insert sorted by stackingOrder
        for (auto it = windows.begin(); ; it++) {
            if (it == windows.end()) {
                windows.insert(it, bbdxWindow.get());
                break;
            }

            if ((*it)->effectwindow()->window()->stackingOrder() > kWindow->window()->stackingOrder()) {
                windows.insert(it, bbdxWindow.get());
                break;
            }
        }
    }

    return windows;
}

void BBDXReal::WindowManager::reconfigure() {
    auto config = BBDXReal::BlurConfig::self();

    if (!config) {
        qCWarning(WINDOW_MANAGER) << BBDXReal::LOG_PREFIX
                                  << "WindowManager::reconfigure() called before BlurConfig::read()";
        return;
    }

    QList<QString> windowClassesFixed{};
    QList<QRegularExpression> windowClassesRegex{};

    for (const auto &line : config->windowClasses().split("\n", Qt::SkipEmptyParts)) {
        if (line.length() >= 2 && line.startsWith(QChar('/')) && line.endsWith(QChar('/'))) {
            // regex pattern

            QString pattern = line.sliced(1, line.length() - 2);
            QRegularExpression regex{pattern};

            if (!regex.isValid()) {
                qCWarning(WINDOW_MANAGER) << BBDXReal::LOG_PREFIX
                                          << "Ignoring malformed regex pattern:" << pattern
                                          << "-" << regex.errorString();
                continue;
            }

            regex.optimize();
            windowClassesRegex.append(std::move(regex));

        } else {
            // fixed string
            // $blank -> empty window class

            if (line == QStringLiteral("$blank")) {
                windowClassesFixed.append(QStringLiteral(""));
            } else {
                windowClassesFixed.append(line);
            }
        }
    }

    m_windowClassesFixed = std::move(windowClassesFixed);
    m_windowClassesRegex = std::move(windowClassesRegex);

    if (config->blurMatching()) {
        m_windowClassMatchMode = WindowClassMatchMode::Whitelist;
    } else {
        m_windowClassMatchMode = WindowClassMatchMode::Blacklist;
    }

    m_blurDecorations = config->blurDecorations();
    m_blurDocks = config->blurDocks();
    m_blurMenus = config->blurMenus();

    m_userBorderRadius = config->cornerRadius();

    for (const auto &[_, window] : m_windows) {
        window->reconfigure();
    }
}

void BBDXReal::WindowManager::refreshMaximizedState(BBDXReal::Window *window) const {
    /**
     * TODO: convert to RegionF
     */

    const KWin::EffectWindow *w = window->effectwindow();

    const KWin::LogicalOutput* screen = w->screen();

    KWin::Region effectiveScreenRegion = KWin::Region(screen->geometry());

    for (const auto &dock : m_docks) {
        if (dock->screen() != screen)  {
            continue;
        }

        // we need to "expand" the docks to their entire screen edge
        // or the boundingRect is wrong
        KWin::Rect dock_rect{dock->frameGeometry().toRect()};

        if (dock_rect.width() > dock_rect.height()) {
            // horizontal
            dock_rect.setX(dock->screen()->geometry().x());
            dock_rect.setWidth(dock->screen()->geometry().width());
        } else {
            // vertical
            dock_rect.setY(dock->screen()->geometry().y());
            dock_rect.setHeight(dock->screen()->geometry().height());
        }

        effectiveScreenRegion -= KWin::Region(dock_rect);
    }

    const KWin::Rect effectiveScreenRect{effectiveScreenRegion.boundingRect()};
    const KWin::Rect windowRect{KWin::Rect(w->frameGeometry().toRect())};

    bool maximizedHorizontal{
        windowRect.left() <= effectiveScreenRect.left()
        && windowRect.right() >= effectiveScreenRect.right()
    };

    bool maximizedVertical{
        windowRect.top() <= effectiveScreenRect.top()
        && windowRect.bottom() >= effectiveScreenRect.bottom()
    };

    if (maximizedHorizontal && maximizedVertical) {
        window->setMaximizedState(Window::MaximizedState::Complete);
    } else if (maximizedHorizontal && !maximizedVertical) {
        window->setMaximizedState(Window::MaximizedState::Horizontal);
    } else if (!maximizedHorizontal && maximizedVertical) {
        window->setMaximizedState(Window::MaximizedState::Vertical);
    } else {
        window->setMaximizedState(Window::MaximizedState::Restored);
    }
}

void BBDXReal::WindowManager::refreshMaximizedStateAll() const {
    for (const auto &[w, window] : m_windows) {
        refreshMaximizedState(window.get());
    }
}

bool BBDXReal::WindowManager::matchesWindowClassFixed(const KWin::EffectWindow *w) const {
    if (m_windowClassesFixed.contains(w->window()->resourceClass()))
        return true;

    if (m_windowClassesFixed.contains(w->window()->resourceName()))
        return true;

    return false;
}

bool BBDXReal::WindowManager::matchesWindowClassRegex(const KWin::EffectWindow *w) const {
    for (const auto &regex : m_windowClassesRegex) {
        if (auto m = regex.match(w->window()->resourceClass()); m.hasMatch())
            return true;

        if (auto m = regex.match(w->window()->resourceName()); m.hasMatch())
            return true;
    }

    return false;
}

bool BBDXReal::WindowManager::shouldForceBlurWindowClass(const KWin::EffectWindow *w) const {
    if (matchesWindowClassFixed(w) || matchesWindowClassRegex(w)) {
        switch (m_windowClassMatchMode) {
            case WindowManager::WindowClassMatchMode::Whitelist:
                return true;
            case WindowManager::WindowClassMatchMode::Blacklist:
                return false;
            [[unlikely]] default:
                return false;
        }
    } else {
        switch (m_windowClassMatchMode) {
            case WindowManager::WindowClassMatchMode::Whitelist:
                return false;
            case WindowManager::WindowClassMatchMode::Blacklist:
                return true;
            [[unlikely]] default:
                return false;
        }
    }
}

void BBDXReal::WindowManager::triggerBlurRegionUpdate(KWin::EffectWindow *w) const {
    m_effect->updateBlurRegion(w);
}

void BBDXReal::WindowManager::invalidateBlurCache(KWin::EffectWindow *w, uint flags, const char *reason) const {
    if (auto it = m_effect->m_windows.find(w); it != m_effect->m_windows.end()) {
        BBDXReal::BlurEffectData &blurInfo = it->second;
        for (auto &[_, renderInfo] : blurInfo.render) {
            if (renderInfo.cache) {
                renderInfo.cache->invalidate(flags, reason);
            }
        }
    }
}

void BBDXReal::WindowManager::setWindowIsTransformed(const KWin::EffectWindow *w, bool toggle) const {
    const auto window = findWindow(w);

    if (!window)
        return;

    window->setIsTransformed(toggle);
}

bool BBDXReal::WindowManager::windowShouldBlurWhileTransformed(const KWin::EffectWindow *w) const {
    const auto window = findWindow(w);

    // assume false for unmanaged windows
    if (!window)
        return false;

    return window->shouldBlurWhileTransformed();
}

bool BBDXReal::WindowManager::windowIsBlurred(const KWin::EffectWindow *w) const {
    const auto window = findWindow(w);

    // assume false for unmanaged windows
    if (!window)
        return false;

    return window->isBlurred();
}

void BBDXReal::WindowManager::getFinalBlurRegion(const KWin::EffectWindow *w, std::optional<KWin::RegionF> &content, std::optional<KWin::RegionF> &frame) const {
    const auto window = findWindow(w);
    if (!window)
        return;

    window->getFinalBlurRegion(content, frame);
}

KWin::BorderRadius BBDXReal::WindowManager::getEffectiveBorderRadius(const KWin::EffectWindow *w) const {
    const auto window = findWindow(w);

    // unmanaged windows can never be force blurred
    if (!window)
        return KWin::BorderRadius();

    return window->getEffectiveBorderRadius();
}

qreal BBDXReal::WindowManager::getEffectiveBlurOpacity(const KWin::EffectWindow *w, KWin::WindowPaintData &data) const {
    const auto window = findWindow(w);

    // for unmanaged windows just use the paint data
    if (!window)
        return data.opacity();

    return window->getEffectiveBlurOpacity(data);
}

bool BBDXReal::WindowManager::windowIsBlurFullyCovered(KWin::EffectWindow *w) const {
    const auto window = findWindow(w);

    // for unmanaged windows assume false so they
    // don't get throttled accidentally
    if (!window)
        return false;

    return window->isBlurFullyCovered();
}

void BBDXReal::WindowManager::repaintAllBlurredWindows() const {
    for (const auto &[kWindow, bbdxWindow] : m_windows) {
        if (!bbdxWindow->isBlurred()) {
            continue;
        }

        const_cast<KWin::EffectWindow *>(kWindow)->addRepaintFull();
    }
}

void BBDXReal::WindowManager::flushWindowCaches(BBDXReal::Window *window) const {
    auto it = m_effect->m_windows.find(window->effectwindow());
    if (it == m_effect->m_windows.end()) {
        return;
    }

    auto &effectData = it->second;
    for (auto &[view, renderData] : effectData.render) {
        if (auto cacheEntry = renderData.cache.get()) {
            cacheEntry->flush();
        }
    }
}

void BBDXReal::WindowManager::flushWindowCachesFor(BBDXReal::Window *window, std::chrono::milliseconds duration) const {
    auto it = m_effect->m_windows.find(window->effectwindow());
    if (it == m_effect->m_windows.end()) {
        return;
    }

    auto &effectData = it->second;
    for (auto &[view, renderData] : effectData.render) {
        if (auto cacheEntry = renderData.cache.get()) {
            cacheEntry->flushFor(duration);
        }
    }
}

void BBDXReal::WindowManager::flushAllWindowCaches() const {
    for (const auto &[kWindow, bbdxWindow] : m_windows) {
        flushWindowCaches(bbdxWindow.get());
    }
}

void BBDXReal::WindowManager::flushAllWindowCachesFor(std::chrono::milliseconds duration) const {
    for (const auto &[kWindow, bbdxWindow] : m_windows) {
        flushWindowCachesFor(bbdxWindow.get(), duration);
    }
}

bool BBDXReal::WindowManager::isInteractionWindow(const KWin::EffectWindow *w) const {
    if (!w || !w->isManaged() || w->internalWindow() || w->isPopupWindow() || w->isInputMethod()) {
        return false;
    }
    if (!w->isNormalWindow() && !w->isDialog() && !w->isUtility()) {
        return false;
    }
    // 某些系统浮层被上报成普通窗口，额外按应用标识过滤；不改变磨砂排除名单。
    static const QRegularExpression auxiliaryClass(
        QStringLiteral("^(?:plasmashell|org[.]kde[.]plasmashell|kwin_wayland|krunner|org[.]kde[.]krunner|fcitx5?(?:[-.].*)?|org[.]fcitx(?:[.].*)?|clawd-on-desk|xwaylandvideobridge|org[.]kde[.]xwaylandvideobridge)$"),
        QRegularExpression::CaseInsensitiveOption);
    return !auxiliaryClass.match(w->window()->resourceClass()).hasMatch()
        && !auxiliaryClass.match(w->window()->resourceName()).hasMatch();
}

void BBDXReal::WindowManager::updateInteractionStack() {
    QList<const KWin::EffectWindow *> current;
    for (auto *w : KWin::effects->stackingOrder()) {
        if (isInteractionWindow(w) && !w->isDeleted() && !w->isMinimized()
            && w->isOnCurrentDesktop() && w->isOnCurrentActivity()) {
            current.append(w);
        }
    }
    if (current == m_interactionStack) {
        return;
    }
    m_interactionStack = std::move(current);
    requestOnDemandRefresh(QStringLiteral("application stacking order changed"));
}

void BBDXReal::WindowManager::requestWindowRefresh(KWin::EffectWindow *w, const QString &reason) {
    if (!isInteractionWindow(w)) {
        ++m_ignoredAuxiliaryEvents;
        return;
    }
    // 诊断仅保留应用标识，不记录标题或输入内容。
    requestOnDemandRefresh(reason + QStringLiteral(" [") + w->window()->resourceClass() + QLatin1Char(']'));
}

void BBDXReal::WindowManager::requestOnDemandRefresh(const QString &reason) {
    if (!BlurConfig::blurOnDemand()) {
        return;
    }
    m_lastRefreshReason = reason;
    if (!m_refreshTimer.isActive()) {
        m_refreshTimer.start();
    }
    m_settleTimer.start();
}

void BBDXReal::WindowManager::refreshOnDemandCaches() {
    if (!BlurConfig::blurOnDemand()) {
        return;
    }
    ++m_onDemandRefreshes;
    // 先丢弃旧覆盖范围再完整重画，避免新旧背景混在同一份缓存中。
    for (const auto &[window, data] : m_windows) {
        invalidateBlurCache(const_cast<KWin::EffectWindow *>(window),
                            static_cast<uint>(BlurCacheInvalidationFlag::REGION),
                            "on-demand window interaction");
    }
    repaintAllBlurredWindows();
}
