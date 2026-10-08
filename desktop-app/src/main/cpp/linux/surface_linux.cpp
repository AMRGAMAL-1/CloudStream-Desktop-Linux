#include "../include/player_bridge_common.h"

#ifndef _WIN32

#include <gtk/gtk.h>
#include <gtk/gtkx.h>
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>
#include <GL/gl.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>
#include <webkit2/webkit2.h>
#include <jsc/jsc.h>

#include <algorithm>
#include <cstring>
#include <dlfcn.h>
#include <codecvt>
#include <clocale>
#include <exception>
#include <future>
#include <filesystem>
#include <locale>
#include <memory>
#include <sstream>
#include <string_view>

namespace {

GtkWidget* g_plug = nullptr;
GtkWidget* g_playerOverlay = nullptr;
GtkGLArea* g_glArea = nullptr;
GtkWidget* g_vlcArea = nullptr;
WebKitWebView* g_webView = nullptr;
// Flicker-free overlay (root fix): the WebKit controls page lives in a
// separate, composite-redirected (offscreen) GTK toplevel. Its snapshots are
// composited as a GL texture inside the mpv render pass (renderMpvFrame), so
// no transparent X window ever stacks over the video on X11/XWayland.
GtkWidget* g_controlsWindow = nullptr;
Window g_controlsXid = 0;
bool g_controlsVisible = true;   // gate: web UI controls currently shown
guint g_compositeTimer = 0;      // snapshot tick (~30fps while active)
bool g_snapInFlight = false;
guint g_snapGen = 0;
guint g_snapWaitTicks = 0;
GCancellable* g_snapCancel = nullptr;
cairo_surface_t* g_snapSurf = nullptr;
GLuint g_overlayTexture = 0;
bool g_overlayTextureValid = false;
int g_overlayTexW = 0;
int g_overlayTexH = 0;
GMainContext* g_mainContext = nullptr;
GMainLoop* g_mainLoop = nullptr;
guint g_mpvSyncSource = 0;
guint g_vlcSyncSource = 0;
guint g_overlayShowSource = 0;
std::thread g_uiThread;
std::mutex g_lifecycleMutex;
// Serializes the native surface transition itself. A player can be disposed
// on a daemon thread while the next player is being created on the AWT thread;
// allowing those two transitions to overlap reuses GTK objects while they are
// being destroyed.
std::mutex g_lifecycleTransitionMutex;
std::mutex g_destroyMutex;
std::condition_variable g_initCv;
bool g_initComplete = false;
bool g_x11EmbeddingAvailable = false;
bool g_overlayVisible = false;
// MPV can deliver a render update while the GTK loop is being stopped. Once
// teardown starts, queued callbacks must become no-ops before widget pointers
// are invalidated or the GTK tree is destroyed.
std::atomic<bool> g_teardownRequested{false};
Window g_renderWindowId = 0;
Window g_containerWindowId = 0;
Window g_hostWindowId = 0;
// The AWT Canvas is destroyed when the player leaves Compose. Keep the GTK
// plug attached to this private X11 parking window between sessions so the
// old Canvas cannot destroy the reusable GTK/WebKit tree.
Window g_parkingWindowId = 0;
int g_lastOverlayWidth = 0;
int g_lastOverlayHeight = 0;
int g_lastOverlayX = 0;
int g_lastOverlayY = 0;
bool g_lastOverlayExternal = false;
mpv_render_context* g_mpvRenderContext = nullptr;
mpv_handle* g_renderMpvHandle = nullptr;
bool g_mpvFirstFrameLogged = false;
std::mutex g_renderMutex;
std::mutex g_vlcMutex;

// VLC is loaded dynamically on Linux so the bridge remains buildable on
// systems that have MPV/WebKitGTK but do not have libVLC development headers.
// The runtime library is still required only when the user selects VLC.
struct libvlc_instance_t;
struct libvlc_media_t;
struct libvlc_media_player_t;

using libvlc_new_fn = libvlc_instance_t* (*)(int, const char* const*);
using libvlc_release_fn = void (*)(libvlc_instance_t*);
using libvlc_media_new_location_fn = libvlc_media_t* (*)(libvlc_instance_t*, const char*);
using libvlc_media_add_option_fn = void (*)(libvlc_media_t*, const char*);
using libvlc_media_release_fn = void (*)(libvlc_media_t*);
using libvlc_media_player_new_from_media_fn = libvlc_media_player_t* (*)(libvlc_media_t*);
using libvlc_media_player_release_fn = void (*)(libvlc_media_player_t*);
using libvlc_media_player_play_fn = int (*)(libvlc_media_player_t*);
using libvlc_media_player_stop_fn = void (*)(libvlc_media_player_t*);
using libvlc_media_player_pause_fn = void (*)(libvlc_media_player_t*);
using libvlc_media_player_get_state_fn = int (*)(libvlc_media_player_t*);
using libvlc_media_player_is_playing_fn = int (*)(libvlc_media_player_t*);
using libvlc_media_player_get_time_fn = long long (*)(libvlc_media_player_t*);
using libvlc_media_player_get_length_fn = long long (*)(libvlc_media_player_t*);
using libvlc_media_player_set_time_fn = void (*)(libvlc_media_player_t*, long long);
using libvlc_media_player_set_rate_fn = int (*)(libvlc_media_player_t*, float);
using libvlc_audio_set_volume_fn = int (*)(libvlc_media_player_t*, int);
using libvlc_audio_set_mute_fn = void (*)(libvlc_media_player_t*, int);
using libvlc_video_lock_cb = void* (*)(void*, void**);
using libvlc_video_unlock_cb = void (*)(void*, void*, const void* const*);
using libvlc_video_display_cb = void (*)(void*, void*);
using libvlc_video_format_cb = unsigned (*)(void**, char*, unsigned*, unsigned*, unsigned*, unsigned*);
using libvlc_video_cleanup_cb = void (*)(void*);
using libvlc_video_set_callbacks_fn = void (*)(
    libvlc_media_player_t*,
    libvlc_video_lock_cb,
    libvlc_video_unlock_cb,
    libvlc_video_display_cb,
    void*
);
using libvlc_video_set_format_callbacks_fn = void (*)(
    libvlc_media_player_t*,
    libvlc_video_format_cb,
    libvlc_video_cleanup_cb
);
using libvlc_errmsg_fn = const char* (*)();
using libvlc_clearerr_fn = void (*)();

struct VlcFrameBuffer {
    std::mutex mutex;
    std::condition_variable callbacksCv;
    std::vector<unsigned char> pixels;
    unsigned width = 0;
    unsigned height = 0;
    unsigned stride = 0;
    bool active = false;
    bool configured = false;
    unsigned callbacksInFlight = 0;
    unsigned long long frameCount = 0;
};

void* g_vlcLibrary = nullptr;
libvlc_instance_t* g_vlcInstance = nullptr;
libvlc_media_player_t* g_vlcPlayer = nullptr;
libvlc_new_fn g_vlcNew = nullptr;
libvlc_release_fn g_vlcRelease = nullptr;
libvlc_media_new_location_fn g_vlcMediaNewLocation = nullptr;
libvlc_media_add_option_fn g_vlcMediaAddOption = nullptr;
libvlc_media_release_fn g_vlcMediaRelease = nullptr;
libvlc_media_player_new_from_media_fn g_vlcPlayerNewFromMedia = nullptr;
libvlc_media_player_release_fn g_vlcPlayerRelease = nullptr;
libvlc_media_player_play_fn g_vlcPlayerPlay = nullptr;
libvlc_media_player_stop_fn g_vlcPlayerStop = nullptr;
libvlc_media_player_pause_fn g_vlcPlayerPause = nullptr;
libvlc_media_player_get_state_fn g_vlcPlayerGetState = nullptr;
libvlc_media_player_is_playing_fn g_vlcPlayerIsPlaying = nullptr;
libvlc_media_player_get_time_fn g_vlcPlayerGetTime = nullptr;
libvlc_media_player_get_length_fn g_vlcPlayerGetLength = nullptr;
libvlc_media_player_set_time_fn g_vlcPlayerSetTime = nullptr;
libvlc_media_player_set_rate_fn g_vlcPlayerSetRate = nullptr;
libvlc_audio_set_volume_fn g_vlcAudioSetVolume = nullptr;
libvlc_audio_set_mute_fn g_vlcAudioSetMute = nullptr;
libvlc_video_set_callbacks_fn g_vlcVideoSetCallbacks = nullptr;
libvlc_video_set_format_callbacks_fn g_vlcVideoSetFormatCallbacks = nullptr;
libvlc_errmsg_fn g_vlcErrmsg = nullptr;
libvlc_clearerr_fn g_vlcClearerr = nullptr;
std::atomic<VlcFrameBuffer*> g_vlcFrame{nullptr};
// VLC may deliver one final callback while libvlc_media_player_release() is
// unwinding its decoder threads. Keep the small session object alive after
// teardown so a late callback can observe active=false safely. The pixel
// storage is released before the object is retired.
std::vector<std::unique_ptr<VlcFrameBuffer>> g_vlcRetiredFrames;

using mpv_render_context_create_fn = int (*)(mpv_render_context**, mpv_handle*, mpv_render_param*);
using mpv_render_context_set_update_callback_fn = void (*)(mpv_render_context*, mpv_render_update_fn, void*);
using mpv_render_context_render_fn = int (*)(mpv_render_context*, mpv_render_param*);
using mpv_render_context_report_swap_fn = void (*)(mpv_render_context*);
using mpv_render_context_free_fn = void (*)(mpv_render_context*);

mpv_render_context_create_fn g_renderContextCreate = nullptr;
mpv_render_context_set_update_callback_fn g_renderContextSetUpdateCallback = nullptr;
mpv_render_context_render_fn g_renderContextRender = nullptr;
mpv_render_context_report_swap_fn g_renderContextReportSwap = nullptr;
mpv_render_context_free_fn g_renderContextFree = nullptr;

constexpr const char* kWebViewShim = R"JS(
(function () {
    const listeners = [];
    const webview = {
        postMessage: function (message) {
            try {
                window.webkit.messageHandlers.cloudstream.postMessage(message);
            } catch (error) {
                console.error('[CloudStream Linux bridge] postMessage failed', error);
            }
        },
        addEventListener: function (type, listener) {
            if (type === 'message' && typeof listener === 'function') listeners.push(listener);
        },
        removeEventListener: function (type, listener) {
            if (type !== 'message') return;
            const index = listeners.indexOf(listener);
            if (index >= 0) listeners.splice(index, 1);
        }
    };
    webview.__dispatch = function (data) {
        const event = { data: data };
        listeners.slice().forEach(function (listener) {
            try { listener(event); } catch (error) { console.error(error); }
        });
    };
    window.chrome = window.chrome || {};
    window.chrome.webview = webview;
    window.__cloudstreamDispatch = function (data) { webview.__dispatch(data); };
})();
)JS";

std::string toUtf8(JNIEnv* env, jstring value) {
    if (!value) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (!chars) return {};
    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}

std::pair<std::string, std::string> webkitStorageDirectories() {
    const auto dataRoot = std::filesystem::path(g_get_user_data_dir()) /
        "CloudStreamDesktop" / "webkit";
    const auto cacheRoot = std::filesystem::path(g_get_user_cache_dir()) /
        "CloudStreamDesktop" / "webkit";
    g_mkdir_with_parents(dataRoot.c_str(), 0700);
    g_mkdir_with_parents(cacheRoot.c_str(), 0700);
    return {dataRoot.string(), cacheRoot.string()};
}

void invokeOnGtkThread(std::function<void()> task) {
    if (!task) return;

    GMainContext* context = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        context = g_mainContext;
        if (context) g_main_context_ref(context);
    }

    if (!context) {
        // Every task submitted here is a GTK task. Running it inline after
        // the GTK loop has been torn down would call GTK from the AWT/MPV
        // thread and can turn a normal late callback into a native crash.
        return;
    }

    auto* heapTask = new std::function<void()>(std::move(task));
    g_main_context_invoke_full(
        context,
        G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
            auto* task = static_cast<std::function<void()>*>(data);
            if (task && *task) (*task)();
            delete task;
            return G_SOURCE_REMOVE;
        },
        heapTask,
        nullptr
    );
    g_main_context_unref(context);
}

void runJavascript(const std::string& script) {
    if (script.empty()) return;
    std::lock_guard<std::mutex> lifecycleLock(g_lifecycleMutex);
    if (g_teardownRequested || !g_webView) return;
    webkit_web_view_evaluate_javascript(
        g_webView,
        script.c_str(),
        static_cast<gssize>(script.size()),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr
    );
}

