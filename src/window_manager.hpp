#pragma once

#include "kwin_compat.hpp"
#include "window.hpp"

#include <effect/effect.h>
#include <effect/effecthandler.h>
#include <effect/effectwindow.h>

#if KWIN_VERSION >= KWIN_VERSION_CODE(6, 5, 80)
#  include <core/output.h>
#  include <core/region.h>
#endif

#include <QList>
#include <QObject>
#include <QPointer>
#include <QRegularExpression>
#include <QString>
#include <QTimer>

#include <memory>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

namespace KWin {
    class BorderRadius;
}

namespace BBDXReal {
class BlurEffect;

class WindowManager : public QObject {
    Q_OBJECT

public:
    enum class WindowClassMatchMode {
        Whitelist,
        Blacklist,
    };

private:
    // pointer to the owning BlurEffect instance
    BBDXReal::BlurEffect *m_effect;

    // 合并同一轮事件；操作停止后补一次最终刷新，不保留周期定时器。
    QTimer m_refreshTimer;
    QTimer m_settleTimer;
    quint64 m_onDemandRefreshes{0};
    QString m_lastRefreshReason;
    void refreshOnDemandCaches();
    // 只比较普通应用的焦点和相对层级，辅助窗口插入/移除不算应用交互。
    QPointer<KWin::EffectWindow> m_lastInteractionWindow;
    QList<const KWin::EffectWindow *> m_interactionStack;
    quint64 m_ignoredAuxiliaryEvents{0};
    bool isInteractionWindow(const KWin::EffectWindow *w) const;
    void updateInteractionStack();

    // managed windows
    std::unordered_map<const KWin::EffectWindow *, std::unique_ptr<BBDXReal::Window>> m_windows{};

    // docks seperate for maximized calculation
    std::unordered_set<const KWin::EffectWindow *> m_docks{};

    // window classes
    QList<QString> m_windowClassesFixed{};
    QList<QRegularExpression> m_windowClassesRegex{};
    WindowClassMatchMode m_windowClassMatchMode{WindowClassMatchMode::Whitelist};

    // window/region types
    bool m_blurDecorations{false};
    bool m_blurDocks{false};
    bool m_blurMenus{false};

    // user configured border radius
    qreal m_userBorderRadius{0.0};

    // match helpers
    bool matchesWindowClassFixed(const KWin::EffectWindow *w) const;
    bool matchesWindowClassRegex(const KWin::EffectWindow *w) const;

    /**
     * Find a managed window, nullptr if not found
     */
    BBDXReal::Window* findWindow(const KWin::EffectWindow *w) const;

    /**
     * Collect BBDXReal Windows sorted by stackingOrder
     */
    std::vector<BBDXReal::Window *> windowsByStackingOrder() const;

public Q_SLOT:
    void slotWindowAdded(KWin::EffectWindow *w);
    void slotWindowDeleted(KWin::EffectWindow *w);

public:
    explicit WindowManager(BBDXReal::BlurEffect *effect);

    /**
     * reconfigure from BlurConfig
     */
    void reconfigure();

    void requestOnDemandRefresh(const QString &reason);
    void requestWindowRefresh(KWin::EffectWindow *w, const QString &reason);
    quint64 ignoredAuxiliaryEvents() const { return m_ignoredAuxiliaryEvents; }
    quint64 onDemandRefreshes() const { return m_onDemandRefreshes; }
    bool onDemandPending() const { return m_refreshTimer.isActive() || m_settleTimer.isActive(); }
    QString lastRefreshReason() const { return m_lastRefreshReason; }

    /**
     * Refresh maximized state of a window / of all windows
     */
    void refreshMaximizedState(BBDXReal::Window *w) const;
    void refreshMaximizedStateAll() const;

    /**
     * getters
     */
    bool blurDecorations() const { return m_blurDecorations; }
    bool blurDocks() const { return m_blurDocks; }
    bool blurMenus() const { return m_blurMenus; }
    qreal userBorderRadius() const { return m_userBorderRadius; }

    /**
     * Match an EffectWindow instance in the black/white list
     * XXX: should this also be moved to BBDXReal::Window?
     */
    bool shouldForceBlurWindowClass(const KWin::EffectWindow *w) const;

    /**
     * emits the wantsBlurRegionUpdate signal
     */
    void triggerBlurRegionUpdate(KWin::EffectWindow *w) const;

    /**
     * emits the windowInvalidatedBlurCache signal
     */
    void invalidateBlurCache(KWin::EffectWindow *w, uint flags, const char *reason) const;

    /**
     * Set the "window is transformed" flag on a window
     * (scaled, translated or PAINT_WINDOW_TRANSFORMED set)
     */
    void setWindowIsTransformed(const KWin::EffectWindow *w, bool toggle) const;

    /**
     * Check if this window should currently be blurred
     * even when PAINT_WINDOW_TRANSFORMED is set
     */
    bool windowShouldBlurWhileTransformed(const KWin::EffectWindow *w) const;

    /**
     * Check if this window is blurred in any way (requested or forced)
     */
    bool windowIsBlurred(const KWin::EffectWindow *w) const;

    /**
     * Get the final blur region for a window, set in content/frame.
     *
     * Forwarded to BBDXReal::Window::getFinalBlurRegion() if w is managed
     * else does nothing.
     */
    void getFinalBlurRegion(const KWin::EffectWindow *w, std::optional<KWin::RegionF> &content, std::optional<KWin::RegionF> &frame) const;

    /**
     * Get effective border radius for requested window,
     * or empty if unmanaged
     */
    KWin::BorderRadius getEffectiveBorderRadius(const KWin::EffectWindow *w) const;

    /**
     * Get effective blur opacity for requested window
     */
    qreal getEffectiveBlurOpacity(const KWin::EffectWindow *w, KWin::WindowPaintData &data) const;

    /**
     * Check if the provided window's blur region is fully covered by
     * the frame geometry of other windows
     */
    bool windowIsBlurFullyCovered(KWin::EffectWindow *w) const;

    /**
     * Add a full repaint to all blurred windows
     */
    void repaintAllBlurredWindows() const;

    /**
     * Flush all caches of a window
     */
    void flushWindowCaches(BBDXReal::Window *window) const;
    void flushWindowCachesFor(BBDXReal::Window *window, std::chrono::milliseconds duration) const;

    /**
     * Flush all caches of all windows
     */
    void flushAllWindowCaches() const;
    void flushAllWindowCachesFor(std::chrono::milliseconds duration) const;
};

} // namespace KWin