// ---------------------------------------------------------------------------
// Flicker-free overlay (root fix): WebKit snapshot -> GL texture -> composite
// inside the mpv render pass. No transparent X window is involved, so there is
// nothing for X11/XWayland to alpha-blend (the source of the old flicker).
// ---------------------------------------------------------------------------

struct SnapCtx {
    void* player;   // unused placeholder; kept for future generation checks
    guint gen;
};

// Upload a newly captured controls snapshot to the overlay GL texture. Runs on
// the GTK thread (snapshot callback); the actual GL upload happens lazily in
// renderMpvFrame where the GtkGLArea context is current.
void onOverlaySnapshot(GObject* src, GAsyncResult* res, gpointer data) {
    auto* ctx = static_cast<SnapCtx*>(data);
    guint gen = ctx->gen;
    delete ctx;
    GError* err = nullptr;
    cairo_surface_t* surf =
        webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(src), res, &err);
    if (err) g_error_free(err);

    {
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        if (g_teardownRequested) {
            if (surf) cairo_surface_destroy(surf);
            return;
        }
        if (gen != g_snapGen) {
            // A watchdog reset invalidated this request; drop it.
            if (surf) cairo_surface_destroy(surf);
            return;
        }
        g_snapInFlight = false;
        g_snapWaitTicks = 0;
        if (!surf) return;
        if (cairo_image_surface_get_format(surf) != CAIRO_FORMAT_ARGB32 ||
            cairo_image_surface_get_width(surf) <= 0 ||
            cairo_image_surface_get_height(surf) <= 0) {
            cairo_surface_destroy(surf);
            return;
        }
        // Drop snapshots taken at a stale size (captured mid-resize): the
        // next tick requests a fresh one at the settled size.
        if (g_controlsXid != 0) {
            GdkWindow* cw = gtk_widget_get_window(g_controlsWindow);
            if (cw) {
                XWindowAttributes wa;
                if (XGetWindowAttributes(gdk_x11_display_get_xdisplay(gdk_display_get_default()),
                                         g_controlsXid, &wa) &&
                    (cairo_image_surface_get_width(surf) != wa.width ||
                     cairo_image_surface_get_height(surf) != wa.height)) {
                    cairo_surface_destroy(surf);
                    return;
                }
            }
        }
        cairo_surface_flush(surf);
        if (g_snapSurf) cairo_surface_destroy(g_snapSurf);
        g_snapSurf = surf;   // consumed by renderMpvFrame on the GL thread
        LOG_TO_FILE("[NativeBridge:Linux] controls snapshot captured "
            << cairo_image_surface_get_width(surf) << "x"
            << cairo_image_surface_get_height(surf));
    }
}

// ~30fps tick while the controls are visible: request a fresh snapshot of the
// offscreen WebKit page. Runs on the GTK thread via g_timeout_add.
gboolean compositeTick(gpointer) {
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    if (g_teardownRequested || !g_webView) return G_SOURCE_REMOVE;

    // Keep the offscreen controls window matched to the REAL video host size,
    // even when no resize event arrives after the initial attach. The frame
    // clock of a composite-redirected window is stalled (it is never
    // presented), so gtk_window_resize alone never lands: force the X
    // geometry and the widget allocation synchronously, like the plug itself.
    if (g_controlsWindow && g_controlsXid != 0 && g_hostWindowId != 0) {
        GdkWindow* cwGdk = gtk_widget_get_window(g_controlsWindow);
        Display* dpy = gdk_x11_display_get_xdisplay(gdk_display_get_default());
        XWindowAttributes hostWa;
        if (cwGdk && dpy && XGetWindowAttributes(dpy, g_hostWindowId, &hostWa) != 0 &&
            hostWa.width > 0 && hostWa.height > 0) {
            int scale = gdk_window_get_scale_factor(cwGdk);
            if (scale < 1) scale = 1;
            const int logicalW = hostWa.width / scale;
            const int logicalH = hostWa.height / scale;
            XWindowAttributes cwWa;
            const bool mismatch =
                XGetWindowAttributes(dpy, g_controlsXid, &cwWa) == 0 ||
                cwWa.width != logicalW || cwWa.height != logicalH;
            if (mismatch) {
                LOG_TO_FILE("[NativeBridge:Linux] controls mismatch: X="
                    << cwWa.width << "x" << cwWa.height
                    << " logical=" << logicalW << "x" << logicalH
                    << " scale=" << scale);
                // Set the X geometry directly (GDK's own resize can be
                // reverted by the pending GTK layout) and request the layout
                // phase so WebKit re-lays-out to the video size.
                XResizeWindow(dpy, g_controlsXid,
                              static_cast<unsigned int>(logicalW),
                              static_cast<unsigned int>(logicalH));
                XFlush(dpy);
                gdk_window_resize(cwGdk, logicalW, logicalH);
                gtk_window_resize(GTK_WINDOW(g_controlsWindow), logicalW, logicalH);
                GtkAllocation alloc = {0, 0, logicalW, logicalH};
                gtk_widget_size_allocate(g_controlsWindow, &alloc);
                if (g_webView) gtk_widget_size_allocate(GTK_WIDGET(g_webView), &alloc);
                LOG_TO_FILE("[NativeBridge:Linux] controls resize to "
                    << logicalW << "x" << logicalH);
            }
        }
    }

    // The frame clock of a composite-redirected (never-presented) window is
    // stalled: gtk_window_resize and CSS layout never apply on their own.
    // Force the UPDATE+LAYOUT phases every tick so the page actually
    // re-lays-out to the video size (and its animations keep running).
    if (g_controlsWindow) {
        GdkWindow* fcWin = gtk_widget_get_window(g_controlsWindow);
        if (fcWin) {
            GdkFrameClock* fc = gdk_window_get_frame_clock(fcWin);
            if (fc) {
                gdk_frame_clock_request_phase(
                    fc, static_cast<GdkFrameClockPhase>(
                            GDK_FRAME_CLOCK_PHASE_UPDATE |
                            GDK_FRAME_CLOCK_PHASE_LAYOUT));
            }
        }
    }

    if (!g_controlsVisible) {
        if (g_snapSurf) {
            cairo_surface_destroy(g_snapSurf);
            g_snapSurf = nullptr;
        }
        return G_SOURCE_CONTINUE;
    }

    if (!g_snapInFlight) {
        g_snapInFlight = true;
        g_snapWaitTicks = 0;
        if (!g_snapCancel) g_snapCancel = g_cancellable_new();
        webkit_web_view_get_snapshot(
            g_webView, WEBKIT_SNAPSHOT_REGION_VISIBLE,
            WEBKIT_SNAPSHOT_OPTIONS_TRANSPARENT_BACKGROUND,
            g_snapCancel, onOverlaySnapshot, new SnapCtx{nullptr, g_snapGen});
    } else if (++g_snapWaitTicks > 30) {
        // Watchdog: a hung web process never calls back. Cancel, bump the
        // generation so a late callback is dropped, and retry after backoff.
        g_snapGen++;
        if (g_snapCancel) {
            g_cancellable_cancel(g_snapCancel);
            g_object_unref(g_snapCancel);
            g_snapCancel = nullptr;
        }
        g_snapInFlight = false;
        g_snapWaitTicks = 0;
        LOG_TO_FILE("[NativeBridge:Linux] snapshot stuck; watchdog reset");
    }
    return G_SOURCE_CONTINUE;
}

void startCompositeTimer() {
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    if (g_compositeTimer == 0 && !g_teardownRequested) {
        g_compositeTimer = g_timeout_add(33, compositeTick, nullptr);
    }
}

void stopCompositeTimer() {
    if (g_compositeTimer != 0) {
        g_source_remove(g_compositeTimer);
        g_compositeTimer = 0;
    }
    if (g_snapCancel) {
        g_cancellable_cancel(g_snapCancel);
        g_object_unref(g_snapCancel);
        g_snapCancel = nullptr;
    }
    g_snapGen++;
    g_snapInFlight = false;
    g_snapWaitTicks = 0;
    if (g_snapSurf) {
        cairo_surface_destroy(g_snapSurf);
        g_snapSurf = nullptr;
    }
    g_overlayTextureValid = false;
}

// Draw the captured controls snapshot as a GL texture over the video. Called
// from renderMpvFrame with the GtkGLArea context current, AFTER mpv rendered
// the video frame. Premultiplied alpha (cairo ARGB32) => GL_ONE blend factor.
void drawOverlayTexture(int width, int height) {
    if (!g_snapSurf || width <= 0 || height <= 0) return;
    cairo_surface_t* surf = g_snapSurf;
    const int sw = cairo_image_surface_get_width(surf);
    const int sh = cairo_image_surface_get_height(surf);
    if (sw <= 0 || sh <= 0) return;

    if (!g_overlayTextureValid || g_overlayTexW != sw || g_overlayTexH != sh) {
        if (g_overlayTexture) glDeleteTextures(1, &g_overlayTexture);
        glGenTextures(1, &g_overlayTexture);
        g_overlayTextureValid = false;
        g_overlayTexW = sw;
        g_overlayTexH = sh;
    }
    glBindTexture(GL_TEXTURE_2D, g_overlayTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw, sh, 0, GL_BGRA,
                 GL_UNSIGNED_BYTE, cairo_image_surface_get_data(surf));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_overlayTextureValid = true;

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // premultiplied alpha
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_overlayTexture);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, width, 0, height, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glBegin(GL_QUADS);
    glColor4f(1, 1, 1, 1);
    glTexCoord2f(0, 1); glVertex2f(0, 0);
    glTexCoord2f(1, 1); glVertex2f(width, 0);
    glTexCoord2f(1, 0); glVertex2f(width, height);
    glTexCoord2f(0, 0); glVertex2f(0, height);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}

gboolean clearTransparentPlug(GtkWidget*, cairo_t* cairo, gpointer) {
    if (!cairo) return FALSE;
    // The plug is an ARGB X11 child over the MPV window. Clear its backing
    // surface before GTK/WebKit paints the next damaged region, otherwise
    // old semi-transparent controls remain blended into the next frame.
    cairo_save(cairo);
    cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cairo);
    cairo_restore(cairo);
    return FALSE;
}

bool reparentGtkPlugToHost(Window plugWindow, Window hostWindow, int width, int height) {
    if (plugWindow == 0 || hostWindow == 0) return false;

    auto* display = gdk_display_get_default();
    if (!display || !GDK_IS_X11_DISPLAY(display)) return false;
    auto* xDisplay = gdk_x11_display_get_xdisplay(display);

    XWindowAttributes hostAttributes{};
    if (XGetWindowAttributes(xDisplay, hostWindow, &hostAttributes) == 0) {
        LOG_TO_FILE("[NativeBridge:Linux] AWT Canvas X11 host is no longer valid");
        return false;
    }

    // AWT Canvas is an X11 child window, but it is not a GtkSocket. Passing
    // it to gtk_plug_new() asks GTK to perform an XEmbed handshake that AWT
    // does not complete and can leave the plug as an unmanaged top-level.
    // Reparent the one GTK surface explicitly instead.
    XSetWindowBorderWidth(xDisplay, plugWindow, 0);
    XReparentWindow(xDisplay, plugWindow, hostWindow, 0, 0);
    XMoveResizeWindow(
        xDisplay,
        plugWindow,
        0,
        0,
        static_cast<unsigned int>(std::max(width, 1)),
        static_cast<unsigned int>(std::max(height, 1))
    );
    XFlush(xDisplay);

    g_containerWindowId = plugWindow;
    LOG_TO_FILE("[NativeBridge:Linux] GTK plug explicitly reparented into AWT Canvas; host="
        << hostWindow << ", plug=" << plugWindow);
    return true;
}

void alignGtkOverlay(int width, int height) {
    if (!g_plug) return;

    auto* plugWindow = gtk_widget_get_window(g_plug);
    if (!plugWindow) return;

    int targetWidth = width;
    int targetHeight = height;
    if (auto* display = gdk_window_get_display(plugWindow);
        display && GDK_IS_X11_DISPLAY(display) && g_hostWindowId != 0) {
        auto* xDisplay = gdk_x11_display_get_xdisplay(display);
        XWindowAttributes hostAttributes{};
        if (XGetWindowAttributes(xDisplay, g_hostWindowId, &hostAttributes) != 0) {
            targetWidth = hostAttributes.width;
            targetHeight = hostAttributes.height;
        }
    }
    if (targetWidth <= 0 || targetHeight <= 0) return;
    const bool geometryChanged =
        targetWidth != g_lastOverlayWidth ||
        targetHeight != g_lastOverlayHeight;
    if (!geometryChanged) return;

    gtk_widget_set_size_request(g_plug, targetWidth, targetHeight);
    gtk_window_resize(GTK_WINDOW(g_plug), targetWidth, targetHeight);

    auto* display = gdk_window_get_display(plugWindow);
    if (display && GDK_IS_X11_DISPLAY(display) && g_containerWindowId != 0) {
        auto* xDisplay = gdk_x11_display_get_xdisplay(display);
        XMoveResizeWindow(
            xDisplay,
            g_containerWindowId,
            0,
            0,
            static_cast<unsigned int>(targetWidth),
            static_cast<unsigned int>(targetHeight)
        );
        XFlush(xDisplay);
    }

    LOG_TO_FILE("[NativeBridge:Linux] GTK player surface resized with Canvas size="
        << targetWidth << "x" << targetHeight);

    g_lastOverlayWidth = targetWidth;
    g_lastOverlayHeight = targetHeight;
}

gboolean showGtkOverlayAfterLayout(gpointer) {
    g_overlayShowSource = 0;
    if (g_teardownRequested || !g_plug || g_lastOverlayWidth <= 0 || g_lastOverlayHeight <= 0) {
        return G_SOURCE_REMOVE;
    }

    // The first AWT Canvas size can be a transient layout size. Keep the
    // native surface hidden until resize notifications have settled, then
    // reveal it at the last complete geometry in one step.
    // The video and WebKit controls are now GTK siblings inside one GtkPlug.
    // Showing the tree together lets GTK compose them in one surface.
    if (!g_overlayVisible) {
        gtk_widget_show_all(g_plug);
        g_overlayVisible = true;
    }
    if (g_containerWindowId != 0) {
        auto* plugWindow = gtk_widget_get_window(g_plug);
        if (plugWindow) {
            auto* display = gdk_window_get_display(plugWindow);
            if (display && GDK_IS_X11_DISPLAY(display)) {
                auto* xDisplay = gdk_x11_display_get_xdisplay(display);
                XMapWindow(xDisplay, g_containerWindowId);
                XFlush(xDisplay);
            }
        }
    }
    if (g_glArea) gtk_widget_queue_draw(GTK_WIDGET(g_glArea));
    LOG_TO_FILE("[NativeBridge:Linux] GTK overlay revealed with MPV Render API");
    return G_SOURCE_REMOVE;
}

void scheduleGtkOverlayShow() {
    if (g_overlayShowSource != 0) {
        g_source_remove(g_overlayShowSource);
        g_overlayShowSource = 0;
    }
    // Let the AWT/Compose layout deliver any follow-up resize first. A single
    // reveal after this quiet period prevents the probing UI from being
    // painted once at the temporary size and again at the final size.
    g_overlayShowSource = g_timeout_add(120, showGtkOverlayAfterLayout, nullptr);
}

std::string makeDispatchScript(const std::string& json) {
    if (json.empty()) return {};
    return "window.__cloudstreamDispatch && window.__cloudstreamDispatch(" + json + ");";
}

std::string extractJsonString(const std::string& json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\":\"";
    const auto start = json.find(needle);
    if (start == std::string::npos) return {};
    const auto valueStart = start + needle.size();
    const auto valueEnd = json.find('"', valueStart);
    if (valueEnd == std::string::npos) return {};
    return json.substr(valueStart, valueEnd - valueStart);
}

void onScriptMessage(WebKitUserContentManager*, WebKitJavascriptResult* result, gpointer) noexcept {
    try {
        if (!result) return;
        JSCValue* value = webkit_javascript_result_get_js_value(result);
        if (!value) return;

        gchar* message = jsc_value_is_string(value)
            ? jsc_value_to_string(value)
            : jsc_value_to_json(value, 0);
        if (!message) return;

        const std::string payload(message);
        g_free(message);

        const std::string eventType = extractJsonString(payload, "type");
        if (eventType == "toggleStats") {
            g_statsVisible = !g_statsVisible;
            return;
        }
        if (eventType == "controlsVisibility") {
            // The web UI reports whether the controls page is actually visible
            // (controls, probing, video-ended, menus). This gates the snapshot
            // compositor: while hidden nothing is drawn over the video and
            // normal watching costs nothing.
            const std::string value = extractJsonString(payload, "value");
            g_controlsVisible = (value == "1");
            return;
        }

        // The shared Kotlin PlayerInboundEvent contract remains the single owner of
        // player behavior. Linux only provides the WebKit transport here.
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        const std::wstring wide = converter.from_bytes(payload);
        dispatchPlayerEvent(wide);
    } catch (const std::exception& error) {
        LOG_TO_FILE("[NativeBridge:Linux] WebKit message callback ignored exception: " << error.what());
    } catch (...) {
        LOG_TO_FILE("[NativeBridge:Linux] WebKit message callback ignored unknown exception");
    }
}

void ensureMpvSymbols() {
    if (g_mpv_get_property && g_mpv_get_property_string && g_mpv_free) return;

    static void* mpvLibrary = nullptr;
    if (!mpvLibrary) {
        constexpr const char* names[] = {
            "libmpv.so.2",
            "libmpv.so",
            "libmpv.so.1",
        };
        for (const char* name : names) {
            mpvLibrary = dlopen(name, RTLD_LAZY | RTLD_GLOBAL);
            if (mpvLibrary) break;
        }
    }
    if (!mpvLibrary) {
        const char* error = dlerror();
        LOG_TO_FILE("[NativeBridge:Linux] Could not load libmpv for state sync: "
            << (error ? error : "unknown loader error"));
        return;
    }

    g_mpv_get_property = reinterpret_cast<mpv_get_property_fn>(dlsym(mpvLibrary, "mpv_get_property"));
    g_mpv_get_property_string = reinterpret_cast<mpv_get_property_string_fn>(dlsym(mpvLibrary, "mpv_get_property_string"));
    g_mpv_free = reinterpret_cast<mpv_free_fn>(dlsym(mpvLibrary, "mpv_free"));
}

std::string vlcLastError() {
    const char* message = g_vlcErrmsg ? g_vlcErrmsg() : nullptr;
    const std::string result = message && *message ? message : "no libVLC diagnostic was returned";
    if (g_vlcClearerr) g_vlcClearerr();
    return result;
}

std::string discoverVlcPluginPath() {
    // VLC_PLUGIN_PATH may contain several colon-separated directories (same
    // convention as PATH). Pick the first one that actually exists so a stale
    // entry in the middle cannot break plugin discovery.
    if (const char* configured = g_getenv("VLC_PLUGIN_PATH"); configured && *configured) {
        std::string remaining(configured);
        while (!remaining.empty()) {
            const size_t separator = remaining.find(':');
            const std::string entry = separator == std::string::npos
                ? remaining
                : remaining.substr(0, separator);
            remaining = separator == std::string::npos ? "" : remaining.substr(separator + 1);
            if (entry.empty()) continue;
            const std::filesystem::path entryPath(entry);
            std::error_code error;
            if (std::filesystem::is_directory(entryPath, error)) {
                return entryPath.string();
            }
            LOG_TO_FILE("[NativeBridge:Linux] VLC_PLUGIN_PATH entry is not a directory: " << entry);
        }
        LOG_TO_FILE("[NativeBridge:Linux] no usable VLC_PLUGIN_PATH entry found in: " << configured);
    }

    std::vector<std::filesystem::path> candidates;
    if (g_vlcNew) {
        Dl_info libraryInfo{};
        if (dladdr(reinterpret_cast<void*>(g_vlcNew), &libraryInfo) != 0 && libraryInfo.dli_fname) {
            const auto libraryDirectory = std::filesystem::path(libraryInfo.dli_fname).parent_path();
            candidates.emplace_back(libraryDirectory / "vlc" / "plugins");
        }
    }
    candidates.emplace_back("/usr/lib/vlc/plugins");
    candidates.emplace_back("/usr/lib64/vlc/plugins");
    candidates.emplace_back("/usr/local/lib/vlc/plugins");
    candidates.emplace_back("/usr/lib/x86_64-linux-gnu/vlc/plugins");
    candidates.emplace_back("/usr/lib/aarch64-linux-gnu/vlc/plugins");

    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_directory(candidate, error)) {
            return candidate.string();
        }
    }
    return {};
}

bool ensureMpvRenderSymbols() {
    static void* mpvLibrary = nullptr;
    if (!mpvLibrary) {
        constexpr const char* names[] = {
            "libmpv.so.2",
            "libmpv.so",
            "libmpv.so.1",
        };
        for (const char* name : names) {
            mpvLibrary = dlopen(name, RTLD_LAZY | RTLD_GLOBAL);
            if (mpvLibrary) break;
        }
    }
    if (!mpvLibrary) {
        const char* error = dlerror();
        LOG_TO_FILE("[NativeBridge:Linux] Could not load libmpv Render API: "
            << (error ? error : "unknown loader error"));
        return false;
    }

    g_renderContextCreate = reinterpret_cast<mpv_render_context_create_fn>(
        dlsym(mpvLibrary, "mpv_render_context_create"));
    g_renderContextSetUpdateCallback = reinterpret_cast<mpv_render_context_set_update_callback_fn>(
        dlsym(mpvLibrary, "mpv_render_context_set_update_callback"));
    g_renderContextRender = reinterpret_cast<mpv_render_context_render_fn>(
        dlsym(mpvLibrary, "mpv_render_context_render"));
    g_renderContextReportSwap = reinterpret_cast<mpv_render_context_report_swap_fn>(
        dlsym(mpvLibrary, "mpv_render_context_report_swap"));
    g_renderContextFree = reinterpret_cast<mpv_render_context_free_fn>(
        dlsym(mpvLibrary, "mpv_render_context_free"));

    return g_renderContextCreate &&
        g_renderContextSetUpdateCallback &&
        g_renderContextRender &&
        g_renderContextReportSwap &&
        g_renderContextFree;
}

bool ensureVlcSymbols() {
    if (g_vlcNew && g_vlcRelease && g_vlcMediaNewLocation &&
        g_vlcMediaAddOption && g_vlcMediaRelease && g_vlcPlayerNewFromMedia &&
        g_vlcPlayerRelease && g_vlcPlayerPlay &&
        g_vlcPlayerStop && g_vlcPlayerPause && g_vlcPlayerGetState &&
        g_vlcPlayerIsPlaying && g_vlcPlayerGetTime && g_vlcPlayerGetLength &&
        g_vlcPlayerSetTime && g_vlcPlayerSetRate && g_vlcAudioSetVolume &&
        g_vlcAudioSetMute && g_vlcVideoSetCallbacks && g_vlcVideoSetFormatCallbacks) {
        return true;
    }

    if (!g_vlcLibrary) {
        constexpr const char* names[] = {
            "libvlc.so.5",
            "libvlc.so",
        };
        for (const char* name : names) {
            g_vlcLibrary = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (g_vlcLibrary) break;
        }
    }
    if (!g_vlcLibrary) {
        const char* error = dlerror();
        LOG_TO_FILE("[NativeBridge:Linux] libVLC runtime library was not found: "
            << (error ? error : "unknown loader error"));
        return false;
    }

#define LOAD_VLC_SYMBOL(field, symbol) \
    field = reinterpret_cast<decltype(field)>(dlsym(g_vlcLibrary, symbol))
    LOAD_VLC_SYMBOL(g_vlcNew, "libvlc_new");
    LOAD_VLC_SYMBOL(g_vlcRelease, "libvlc_release");
    LOAD_VLC_SYMBOL(g_vlcMediaNewLocation, "libvlc_media_new_location");
    LOAD_VLC_SYMBOL(g_vlcMediaAddOption, "libvlc_media_add_option");
    LOAD_VLC_SYMBOL(g_vlcMediaRelease, "libvlc_media_release");
    LOAD_VLC_SYMBOL(g_vlcPlayerNewFromMedia, "libvlc_media_player_new_from_media");
    LOAD_VLC_SYMBOL(g_vlcPlayerRelease, "libvlc_media_player_release");
    LOAD_VLC_SYMBOL(g_vlcPlayerPlay, "libvlc_media_player_play");
    LOAD_VLC_SYMBOL(g_vlcPlayerStop, "libvlc_media_player_stop");
    LOAD_VLC_SYMBOL(g_vlcPlayerPause, "libvlc_media_player_pause");
    LOAD_VLC_SYMBOL(g_vlcPlayerGetState, "libvlc_media_player_get_state");
    LOAD_VLC_SYMBOL(g_vlcPlayerIsPlaying, "libvlc_media_player_is_playing");
    LOAD_VLC_SYMBOL(g_vlcPlayerGetTime, "libvlc_media_player_get_time");
    LOAD_VLC_SYMBOL(g_vlcPlayerGetLength, "libvlc_media_player_get_length");
    LOAD_VLC_SYMBOL(g_vlcPlayerSetTime, "libvlc_media_player_set_time");
    LOAD_VLC_SYMBOL(g_vlcPlayerSetRate, "libvlc_media_player_set_rate");
    LOAD_VLC_SYMBOL(g_vlcAudioSetVolume, "libvlc_audio_set_volume");
    LOAD_VLC_SYMBOL(g_vlcAudioSetMute, "libvlc_audio_set_mute");

    LOAD_VLC_SYMBOL(g_vlcVideoSetCallbacks, "libvlc_video_set_callbacks");
    LOAD_VLC_SYMBOL(g_vlcVideoSetFormatCallbacks, "libvlc_video_set_format_callbacks");
    g_vlcErrmsg = reinterpret_cast<libvlc_errmsg_fn>(dlsym(g_vlcLibrary, "libvlc_errmsg"));
    g_vlcClearerr = reinterpret_cast<libvlc_clearerr_fn>(dlsym(g_vlcLibrary, "libvlc_clearerr"));
    const bool complete = g_vlcNew && g_vlcRelease && g_vlcMediaNewLocation &&
#undef LOAD_VLC_SYMBOL
        g_vlcMediaAddOption && g_vlcMediaRelease && g_vlcPlayerNewFromMedia &&
        g_vlcPlayerRelease && g_vlcPlayerPlay &&
        g_vlcPlayerStop && g_vlcPlayerPause && g_vlcPlayerGetState &&
        g_vlcPlayerIsPlaying && g_vlcPlayerGetTime && g_vlcPlayerGetLength &&
        g_vlcPlayerSetTime && g_vlcPlayerSetRate && g_vlcAudioSetVolume &&
        g_vlcAudioSetMute && g_vlcVideoSetCallbacks && g_vlcVideoSetFormatCallbacks;
    if (!complete) {
        LOG_TO_FILE("[NativeBridge:Linux] libVLC is present but required symbols are missing");
    }
    return complete;
}

void* getGlProcAddress(void*, const char* name) {
    static void* glLibrary = dlopen("libGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (!glLibrary || !name) return nullptr;

    using GetProcAddressFn = void* (*)(const unsigned char*);
    auto getProc = reinterpret_cast<GetProcAddressFn>(dlsym(glLibrary, "glXGetProcAddressARB"));
    if (getProc) {
        if (void* address = getProc(reinterpret_cast<const unsigned char*>(name))) {
            return address;
        }
    }
    return dlsym(glLibrary, name);
}

void mpvRenderUpdateCallback(void*) {
    {
        std::lock_guard<std::mutex> renderLock(g_renderMutex);
        if (!g_mpvRenderContext) return;
    }
    {
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        if (g_teardownRequested || !g_mainContext || !g_glArea) return;
    }
    invokeOnGtkThread([] {
        // This callback may be queued immediately before GTK teardown. Keep
        // the pointer check and the GTK call on the GTK thread, under the
        // lifecycle lock, so a queued render can never target a destroyed
        // GtkGLArea.
        std::lock_guard<std::mutex> lifecycleLock(g_lifecycleMutex);
        std::lock_guard<std::mutex> renderLock(g_renderMutex);
        if (!g_teardownRequested && g_glArea && g_mpvRenderContext) {
            gtk_gl_area_queue_render(g_glArea);
        }
    });
}

gboolean renderMpvFrame(GtkGLArea* area, GdkGLContext*, gpointer) {
    std::lock_guard<std::mutex> lock(g_renderMutex);
    if (!g_mpvRenderContext || !g_renderContextRender) return FALSE;

    const int width = std::max(gtk_widget_get_allocated_width(GTK_WIDGET(area)), 1);
    const int height = std::max(gtk_widget_get_allocated_height(GTK_WIDGET(area)), 1);
    // GtkGLArea renders into a GTK-owned framebuffer. FBO 0 is not
    // guaranteed to be the active target, and using it makes MPV decode and
    // audio correctly while every video frame is presented elsewhere.
    using GlGetIntegervFn = void (*)(unsigned int, int*);
    auto glGetIntegerv = reinterpret_cast<GlGetIntegervFn>(
        getGlProcAddress(nullptr, "glGetIntegerv"));
    int activeFbo = 0;
    if (glGetIntegerv) {
        constexpr unsigned int kDrawFramebufferBinding = 0x8CA6;
        glGetIntegerv(kDrawFramebufferBinding, &activeFbo);
    }
    if (!g_mpvFirstFrameLogged) {
        g_mpvFirstFrameLogged = true;
        LOG_TO_FILE("[NativeBridge:Linux] First MPV Render API frame: FBO="
            << activeFbo << ", size=" << width << "x" << height);
    }
    mpv_opengl_fbo fbo{activeFbo, width, height, 0};
    // GtkGLArea's framebuffer has the opposite vertical origin from the
    // orientation expected by the desktop player surface.
    int flipY = 1;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flipY},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };

    const int result = g_renderContextRender(g_mpvRenderContext, params);
    if (result < 0) {
        LOG_TO_FILE("[NativeBridge:Linux] MPV Render API frame failed: " << result);
        return FALSE;
    }
    // Flicker-free overlay: draw the WebKit controls snapshot inside the same
    // GL pass as the video (no transparent X window involved).
    drawOverlayTexture(width, height);
    g_renderContextReportSwap(g_mpvRenderContext);
    return TRUE;
}

// VLC used to render through a child X11 window attached with
// libvlc_media_player_set_xwindow(). That path can keep decoding and playing
// audio while the compositor presents an empty/black child window. Feed VLC
// frames into a normal GTK drawing area instead, so video and the transparent
// WebKit controls share the same GTK composition tree.
unsigned vlcVideoFormat(void** opaque, char* chroma, unsigned* width, unsigned* height,
    unsigned* pitches, unsigned* lines) {
    if (!opaque || !chroma || !width || !height || !pitches || !lines || *width == 0 || *height == 0) {
        return 0;
    }

    auto* frame = g_vlcFrame.load();
    if (!frame) return 0;
    *opaque = frame;
    std::lock_guard<std::mutex> lock(frame->mutex);
    if (!frame->active) return 0;
    frame->width = *width;
    frame->height = *height;
    frame->stride = *width * 4;
    frame->pixels.resize(static_cast<size_t>(frame->stride) * frame->height);
    std::fill(frame->pixels.begin(), frame->pixels.end(), 0);
    frame->configured = true;

    // RV32 is VLC's native 32-bit packed RGB output. On little-endian Linux
    // its byte order is compatible with Cairo's ARGB32 storage layout.
    chroma[0] = 'R';
    chroma[1] = 'V';
    chroma[2] = '3';
    chroma[3] = '2';
    pitches[0] = frame->stride;
    lines[0] = frame->height;
    return 1;
}

void vlcVideoCleanup(void* opaque) {
    auto* frame = static_cast<VlcFrameBuffer*>(opaque);
    if (!frame) return;
    std::lock_guard<std::mutex> lock(frame->mutex);
    frame->configured = false;
}

void* vlcVideoLock(void* opaque, void** planes) {
    auto* frame = static_cast<VlcFrameBuffer*>(opaque);
    if (!frame || !planes) return nullptr;

    frame->mutex.lock();
    if (!frame->active || !frame->configured || frame->pixels.empty()) {
        frame->mutex.unlock();
        return nullptr;
    }
    ++frame->callbacksInFlight;
    planes[0] = frame->pixels.data();
    return frame;
}

void vlcVideoUnlock(void*, void* picture, const void* const*) {
    auto* frame = static_cast<VlcFrameBuffer*>(picture);
    if (!frame) return;
    if (frame->callbacksInFlight > 0) --frame->callbacksInFlight;
    const bool callbacksDone = frame->callbacksInFlight == 0;
    frame->mutex.unlock();
    if (callbacksDone) frame->callbacksCv.notify_all();
}

void vlcVideoDisplay(void* opaque, void*) {
    auto* frame = static_cast<VlcFrameBuffer*>(opaque);
    if (!frame) return;

    bool firstFrame = false;
    unsigned frameWidth = 0;
    unsigned frameHeight = 0;
    {
        std::lock_guard<std::mutex> lock(frame->mutex);
        if (!frame->active || !frame->configured) return;
        ++frame->callbacksInFlight;
        firstFrame = frame->frameCount == 0;
        frameWidth = frame->width;
        frameHeight = frame->height;
        ++frame->frameCount;
    }
    if (firstFrame) {
        LOG_TO_FILE("[NativeBridge:Linux] First VLC video callback frame: "
            << frameWidth << "x" << frameHeight);
    }

    invokeOnGtkThread([] {
        std::lock_guard<std::mutex> lifecycleLock(g_lifecycleMutex);
        if (!g_teardownRequested && g_vlcArea) {
            gtk_widget_queue_draw(g_vlcArea);
        }
    });
    {
        std::lock_guard<std::mutex> lock(frame->mutex);
        if (frame->callbacksInFlight > 0) --frame->callbacksInFlight;
        if (frame->callbacksInFlight == 0) frame->callbacksCv.notify_all();
    }
}

gboolean drawVlcFrame(GtkWidget* widget, cairo_t* cairo, gpointer) {
    if (!cairo) return FALSE;

    cairo_set_source_rgb(cairo, 0.0, 0.0, 0.0);
    cairo_paint(cairo);

    auto* frame = g_vlcFrame.load();
    if (!frame) return FALSE;
    std::lock_guard<std::mutex> lock(frame->mutex);
    if (!frame->active || !frame->configured || frame->pixels.empty() ||
        frame->width == 0 || frame->height == 0) {
        return FALSE;
    }

    auto* image = cairo_image_surface_create_for_data(
        frame->pixels.data(),
        CAIRO_FORMAT_ARGB32,
        static_cast<int>(frame->width),
        static_cast<int>(frame->height),
        static_cast<int>(frame->stride)
    );
    if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(image);
        return FALSE;
    }

    const double targetWidth = std::max(1, gtk_widget_get_allocated_width(widget));
    const double targetHeight = std::max(1, gtk_widget_get_allocated_height(widget));
    const double scale = std::min(
        targetWidth / static_cast<double>(frame->width),
        targetHeight / static_cast<double>(frame->height)
    );
    const double drawWidth = static_cast<double>(frame->width) * scale;
    const double drawHeight = static_cast<double>(frame->height) * scale;

    cairo_save(cairo);
    cairo_translate(cairo, (targetWidth - drawWidth) / 2.0, (targetHeight - drawHeight) / 2.0);
    cairo_scale(cairo, scale, scale);
    cairo_set_source_surface(cairo, image, 0.0, 0.0);
    cairo_pattern_set_filter(cairo_get_source(cairo), CAIRO_FILTER_BILINEAR);
    cairo_paint(cairo);
    cairo_restore(cairo);
    cairo_surface_destroy(image);
    return FALSE;
}

bool attachMpvRenderOnGtk(mpv_handle* handle) {
    if (!handle || g_teardownRequested || !g_glArea || g_mpvRenderContext) return g_mpvRenderContext != nullptr;
    if (!ensureMpvRenderSymbols()) {
        LOG_TO_FILE("[NativeBridge:Linux] MPV Render API symbols are unavailable");
        return false;
    }

    GdkDisplay* display = gdk_display_get_default();
    auto* xDisplay = (display && GDK_IS_X11_DISPLAY(display))
        ? gdk_x11_display_get_xdisplay(display)
        : nullptr;

    auto createRenderContext = [&]() -> std::pair<int, mpv_render_context*> {
        gtk_gl_area_make_current(g_glArea);
        if (auto* error = gtk_gl_area_get_error(g_glArea)) {
            LOG_TO_FILE("[NativeBridge:Linux] GtkGLArea initialization failed: " << error->message);
            return {-1, nullptr};
        }

        const char* api = MPV_RENDER_API_TYPE_OPENGL;
        mpv_opengl_init_params glInit{getGlProcAddress, nullptr};
        mpv_render_param params[4] = {
            {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(api)},
            {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
            {MPV_RENDER_PARAM_X11_DISPLAY, xDisplay},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };

        mpv_render_context* renderContext = nullptr;
        const int result = g_renderContextCreate(&renderContext, handle, params);
        return {result, renderContext};
    };

    auto [result, renderContext] = createRenderContext();

    // When the reusable GTK plug has just been reparented, its GtkGLArea can
    // still be hidden/unmapped even though it is realized. MPV then reports
    // MPV_ERROR_UNSUPPORTED (-18) because no usable GL surface is current.
    // Recover only this transition case: reveal/map the existing surface,
    // synchronize X11, and create the render context again. The normal first
    // open remains hidden until the settled layout reveal timer.
    if ((result < 0 || !renderContext) && result == MPV_ERROR_UNSUPPORTED &&
        g_plug && !g_overlayVisible) {
        LOG_TO_FILE("[NativeBridge:Linux] MPV Render API needs a visible reusable GTK surface; retrying after map");
        gtk_widget_show_all(g_plug);
        g_overlayVisible = true;
        if (g_containerWindowId != 0) {
            auto* plugWindow = gtk_widget_get_window(g_plug);
            if (plugWindow) {
                auto* plugDisplay = gdk_window_get_display(plugWindow);
                if (plugDisplay && GDK_IS_X11_DISPLAY(plugDisplay)) {
                    auto* displayHandle = gdk_x11_display_get_xdisplay(plugDisplay);
                    XMapWindow(displayHandle, g_containerWindowId);
                    XFlush(displayHandle);
                    XSync(displayHandle, False);
                }
            }
        }
        const auto retry = createRenderContext();
        result = retry.first;
        renderContext = retry.second;
    }

    if (result < 0 || !renderContext) {
        LOG_TO_FILE("[NativeBridge:Linux] MPV Render API creation failed: " << result);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_renderMutex);
        g_mpvRenderContext = renderContext;
        g_renderMpvHandle = handle;
        g_mpvFirstFrameLogged = false;
    }
    g_renderContextSetUpdateCallback(renderContext, mpvRenderUpdateCallback, nullptr);
    gtk_widget_queue_draw(GTK_WIDGET(g_glArea));
    LOG_TO_FILE("[NativeBridge:Linux] MPV Render API attached to GtkGLArea");
    return true;
}

void detachMpvRenderOnGtk() {
    std::lock_guard<std::mutex> lock(g_renderMutex);
    if (g_mpvRenderContext && g_renderContextFree) {
        // Stop new render-update callbacks before releasing the context.
        // Otherwise MPV can queue one more GtkGLArea redraw while GTK is
        // already tearing the surface down.
        if (g_renderContextSetUpdateCallback) {
            g_renderContextSetUpdateCallback(g_mpvRenderContext, nullptr, nullptr);
        }
        g_renderContextFree(g_mpvRenderContext);
    }
    g_mpvRenderContext = nullptr;
    g_renderMpvHandle = nullptr;
}

gboolean syncMpvState(gpointer) {
    // Keep the MPV mutex for the complete poll. DesktopMpvEngine clears this
    // pointer before terminating the handle; releasing it after only the copy
    // would allow a stale callback to read freed MPV memory.
    std::lock_guard<std::mutex> mpvLock(g_mpvMutex);
    mpv_handle* handle = g_mpvHandle;
    if (!handle || !g_webView) return G_SOURCE_CONTINUE;

    ensureMpvSymbols();
    if (!g_mpv_get_property) return G_SOURCE_CONTINUE;

    double duration = 0.0;
    double position = 0.0;
    double cacheDuration = 0.0;
    int paused = 0;
    int coreIdle = 0;
    int pausedForCache = 0;
    g_mpv_get_property(handle, "duration", MPV_FORMAT_DOUBLE, &duration);
    g_mpv_get_property(handle, "time-pos", MPV_FORMAT_DOUBLE, &position);
    g_mpv_get_property(handle, "pause", MPV_FORMAT_FLAG, &paused);
    g_mpv_get_property(handle, "core-idle", MPV_FORMAT_FLAG, &coreIdle);
    g_mpv_get_property(handle, "paused-for-cache", MPV_FORMAT_FLAG, &pausedForCache);
    g_mpv_get_property(handle, "demuxer-cache-duration", MPV_FORMAT_DOUBLE, &cacheDuration);

    const bool buffering = pausedForCache != 0 || (coreIdle != 0 && paused == 0);
    const long long positionMs = static_cast<long long>(position * 1000.0);
    const long long durationMs = static_cast<long long>(duration * 1000.0);
    const long long bufferMs = static_cast<long long>((position + cacheDuration) * 1000.0);

    std::ostringstream json;
    json << "{\"type\":\"state_update\",\"positionMs\":" << positionMs
         << ",\"bufferMs\":" << bufferMs
         << ",\"durationMs\":" << durationMs
         << ",\"isLoading\":" << (buffering ? "true" : "false")
         << ",\"isPlaying\":" << (paused ? "false" : "true") << "}";
    runJavascript(makeDispatchScript(json.str()));
    return G_SOURCE_CONTINUE;
}

void startMpvSyncOnGtk() {
    if (!g_mainContext || g_mpvSyncSource != 0) return;
    g_mpvSyncSource = g_timeout_add(16, syncMpvState, nullptr);
    LOG_TO_FILE("[NativeBridge:Linux] MPV -> WebView sync started");
}

void stopMpvSyncOnGtk() {
    if (g_mpvSyncSource != 0) {
        g_source_remove(g_mpvSyncSource);
        g_mpvSyncSource = 0;
        LOG_TO_FILE("[NativeBridge:Linux] MPV -> WebView sync stopped");
    }
}

gboolean syncVlcState(gpointer) {
    long long positionMs = 0;
    long long durationMs = 0;
    int state = 0;
    int playing = 0;
    {
        std::lock_guard<std::mutex> vlcLock(g_vlcMutex);
        if (!g_vlcPlayer || !g_vlcPlayerGetState || !g_vlcPlayerGetTime || !g_vlcPlayerGetLength) {
            return G_SOURCE_CONTINUE;
        }
        state = g_vlcPlayerGetState(g_vlcPlayer);
        playing = g_vlcPlayerIsPlaying ? g_vlcPlayerIsPlaying(g_vlcPlayer) : 0;
        positionMs = std::max(0LL, g_vlcPlayerGetTime(g_vlcPlayer));
        durationMs = std::max(0LL, g_vlcPlayerGetLength(g_vlcPlayer));
    }

    // libVLC state values are stable in the public VLC 3 ABI:
    // Opening=1, Buffering=2, Playing=3, Paused=4, Ended=6, Error=7.
    const bool buffering = state == 1 || state == 2 || (state == 0 && playing == 0 && durationMs == 0);
    const bool isPlaying = playing != 0 || state == 3;
    std::ostringstream json;
    json << "{\"type\":\"state_update\",\"positionMs\":" << positionMs
         << ",\"bufferMs\":" << (buffering ? positionMs : durationMs)
         << ",\"durationMs\":" << durationMs
         << ",\"isLoading\":" << (buffering ? "true" : "false")
         << ",\"isPlaying\":" << (isPlaying ? "true" : "false")
         << ",\"state\":" << state << "}";
    const std::string stateJson = json.str();
    runJavascript(makeDispatchScript(stateJson));
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    dispatchPlayerEvent(converter.from_bytes(stateJson));
    return G_SOURCE_CONTINUE;
}

void startVlcSyncOnGtk() {
    if (!g_mainContext || g_vlcSyncSource != 0) return;
    g_vlcSyncSource = g_timeout_add(80, syncVlcState, nullptr);
    LOG_TO_FILE("[NativeBridge:Linux] VLC -> WebView sync started");
}

void stopVlcSyncOnGtk() {
    if (g_vlcSyncSource != 0) {
        g_source_remove(g_vlcSyncSource);
        g_vlcSyncSource = 0;
        LOG_TO_FILE("[NativeBridge:Linux] VLC -> WebView sync stopped");
    }
}

void stopVlcOnGtk() {
    stopVlcSyncOnGtk();

    // Detach the callback context before stopping VLC. New callbacks from
    // this session will now fail their active check, while the old context
    // remains alive until all callbacks have drained.
    auto* frame = g_vlcFrame.exchange(nullptr);
    if (frame) {
        std::lock_guard<std::mutex> frameLock(frame->mutex);
        frame->active = false;
        frame->configured = false;
    }

    libvlc_media_player_t* player = nullptr;
    libvlc_instance_t* instance = nullptr;
    {
        std::lock_guard<std::mutex> vlcLock(g_vlcMutex);
        player = g_vlcPlayer;
        instance = g_vlcInstance;
        g_vlcPlayer = nullptr;
        g_vlcInstance = nullptr;
    }
    if (player) {
        if (g_vlcPlayerStop) g_vlcPlayerStop(player);
        if (g_vlcPlayerRelease) g_vlcPlayerRelease(player);
    }
    if (instance && g_vlcRelease) {
        g_vlcRelease(instance);
    }
    if (frame) {
        std::unique_lock<std::mutex> frameLock(frame->mutex);
        const bool callbacksDone = frame->callbacksCv.wait_for(frameLock, std::chrono::seconds(2), [frame] { return frame->callbacksInFlight == 0; });
        if (!callbacksDone) {
            LOG_TO_FILE("[NativeBridge:Linux] VLC callback barrier timed out; retaining session context");
        }
        frame->pixels.clear();
        frame->pixels.shrink_to_fit();
        frame->width = 0;
        frame->height = 0;
        frame->stride = 0;
        frame->frameCount = 0;
        g_vlcRetiredFrames.emplace_back(frame);
    }
    if (g_vlcArea) gtk_widget_hide(g_vlcArea);
    if (g_glArea) gtk_widget_show(GTK_WIDGET(g_glArea));
}

bool startVlcOnGtk(
    const std::string& url,
    const std::string& title,
    const std::string& userAgent,
    const std::string& referer,
    long long startMs
) {
    if (url.empty() || !g_vlcArea || !ensureVlcSymbols()) return false;
    stopVlcOnGtk();

    // libVLC discovers system plugins from libvlccore. Packaged/custom VLC
    // layouts can override that location explicitly without changing the
    // process-wide environment. When the environment is not configured,
    // derive the plugin directory from the loaded libVLC path before falling
    // back to the common multi-arch locations.
    std::vector<std::string> vlcOptionStorage;
    std::vector<const char*> vlcOptions;
    const std::string pluginPath = discoverVlcPluginPath();
    if (!pluginPath.empty()) {
        vlcOptionStorage.emplace_back("--plugin-path=" + pluginPath);
        vlcOptions.push_back(vlcOptionStorage.back().c_str());
        LOG_TO_FILE("[NativeBridge:Linux] Using VLC plugin path: " << pluginPath);
    }
    auto* vlcInstance = g_vlcNew(
        static_cast<int>(vlcOptions.size()),
        vlcOptions.empty() ? nullptr : vlcOptions.data()
    );
    if (!vlcInstance) {
        const std::string firstError = vlcLastError();
        LOG_TO_FILE("[NativeBridge:Linux] libvlc_new failed with plugin path: " << firstError);
        if (!pluginPath.empty()) {
            // A stale/partial --plugin-path can make libvlc_new fail even when
            // the system VLC installation is otherwise healthy. Retry without
            // the explicit path so libvlccore falls back to its compiled-in
            // plugin directory before we give up.
            LOG_TO_FILE("[NativeBridge:Linux] Retrying libvlc_new without an explicit plugin path");
            vlcOptionStorage.clear();
            vlcOptions.clear();
            vlcInstance = g_vlcNew(0, nullptr);
        }
        if (!vlcInstance) {
            LOG_TO_FILE("[NativeBridge:Linux] libvlc_new failed: " << vlcLastError()
                << "; verify VLC plugins are installed and VLC_PLUGIN_PATH is valid");
            return false;
        }
    }
    auto* media = g_vlcMediaNewLocation(vlcInstance, url.c_str());
    if (!media) {
        g_vlcRelease(vlcInstance);
        LOG_TO_FILE("[NativeBridge:Linux] libvlc_media_new_location failed: " << vlcLastError());
        return false;
    }

    if (!userAgent.empty()) {
        const std::string option = ":http-user-agent=" + userAgent;
        g_vlcMediaAddOption(media, option.c_str());
    }
    if (!referer.empty()) {
        const std::string option = ":http-referrer=" + referer;
        g_vlcMediaAddOption(media, option.c_str());
    }
    if (!title.empty()) {
        const std::string option = ":meta-title=" + title;
        g_vlcMediaAddOption(media, option.c_str());
    }
    auto* player = g_vlcPlayerNewFromMedia(media);
    g_vlcMediaRelease(media);
    if (!player) {
        g_vlcRelease(vlcInstance);
        LOG_TO_FILE("[NativeBridge:Linux] libvlc_media_player_new_from_media failed: " << vlcLastError());
        return false;
    }

    // VLC receives decoded frames through callbacks and paints them into the
    // reusable GTK drawing area. This avoids a separate X11 child window,
    // which is the source of the audio-with-black-video failure on Linux.
    auto* frame = new VlcFrameBuffer();
    {
        std::lock_guard<std::mutex> frameLock(frame->mutex);
        frame->active = true;
        frame->configured = false;
        frame->frameCount = 0;
    }
    g_vlcFrame.store(frame);
    g_vlcVideoSetCallbacks(
        player,
        vlcVideoLock,
        vlcVideoUnlock,
        vlcVideoDisplay,
        frame
    );
    g_vlcVideoSetFormatCallbacks(player, vlcVideoFormat, vlcVideoCleanup);
    gtk_widget_show(g_vlcArea);
    gtk_widget_hide(GTK_WIDGET(g_glArea));

    {
        std::lock_guard<std::mutex> vlcLock(g_vlcMutex);
        g_vlcInstance = vlcInstance;
        g_vlcPlayer = player;
    }
    if (g_vlcPlayerPlay(player) != 0) {
        LOG_TO_FILE("[NativeBridge:Linux] libvlc_media_player_play failed: " << vlcLastError());
        stopVlcOnGtk();
        return false;
    }
    if (startMs > 0 && g_vlcPlayerSetTime) {
        g_vlcPlayerSetTime(player, startMs);
    }
    gtk_widget_queue_draw(g_vlcArea);
    LOG_TO_FILE("[NativeBridge:Linux] libVLC started with GTK video callbacks");
    return true;
}

// Keep the GTK/WebKit tree alive between player sessions. Recreating an
// accelerated WebKit/X11 surface after every Back is not reliable on Linux;
// only the MPV render context and the host Canvas need to change per session.
void parkGtkOverlayForReuseOnGtk() {
    stopVlcOnGtk();
    stopMpvSyncOnGtk();
    if (g_overlayShowSource != 0) {
        g_source_remove(g_overlayShowSource);
        g_overlayShowSource = 0;
    }

    // The MPV render context must be detached before the old mpv handle is
    // terminated, but the GTK/WebKit widgets themselves remain reusable.
    detachMpvRenderOnGtk();

    if (g_webView) {
        webkit_web_view_stop_loading(g_webView);
    }

    // Do not leave the plug under the AWT Canvas. AWT destroys its X11 child
    // window as soon as the Compose player is removed, which would invalidate
    // every GtkWidget/GdkWindow pointer kept for the next player session.
    if (g_plug && g_parkingWindowId != 0) {
        auto* plugWindow = gtk_widget_get_window(g_plug);
        auto* display = plugWindow ? gdk_window_get_display(plugWindow) : nullptr;
        if (plugWindow && display && GDK_IS_X11_DISPLAY(display)) {
            auto* xDisplay = gdk_x11_display_get_xdisplay(display);
            const Window surfaceWindow = g_renderWindowId != 0
                ? g_renderWindowId
                : g_containerWindowId;
            if (surfaceWindow != 0) {
                XReparentWindow(xDisplay, surfaceWindow, g_parkingWindowId, 0, 0);
                XUnmapWindow(xDisplay, surfaceWindow);
                XFlush(xDisplay);
                XSync(xDisplay, False);
                LOG_TO_FILE("[NativeBridge:Linux] GTK surface moved to private parking window before AWT Canvas removal");
            }
        }
        gtk_widget_hide(g_plug);
        g_containerWindowId = 0;
        g_hostWindowId = 0;
        if (auto* display = gtk_widget_get_display(g_plug)) {
            gdk_display_flush(display);
            gdk_display_sync(display);
        }
    } else if (g_plug) {
        // This is only a fallback for an initialization failure before the
        // parking window could be created.
        gtk_widget_hide(g_plug);
    }
    g_overlayVisible = false;
    g_lastOverlayWidth = 0;
    g_lastOverlayHeight = 0;
    g_lastOverlayX = 0;
    g_lastOverlayY = 0;
    g_lastOverlayExternal = false;
    g_teardownRequested = false;
}

void runGtkThread(Window hostWindow, int width, int height) {
    // Compose Desktop's AWT Canvas currently exposes an X11 window id. In a
    // Wayland session this is normally an XWayland window; selecting GTK's
    // X11 backend keeps WebKitGTK and MPV on the same native surface instead
    // of creating an unrelated Wayland toplevel. This is an explicit,
    // documented compatibility path—not a claim of native Wayland embedding.
    const bool waylandSession =
        g_getenv("WAYLAND_DISPLAY") != nullptr ||
        g_strcmp0(g_getenv("XDG_SESSION_TYPE"), "wayland") == 0;
    if (waylandSession && !g_getenv("GDK_BACKEND") && g_getenv("DISPLAY")) {
        g_setenv("GDK_BACKEND", "x11", FALSE);
        LOG_TO_FILE("[NativeBridge:Linux] Wayland session with X11 host detected; using GTK XWayland compatibility backend");
    }

    int argc = 0;
    char** argv = nullptr;
    if (!gtk_init_check(&argc, &argv)) {
        LOG_TO_FILE("[NativeBridge:Linux] GTK initialization failed");
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        g_initComplete = true;
        g_initCv.notify_one();
        return;
    }

    GdkDisplay* display = gdk_display_get_default();
    if (!display || !GDK_IS_X11_DISPLAY(display)) {
        LOG_TO_FILE("[NativeBridge:Linux] WebKitGTK embedding requires an X11/XWayland host; native Wayland embedding is not available in the current AWT Canvas architecture");
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        g_initComplete = true;
        g_initCv.notify_one();
        return;
    }

    auto* xDisplay = gdk_x11_display_get_xdisplay(display);
    const Window parkingWindow = XCreateSimpleWindow(
        xDisplay,
        DefaultRootWindow(xDisplay),
        -100,
        -100,
        1,
        1,
        0,
        0,
        0
    );
    if (parkingWindow == 0) {
        LOG_TO_FILE("[NativeBridge:Linux] Failed to create the GTK parking window");
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        g_initComplete = true;
        g_initCv.notify_one();
        return;
    }
    XUnmapWindow(xDisplay, parkingWindow);
    XFlush(xDisplay);
    g_parkingWindowId = parkingWindow;

    auto* context = g_main_context_default();
    auto* loop = g_main_loop_new(context, FALSE);
    // The AWT Canvas is an X11 child, not a GtkSocket. Create a single GTK
    // plug without an XEmbed socket and explicitly reparent it into the AWT
    // host after realization. MPV itself is rendered into GtkGLArea below.
    auto* plug = gtk_plug_new(0);
    if (!plug) {
        LOG_TO_FILE("[NativeBridge:Linux] gtk_plug_new failed");
        g_main_loop_unref(loop);
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        g_initComplete = true;
        g_initCv.notify_one();
        return;
    }

    auto* manager = webkit_user_content_manager_new();
    webkit_user_content_manager_register_script_message_handler(manager, "cloudstream");
    g_signal_connect(manager, "script-message-received::cloudstream", G_CALLBACK(onScriptMessage), nullptr);

    auto* shim = webkit_user_script_new(
        kWebViewShim,
        WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        nullptr,
        nullptr
    );
    webkit_user_content_manager_add_script(manager, shim);
    webkit_user_script_unref(shim);

    // Alpha-correct controls snapshots: WebKit's DMABUF/compositing paths read
    // back degraded alpha in snapshots (near-opaque chrome over the video); the
    // software path snapshots with correct alpha everywhere and the controls
    // page is cheap to render. The page is offscreen, so disabling accelerated
    // compositing cannot cause the ghosting seen with an on-screen overlay.
    // Set before the web process spawns (overwrite=0 keeps user overrides).
    setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 0);
    setenv("WEBKIT_DISABLE_COMPOSITING_MODE", "1", 0);

    const auto [webkitDataDirectory, webkitCacheDirectory] = webkitStorageDirectories();
    auto* dataManager = webkit_website_data_manager_new(
        "base-data-directory", webkitDataDirectory.c_str(),
        "base-cache-directory", webkitCacheDirectory.c_str(),
        nullptr
    );
    auto* webContext = webkit_web_context_new_with_website_data_manager(dataManager);
    auto* webView = WEBKIT_WEB_VIEW(g_object_new(
        WEBKIT_TYPE_WEB_VIEW,
        "web-context", webContext,
        "user-content-manager", manager,
        nullptr
    ));
    g_object_unref(webContext);
    g_object_unref(dataManager);
    g_object_unref(manager);
    LOG_TO_FILE("[NativeBridge:Linux] WebKit data directory=" << webkitDataDirectory
        << ", cache directory=" << webkitCacheDirectory);
    // Keep WebKit's normal accelerated path so the transparent child does not
    // become an opaque black surface over MPV. The ARGB plug is explicitly
    // cleared on GTK draw events to remove stale alpha pixels instead.
    LOG_TO_FILE("[NativeBridge:Linux] WebKit hardware compositing enabled; transparent plug clearing active");
    if (auto* rgbaVisual = gdk_screen_get_rgba_visual(gtk_widget_get_screen(plug))) {
        gtk_widget_set_visual(plug, rgbaVisual);
        gtk_widget_set_visual(GTK_WIDGET(webView), rgbaVisual);
    }
    gtk_widget_set_app_paintable(plug, TRUE);
    gtk_widget_set_can_focus(GTK_WIDGET(webView), TRUE);
    GdkRGBA transparent = {0.0, 0.0, 0.0, 0.0};
    webkit_web_view_set_background_color(webView, &transparent);

    gtk_window_set_decorated(GTK_WINDOW(plug), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(plug), FALSE);
    g_signal_connect(plug, "draw", G_CALLBACK(clearTransparentPlug), nullptr);
    auto* playerOverlay = gtk_overlay_new();
    auto* glArea = gtk_gl_area_new();
    auto* vlcArea = gtk_drawing_area_new();
    gtk_gl_area_set_auto_render(GTK_GL_AREA(glArea), FALSE);
    gtk_widget_set_hexpand(glArea, TRUE);
    gtk_widget_set_vexpand(glArea, TRUE);
    gtk_widget_set_hexpand(vlcArea, TRUE);
    gtk_widget_set_vexpand(vlcArea, TRUE);
    gtk_widget_set_no_show_all(vlcArea, TRUE);
    gtk_widget_set_hexpand(playerOverlay, TRUE);
    gtk_widget_set_vexpand(playerOverlay, TRUE);
    gtk_container_add(GTK_CONTAINER(playerOverlay), glArea);
    // The WebKit controls page is NOT a visible child of the plug anymore: it
    // lives in a separate composite-redirected toplevel (below) so no
    // transparent window stacks over the video.
    gtk_overlay_add_overlay(GTK_OVERLAY(playerOverlay), vlcArea);
    g_signal_connect(glArea, "render", G_CALLBACK(renderMpvFrame), nullptr);
    g_signal_connect(vlcArea, "draw", G_CALLBACK(drawVlcFrame), nullptr);
    gtk_container_add(GTK_CONTAINER(plug), playerOverlay);
    gtk_widget_set_halign(vlcArea, GTK_ALIGN_FILL);
    gtk_widget_set_valign(vlcArea, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(vlcArea, TRUE);
    gtk_widget_set_vexpand(vlcArea, TRUE);
    gtk_widget_show_all(plug);
    gtk_widget_hide(vlcArea);
    const Window renderWindowId = gtk_plug_get_id(GTK_PLUG(plug));
    const bool reparented = reparentGtkPlugToHost(
        renderWindowId,
        hostWindow,
        std::max(width, 1),
        std::max(height, 1)
    );
    // Realize the GTK/WebKit tree before mpv_render_context_create(). The
    // player remains hidden until the first settled Canvas resize.
    gtk_widget_hide(plug);
    gtk_window_resize(GTK_WINDOW(plug), std::max(width, 1), std::max(height, 1));

    // Flicker-free overlay (root fix): host the WebKit controls page in its
    // own undecorated window, reparented under the AWT canvas (so GDK
    // dispatches pointer/keyboard events to it), then redirected offscreen via
    // the Composite extension (manual). It keeps rendering and receiving input
    // but never reaches the screen, so no transparent X window can ever
    // flicker over the video. Its snapshots are composited in GL later.
    auto* controlsWindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(controlsWindow), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(controlsWindow), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(controlsWindow), GDK_WINDOW_TYPE_HINT_UTILITY);
    gtk_widget_set_app_paintable(controlsWindow, TRUE);
    if (auto* rgbaVisual = gdk_screen_get_rgba_visual(gtk_widget_get_screen(controlsWindow))) {
        gtk_widget_set_visual(controlsWindow, rgbaVisual);
    }
    gtk_container_add(GTK_CONTAINER(controlsWindow), GTK_WIDGET(webView));
    // The page must ask the X server for pointer/keyboard events once it is a
    // child of a foreign (AWT) parent (mirrors NuvioDesktop's overlay host).
    gtk_widget_add_events(
        controlsWindow,
        GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
        GDK_SCROLL_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK |
        GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK | GDK_FOCUS_CHANGE_MASK);
    gtk_widget_realize(controlsWindow);
    Window controlsXid = 0;
    {
        GdkWindow* controlsGdkWin = gtk_widget_get_window(controlsWindow);
        if (controlsGdkWin && GDK_IS_X11_WINDOW(controlsGdkWin)) {
            GdkDisplay* gdkDisplay = gdk_window_get_display(controlsGdkWin);
            Display* dpy = GDK_WINDOW_XDISPLAY(controlsGdkWin);
            // Size to the host canvas BEFORE the redirect: at this point the
            // frame clock is still live, so gtk_window_resize commits and GTK
            // remembers the real size instead of the default request size.
            int initScale = gdk_window_get_scale_factor(controlsGdkWin);
            if (initScale < 1) initScale = 1;
            gtk_window_resize(GTK_WINDOW(controlsWindow),
                              std::max(width / initScale, 1),
                              std::max(height / initScale, 1));
            // Reparent under the AWT canvas through GDK so pointer events are
            // dispatched to the page (raw XReparentWindow would not tell GDK
            // about the new parent).
            GdkWindow* hostGdk = gdk_x11_window_foreign_new_for_display(gdkDisplay, hostWindow);
            if (hostGdk) {
                gdk_window_reparent(controlsGdkWin, hostGdk, 0, 0);
            } else {
                XReparentWindow(dpy, GDK_WINDOW_XID(controlsGdkWin), hostWindow, 0, 0);
            }
            gtk_widget_show_all(controlsWindow);
            gdk_window_raise(controlsGdkWin);
            controlsXid = GDK_WINDOW_XID(controlsGdkWin);
            int compEventBase = 0, compErrorBase = 0;
            if (XCompositeQueryExtension(dpy, &compEventBase, &compErrorBase)) {
                XCompositeRedirectWindow(dpy, controlsXid, CompositeRedirectManual);
                LOG_TO_FILE("[NativeBridge:Linux] controls window composite-redirected offscreen 0x"
                    << std::hex << controlsXid);
            } else {
                LOG_TO_FILE("[NativeBridge:Linux] XComposite NOT available; controls window remains mapped");
            }
            // Redirected windows are never presented, so GDK's event-compression
            // frame-clock flush stalls on some WMs and pointer motion piles up
            // undelivered. Deliver motion immediately.
            gdk_window_set_event_compression(controlsGdkWin, FALSE);
            GdkWindow* wvWin = gtk_widget_get_window(GTK_WIDGET(webView));
            if (wvWin && wvWin != controlsGdkWin) {
                gdk_window_set_event_compression(wvWin, FALSE);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        g_plug = plug;
        g_playerOverlay = playerOverlay;
        g_glArea = GTK_GL_AREA(glArea);
        g_vlcArea = vlcArea;
        g_webView = webView;
        g_controlsWindow = controlsWindow;
        g_controlsXid = controlsXid;
        g_controlsVisible = true;
        g_renderWindowId = renderWindowId;
        g_containerWindowId = reparented ? renderWindowId : 0;
        g_hostWindowId = hostWindow;
        g_lastOverlayWidth = 0;
        g_lastOverlayHeight = 0;
        g_lastOverlayX = 0;
        g_lastOverlayY = 0;
        g_lastOverlayExternal = false;
        g_overlayVisible = false;
        g_mainContext = g_main_context_ref(context);
        g_mainLoop = loop;
        g_x11EmbeddingAvailable = reparented && renderWindowId != 0;
        g_teardownRequested = false;
        g_initComplete = true;
    }

    // The AWT Canvas may already have a real X11 size when the GTK bridge is
    // created, but Compose is not required to emit a later componentResized
    // event. Resolve that geometry from the native host immediately so the
    // embedded surface is not left hidden waiting for an event that may never
    // arrive. Later AWT resize events still update it normally.
    alignGtkOverlay(std::max(width, 1), std::max(height, 1));
    if (g_lastOverlayWidth > 0 && g_lastOverlayHeight > 0) {
        scheduleGtkOverlayShow();
    }

    g_initCv.notify_one();
    LOG_TO_FILE("[NativeBridge:Linux] GTK overlay initialized; MPV Render API surface="
        << renderWindowId << ", WebKit overlay=GTK, embedded=" << (reparented ? "yes" : "no"));

    // Flicker-free overlay: start the snapshot compositor (gated by
    // controlsVisibility from the web UI).
    startCompositeTimer();

    g_main_loop_run(loop);

    // The loop can only end during process shutdown or a future explicit
    // native shutdown path. Invalidate queued render/WebKit callbacks before
    // touching the GTK tree so they cannot observe partially destroyed state.
    g_teardownRequested = true;
    stopMpvSyncOnGtk();
    stopVlcOnGtk();
    stopCompositeTimer();
    if (g_overlayShowSource != 0) {
        g_source_remove(g_overlayShowSource);
        g_overlayShowSource = 0;
    }
    detachMpvRenderOnGtk();
    if (g_webView) {
        webkit_web_view_stop_loading(g_webView);
    }
    gtk_widget_hide(plug);
    {
        // Invalidate all GTK pointers before destroying the widget tree. Any
        // already queued callback will now fail its guarded pointer check.
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        g_webView = nullptr;
        g_plug = nullptr;
        g_playerOverlay = nullptr;
        g_glArea = nullptr;
        g_vlcArea = nullptr;
        g_controlsWindow = nullptr;
        g_controlsXid = 0;
        g_x11EmbeddingAvailable = false;
        g_renderWindowId = 0;
        g_containerWindowId = 0;
        g_hostWindowId = 0;
        g_lastOverlayWidth = 0;
        g_lastOverlayHeight = 0;
        g_lastOverlayX = 0;
        g_lastOverlayY = 0;
        g_lastOverlayExternal = false;
        g_overlayVisible = false;
    }
    if (controlsWindow) {
        if (controlsXid != 0) {
            XCompositeUnredirectWindow(xDisplay, controlsXid, CompositeRedirectManual);
        }
        gtk_widget_destroy(controlsWindow);
    }
    if (plug) {
        // gtk_widget_destroy() invalidates the GtkWidget immediately, so keep
        // the display handle before destroying the plug. The X11 flush/sync
        // below must happen after destruction but can no longer query `plug`.
        GdkDisplay* display = gtk_widget_get_display(plug);
        gtk_widget_destroy(plug);
        if (display) {
            // WebKit's accelerated X11 compositor can leave Render requests
            // queued after the widget is hidden. Drain them before the AWT
            // host or the next player's GTK surface can reuse those XIDs.
            gdk_display_flush(display);
            gdk_display_sync(display);
        }
    }
    if (parkingWindow != 0) {
        XDestroyWindow(xDisplay, parkingWindow);
        XFlush(xDisplay);
        g_parkingWindowId = 0;
    }

    {
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        if (g_mainContext) {
            g_main_context_unref(g_mainContext);
            g_mainContext = nullptr;
        }
        g_mainLoop = nullptr;
    }
    g_main_loop_unref(loop);
}

} // namespace

void postUiTask(std::function<void()> task) {
    invokeOnGtkThread(std::move(task));
}

void processUiTasks() {}

void startMpvSyncPlatform() {
    postUiTask(startMpvSyncOnGtk);
}

void stopMpvSyncPlatform() {
    postUiTask(stopMpvSyncOnGtk);
}

extern "C" {

JNIEXPORT jlong JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_initWebView(
    JNIEnv*, jobject, jlong hostWindowId, jint width, jint height)
{
    std::lock_guard<std::mutex> transitionLock(g_lifecycleTransitionMutex);
    std::unique_lock<std::mutex> lock(g_lifecycleMutex);

    // Reuse the already-running GTK/WebKit tree. Recreating WebKitGTK and
    // reparenting a fresh X11 plug after every player close is the unstable
    // transition that caused the second-open crash.
    if (g_uiThread.joinable() && g_mainLoop != nullptr && g_initComplete) {
        lock.unlock();

        auto completed = std::make_shared<std::promise<bool>>();
        auto result = completed->get_future();
        invokeOnGtkThread([hostWindowId, width, height, completed] {
            bool attached = false;
            if (g_plug && g_renderWindowId != 0) {
                parkGtkOverlayForReuseOnGtk();
                g_hostWindowId = static_cast<Window>(hostWindowId);
                g_containerWindowId = 0;
                attached = reparentGtkPlugToHost(
                    g_renderWindowId,
                    static_cast<Window>(hostWindowId),
                    std::max(width, 1),
                    std::max(height, 1)
                );
                g_x11EmbeddingAvailable = attached;
                if (attached) {
                    alignGtkOverlay(std::max(width, 1), std::max(height, 1));
                    if (g_lastOverlayWidth > 0 && g_lastOverlayHeight > 0) {
                        scheduleGtkOverlayShow();
                    }
                    LOG_TO_FILE("[NativeBridge:Linux] Reattached reusable GTK/WebKit surface to new AWT Canvas");
                }
            }
            completed->set_value(attached);
        });

        if (result.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            LOG_TO_FILE("[NativeBridge:Linux] Timed out while reattaching the reusable GTK surface");
            return 0;
        }
        return result.get() ? static_cast<jlong>(g_renderWindowId) : 0;
    }

    if (g_uiThread.joinable()) {
        if (g_mainLoop != nullptr || !g_initComplete) {
            return g_x11EmbeddingAvailable ? static_cast<jlong>(g_renderWindowId) : 0;
        }
        lock.unlock();
        g_uiThread.join();
        lock.lock();
    }

    g_initComplete = false;
    g_x11EmbeddingAvailable = false;
    lock.unlock();

    g_uiThread = std::thread(runGtkThread, static_cast<Window>(hostWindowId), width, height);

    lock.lock();
    g_initCv.wait(lock, [] { return g_initComplete; });
    const bool available = g_x11EmbeddingAvailable;
    lock.unlock();
    // Linux uses the GTK overlay and MPV Render API; the returned XID is kept
    // only for diagnostics and is not passed to mpv as a "wid".
    return available ? static_cast<jlong>(g_renderWindowId) : 0;
}

JNIEXPORT jboolean JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_attachMpvRender(
    JNIEnv*, jobject, jlong handle)
{
    auto completed = std::make_shared<std::promise<bool>>();
    auto result = completed->get_future();
    postUiTask([handle, completed] {
        completed->set_value(attachMpvRenderOnGtk(reinterpret_cast<mpv_handle*>(handle)));
    });
    if (result.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        LOG_TO_FILE("[NativeBridge:Linux] Timed out while attaching MPV Render API");
        return JNI_FALSE;
    }
    return result.get() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_startVlc(
    JNIEnv* env,
    jobject,
    jstring url,
    jstring title,
    jstring userAgent,
    jstring referer,
    jlong startMs)
{
    const std::string urlValue = toUtf8(env, url);
    const std::string titleValue = toUtf8(env, title);
    const std::string userAgentValue = toUtf8(env, userAgent);
    const std::string refererValue = toUtf8(env, referer);
    auto completed = std::make_shared<std::promise<bool>>();
    auto result = completed->get_future();
    postUiTask([urlValue, titleValue, userAgentValue, refererValue, startMs, completed] {
        completed->set_value(startVlcOnGtk(urlValue, titleValue, userAgentValue, refererValue, startMs));
    });
    if (result.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        LOG_TO_FILE("[NativeBridge:Linux] Timed out while starting embedded VLC");
        return JNI_FALSE;
    }
    return result.get() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_stopVlc(
    JNIEnv*, jobject)
{
    postUiTask(stopVlcOnGtk);
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_pauseVlc(
    JNIEnv*, jobject)
{
    postUiTask([] {
        std::lock_guard<std::mutex> vlcLock(g_vlcMutex);
        if (g_vlcPlayer && g_vlcPlayerPause) g_vlcPlayerPause(g_vlcPlayer);
    });
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_playVlc(
    JNIEnv*, jobject)
{
    postUiTask([] {
        std::lock_guard<std::mutex> vlcLock(g_vlcMutex);
        if (g_vlcPlayer && g_vlcPlayerPlay) g_vlcPlayerPlay(g_vlcPlayer);
    });
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_seekVlc(
    JNIEnv*, jobject, jlong positionMs)
{
    postUiTask([positionMs] {
        std::lock_guard<std::mutex> vlcLock(g_vlcMutex);
        if (g_vlcPlayer && g_vlcPlayerSetTime) {
            g_vlcPlayerSetTime(g_vlcPlayer, std::max<jlong>(0, positionMs));
        }
    });
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_setVlcVolume(
    JNIEnv*, jobject, jint volume)
{
    postUiTask([volume] {
        std::lock_guard<std::mutex> vlcLock(g_vlcMutex);
        if (g_vlcPlayer && g_vlcAudioSetVolume) {
            g_vlcAudioSetVolume(g_vlcPlayer, std::clamp(static_cast<int>(volume), 0, 200));
        }
    });
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_setVlcRate(
    JNIEnv*, jobject, jfloat rate)
{
    postUiTask([rate] {
        std::lock_guard<std::mutex> vlcLock(g_vlcMutex);
        if (g_vlcPlayer && g_vlcPlayerSetRate) {
            g_vlcPlayerSetRate(g_vlcPlayer, rate > 0.05f ? rate : 1.0f);
        }
    });
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_setVlcMute(
    JNIEnv*, jobject, jboolean muted)
{
    postUiTask([muted] {
        std::lock_guard<std::mutex> vlcLock(g_vlcMutex);
        if (g_vlcPlayer && g_vlcAudioSetMute) g_vlcAudioSetMute(g_vlcPlayer, muted ? 1 : 0);
    });
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_startVlcSync(
    JNIEnv*, jobject)
{
    postUiTask(startVlcSyncOnGtk);
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_stopVlcSync(
    JNIEnv*, jobject)
{
    postUiTask(stopVlcSyncOnGtk);
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_destroyWebView(
    JNIEnv*, jobject)
{
    // This is a per-player reset, not a process-wide GTK/WebKit shutdown.
    // Keep one native surface alive and park it between sessions; the next
    // player will reparent it to its new AWT Canvas in initWebView().
    std::lock_guard<std::mutex> transitionLock(g_lifecycleTransitionMutex);
    std::lock_guard<std::mutex> destroyLock(g_destroyMutex);

    GMainLoop* loop = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        loop = g_mainLoop;
    }
    if (!loop) return;

    auto completed = std::make_shared<std::promise<void>>();
    auto result = completed->get_future();
    invokeOnGtkThread([completed] {
        parkGtkOverlayForReuseOnGtk();
        completed->set_value();
    });

    if (result.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        LOG_TO_FILE("[NativeBridge:Linux] Timed out while parking the reusable GTK surface");
        return;
    }
    LOG_TO_FILE("[NativeBridge:Linux] GTK/WebKit surface parked for reuse");
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_setFullscreen(
    JNIEnv*, jobject, jlong windowId, jboolean fullscreen, jint x, jint y, jint width, jint height)
{
    const auto window = static_cast<Window>(windowId);
    if (window == 0) return;

    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        LOG_TO_FILE("[NativeBridge:Linux] Could not open X11 display for fullscreen");
        return;
    }

    const Window root = DefaultRootWindow(display);
    const Atom wmState = XInternAtom(display, "_NET_WM_STATE", False);
    const Atom fullscreenAtom = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    const long stateAction = fullscreen == JNI_TRUE ? 1L : 0L; // ADD / REMOVE

    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = wmState;
    event.xclient.format = 32;
    event.xclient.data.l[0] = stateAction;
    event.xclient.data.l[1] = static_cast<long>(fullscreenAtom);
    event.xclient.data.l[2] = 0;
    event.xclient.data.l[3] = 1; // source indication: normal application
    event.xclient.data.l[4] = 0;

    XSendEvent(
        display,
        root,
        False,
        SubstructureRedirectMask | SubstructureNotifyMask,
        &event
    );
    XSync(display, False);

    if (fullscreen == JNI_TRUE) {
        const int targetWidth = width > 0 ? width : DisplayWidth(display, DefaultScreen(display));
        const int targetHeight = height > 0 ? height : DisplayHeight(display, DefaultScreen(display));
        XMoveResizeWindow(
            display,
            window,
            x,
            y,
            static_cast<unsigned int>(targetWidth),
            static_cast<unsigned int>(targetHeight)
        );
        XRaiseWindow(display, window);
    }

    XFlush(display);
    LOG_TO_FILE("[NativeBridge:Linux] X11 fullscreen "
        << (fullscreen == JNI_TRUE ? "entered" : "exited")
        << " window=" << window
        << " geometry=" << x << "," << y << " " << width << "x" << height);
    XCloseDisplay(display);
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_applyWindowChrome(
    JNIEnv*, jobject, jlong, jboolean, jint, jint, jint)
{
    // Linux window decorations are managed by the active compositor.
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_setPipSubclass(
    JNIEnv*, jobject, jlong, jboolean)
{
}

JNIEXPORT jboolean JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_setPipWindow(
    JNIEnv*, jobject, jlong windowId, jboolean pip, jint x, jint y, jint width, jint height)
{
    const auto window = static_cast<Window>(windowId);
    if (window == 0) return JNI_FALSE;

    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        LOG_TO_FILE("[NativeBridge:Linux] Could not open X11 display for PiP");
        return JNI_FALSE;
    }

    const Window root = DefaultRootWindow(display);
    const Atom wmState = XInternAtom(display, "_NET_WM_STATE", False);
    const Atom aboveAtom = XInternAtom(display, "_NET_WM_STATE_ABOVE", False);
    const Atom fullscreenAtom = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    const long stateAction = pip == JNI_TRUE ? 1L : 0L; // ADD / REMOVE

    auto sendState = [&](Atom atom, long action) {
        XEvent event{};
        event.xclient.type = ClientMessage;
        event.xclient.window = window;
        event.xclient.message_type = wmState;
        event.xclient.format = 32;
        event.xclient.data.l[0] = action;
        event.xclient.data.l[1] = static_cast<long>(atom);
        event.xclient.data.l[2] = 0;
        event.xclient.data.l[3] = 1;
        event.xclient.data.l[4] = 0;
        XSendEvent(display, root, False,
            SubstructureRedirectMask | SubstructureNotifyMask, &event);
    };

    if (pip == JNI_TRUE) {
        // PiP must never remain in EWMH fullscreen state. The window manager
        // owns the state transition; the explicit geometry is the X11 fallback
        // used by Treeland/XWayland when the configure arrives asynchronously.
        sendState(fullscreenAtom, 0L);
        sendState(aboveAtom, 1L);
        XSync(display, False);
        if (width > 0 && height > 0) {
            XMoveResizeWindow(display, window, x, y,
                static_cast<unsigned int>(width), static_cast<unsigned int>(height));
        }
        XMapRaised(display, window);
        XRaiseWindow(display, window);
    } else {
        sendState(aboveAtom, 0L);
        XSync(display, False);
    }

    XFlush(display);
    LOG_TO_FILE("[NativeBridge:Linux] PiP "
        << (pip == JNI_TRUE ? "entered" : "exited")
        << " window=" << window << " geometry="
        << x << "," << y << " " << width << "x" << height);
    XCloseDisplay(display);
    return JNI_TRUE;
}

static void sendX11MoveResizeRequest(Window window, int direction) {
    if (window == 0) return;
    Display* display = XOpenDisplay(nullptr);
    if (!display) return;

    const Window root = DefaultRootWindow(display);
    Window child = 0;
    int rootX = 0;
    int rootY = 0;
    int winX = 0;
    int winY = 0;
    unsigned int mask = 0;
    XQueryPointer(display, root, &child, &child, &rootX, &rootY, &winX, &winY, &mask);

    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = XInternAtom(display, "_NET_WM_MOVERESIZE", False);
    event.xclient.format = 32;
    event.xclient.data.l[0] = rootX;
    event.xclient.data.l[1] = rootY;
    event.xclient.data.l[2] = direction;
    event.xclient.data.l[3] = Button1;
    event.xclient.data.l[4] = 1;
    XUngrabPointer(display, CurrentTime);
    XSendEvent(display, root, False,
        SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(display);
    XCloseDisplay(display);
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_startWindowDrag(
    JNIEnv*, jobject, jlong windowId)
{
    sendX11MoveResizeRequest(static_cast<Window>(windowId), 8);
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_startWindowResize(
    JNIEnv*, jobject, jlong windowId, jint direction)
{
    sendX11MoveResizeRequest(static_cast<Window>(windowId), static_cast<int>(direction));
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_resizeWebView(
    JNIEnv*, jobject, jint width, jint height)
{
    postUiTask([width, height] {
        if (g_teardownRequested || !g_plug) return;
        if (width <= 0 || height <= 0) {
            if (g_overlayShowSource != 0) {
                g_source_remove(g_overlayShowSource);
                g_overlayShowSource = 0;
            }
            gtk_widget_hide(g_plug);
            g_overlayVisible = false;
            if (g_containerWindowId != 0) {
                auto* plugWindow = gtk_widget_get_window(g_plug);
                if (plugWindow) {
                    auto* display = gdk_window_get_display(plugWindow);
                    if (display && GDK_IS_X11_DISPLAY(display)) {
                        XUnmapWindow(
                            gdk_x11_display_get_xdisplay(display),
                            g_containerWindowId
                        );
                    }
                }
            }
            return;
        }
        alignGtkOverlay(width, height);
        // Flicker-free overlay: keep the offscreen controls window matched to
        // the canvas size so snapshots and input hit-testing align with the
        // video. A composite-redirected window is never presented, so its GTK
        // frame clock stalls and gtk_window_resize never lands; resize the X
        // window directly and force the widget allocation synchronously.
        if (g_controlsWindow && width > 0 && height > 0) {
            LOG_TO_FILE("[NativeBridge:Linux] controls resize to " << width << "x" << height);
            GdkWindow* cwGdk = gtk_widget_get_window(g_controlsWindow);
            if (cwGdk) {
                int scale = gdk_window_get_scale_factor(cwGdk);
                if (scale < 1) scale = 1;
                gdk_window_resize(cwGdk, width / scale, height / scale);
            }
            GtkAllocation alloc = {0, 0, width, height};
            gtk_widget_size_allocate(g_controlsWindow, &alloc);
            if (g_webView) gtk_widget_size_allocate(GTK_WIDGET(g_webView), &alloc);
        }
        if (!g_overlayVisible) scheduleGtkOverlayShow();
    });
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_focusWebView(
    JNIEnv*, jobject)
{
    postUiTask([] {
        if (g_teardownRequested || !g_plug) return;
        // Focusing the WebView is requested from several paths (UI-ready,
        // canvas focus, and player messages). Re-showing the whole GTK tree
        // on each request can make the X11 child surface flash or be
        // re-composited above the first MPV frame. Reveal it only once; later
        // focus requests should only move keyboard focus.
        // UI-ready can arrive before AWT delivers the first real Canvas
        // size. Do not reveal the plug at its initialization size (often
        // 1x1); resizeWebView() will schedule the first reveal once layout
        // has supplied a usable geometry.
        if (!g_overlayVisible && g_lastOverlayWidth > 0 && g_lastOverlayHeight > 0) {
            gtk_widget_show_all(g_plug);
            g_overlayVisible = true;
            if (g_containerWindowId != 0) {
                auto* plugWindow = gtk_widget_get_window(g_plug);
                if (plugWindow) {
                    auto* display = gdk_window_get_display(plugWindow);
                    if (display && GDK_IS_X11_DISPLAY(display)) {
                        XMapWindow(
                            gdk_x11_display_get_xdisplay(display),
                            g_containerWindowId
                        );
                    }
                }
            }
        }
        if (g_webView) gtk_widget_grab_focus(GTK_WIDGET(g_webView));
    });
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_executeScript(
    JNIEnv* env, jobject, jstring script)
{
    const auto value = toUtf8(env, script);
    postUiTask([value] {
        runJavascript(value);
    });
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_loadUrl(
    JNIEnv* env, jobject, jstring url)
{
    const auto value = toUtf8(env, url);
    postUiTask([value] {
        if (!g_teardownRequested && g_webView && !value.empty()) {
            webkit_web_view_load_uri(g_webView, value.c_str());
        }
    });
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_openDevTools(
    JNIEnv*, jobject)
{
    postUiTask([] {
        if (!g_teardownRequested && g_webView) {
            webkit_web_inspector_show(webkit_web_view_get_inspector(g_webView));
        }
    });
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_configureMpvLocale(
    JNIEnv*, jobject)
{
    setlocale(LC_NUMERIC, "C");
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_postMessage(
    JNIEnv* env, jobject, jstring message)
{
    const auto value = toUtf8(env, message);
    postUiTask([value] { runJavascript(makeDispatchScript(value)); });
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_warmupWebView2(
    JNIEnv*, jobject, jstring)
{
    // WebKitGTK is initialized with the player instance; there is no WebView2 warmup on Linux.
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_shutdownWebView2Warmup(
    JNIEnv*, jobject)
{
}

} // extern "C"

#endif
