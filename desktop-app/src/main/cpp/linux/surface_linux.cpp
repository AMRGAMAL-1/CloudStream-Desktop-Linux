#include "../include/player_bridge_common.h"

#ifndef _WIN32

#include <gtk/gtk.h>
#include <gtk/gtkx.h>
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>
#include <webkit2/webkit2.h>
extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <jsc/jsc.h>

#include <algorithm>
#include <cstring>
#include <strings.h>
#include <cstdarg>
#include <dlfcn.h>
#include <codecvt>
#include <clocale>
#include <exception>
#include <future>
#include <filesystem>
#include <locale>
#include <map>
#include <memory>
#include <sstream>
#include <string_view>

namespace {

GtkWidget* g_plug = nullptr;
GtkWidget* g_playerOverlay = nullptr;
GtkGLArea* g_glArea = nullptr;
GtkWidget* g_vlcArea = nullptr;
bool g_glContextProbed = false;   // one-time GL profile probe (root-fix phase 0.2)
WebKitWebView* g_webView = nullptr;
// Flicker-free overlay (root fix): the WebKit controls page lives in a
// separate, composite-redirected (offscreen) GTK toplevel. Its snapshots are
// composited as a GL texture inside the mpv render pass (renderMpvFrame), so
// no transparent X window ever stacks over the video on X11/XWayland.
GtkWidget* g_controlsWindow = nullptr;
Window g_controlsXid = 0;
std::atomic<bool> g_controlsVisible{true};   // gate: web UI controls currently shown
guint g_compositeTimer = 0;      // size-sync + snapshot tick while active
// Reused GL texture for the controls snapshot (avoids gen/delete churn).
GLuint g_overlayTex = 0;
int g_overlayTexW = 0;
int g_overlayTexH = 0;
// Reused GL texture for VLC video frames (same compositing pass as mpv, so
// VLC gets the identical vsynced overlay pipeline instead of software cairo).
GLuint g_videoTex = 0;
int g_videoTexW = 0;
int g_videoTexH = 0;
// Timestamp (µs, monotonic) of the last mpv render update. While mpv is
// buffering/probing it produces no frames, so no renders get queued and the
// controls/probing overlay would never composite (black canvas). compositeTick
// force-queues renders while the overlay is visible and mpv is idle.
std::atomic<gint64> g_lastMpvUpdateUs{0};
static gint64 g_lastRenderDoneUs = 0;
// Timestamp (µs, monotonic) of the last VLC video frame. Same idle problem as
// mpv: with no frames nothing queues a repaint and the overlay never shows.
std::atomic<gint64> g_lastVlcFrameUs{0};
// Frames actually composited to the screen by the VLC GL path. If the decoder
// produces frames (g_lastVlcFrameUs fresh) but nothing is ever presented, the
// vout/converter chain is broken for this geometry (observed: "Failed to
// create video converter" + recursion errors) and Kotlin falls back to mpv.
std::atomic<long long> g_vlcPresentedFrames{0};
// Snapshot pipeline state: async WebKit snapshot -> GL texture.
bool g_snapInFlight = false;
guint g_snapGen = 0;
// Serial number of the stored snapshot content. Lets the GL draw skip
// re-uploading an unchanged snapshot (pointer reuse after free would fool a
// pointer comparison; a serial cannot).
static unsigned long long g_snapSerial = 0;
guint g_snapWaitTicks = 0;
GCancellable* g_snapCancel = nullptr;
cairo_surface_t* g_snapSurf = nullptr;
gint64 g_lastSnapReqUs = 0;
gint64 g_lastSnapDoneUs = 0;   // latest controls snapshot (premultiplied ARGB32)
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
// Render contexts keyed by mpv handle: player sessions overlap transiently
// during switches (old disposing while new attaches). A singleton let a new
// attach clobber the old context — leaking it still-attached to the old
// handle, whose terminate_destroy then aborts the process
// ("Broken API use: mpv_render_context_free() not called").
// g_activeMpvRenderContext always tracks the newest live context (the
// visible session's); render callbacks never touch a stale one.
// Render contexts keyed by mpv handle: player sessions overlap transiently
// during switches (old disposing while new attaches). A singleton let a new
// attach clobber the old context — leaking it still-attached to the old
// handle, whose terminate_destroy then aborts the process
// ("Broken API use: mpv_render_context_free() not called").
// g_activeMpvRenderContext always tracks the newest live context (the
// visible session's); render callbacks never touch a stale one.
std::map<mpv_handle*, mpv_render_context*> g_mpvRenderContexts;
mpv_render_context* g_activeMpvRenderContext = nullptr;

static mpv_render_context* mpvActiveContext();
static void mpvAttachContext(mpv_handle* handle, mpv_render_context* renderContext);
static void mpvDetachContext(mpv_handle* handle);
static void mpvDetachAllContexts();
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
using libvlc_audio_set_delay_fn = int (*)(libvlc_media_player_t*, long long);
using libvlc_audio_get_delay_fn = long long (*)(libvlc_media_player_t*);
using libvlc_audio_set_track_fn = int (*)(libvlc_media_player_t*, int);
using libvlc_audio_get_track_fn = int (*)(libvlc_media_player_t*);
using libvlc_audio_get_track_count_fn = int (*)(libvlc_media_player_t*);
using libvlc_audio_get_track_description_fn = void* (*)(libvlc_media_player_t*);
using libvlc_video_set_spu_delay_fn = int (*)(libvlc_media_player_t*, long long);
using libvlc_video_get_spu_delay_fn = long long (*)(libvlc_media_player_t*);
using libvlc_video_set_spu_fn = int (*)(libvlc_media_player_t*, int);
using libvlc_video_get_spu_fn = int (*)(libvlc_media_player_t*);
using libvlc_video_get_spu_count_fn = int (*)(libvlc_media_player_t*);
using libvlc_video_get_spu_description_fn = void* (*)(libvlc_media_player_t*);
using libvlc_video_set_track_fn = int (*)(libvlc_media_player_t*, int);
using libvlc_video_get_track_fn = int (*)(libvlc_media_player_t*);
using libvlc_video_get_track_count_fn = int (*)(libvlc_media_player_t*);
using libvlc_video_set_aspect_ratio_fn = void (*)(libvlc_media_player_t*, const char*);
using libvlc_video_set_crop_geometry_fn = void (*)(libvlc_media_player_t*, const char*);
using libvlc_video_take_snapshot_fn = int (*)(libvlc_media_player_t*, unsigned, const char*, unsigned, unsigned);
using libvlc_media_player_add_slave_fn = int (*)(libvlc_media_player_t*, int, const char*, int);
using libvlc_track_description_list_release_fn = void (*)(void*);
using libvlc_media_player_get_chapter_count_fn = int (*)(libvlc_media_player_t*);
using libvlc_media_player_get_chapter_fn = int (*)(libvlc_media_player_t*);
using libvlc_media_player_set_chapter_fn = void (*)(libvlc_media_player_t*, int);
using libvlc_video_set_subtitle_text_scale_fn = void (*)(libvlc_media_player_t*, int);
using libvlc_audio_equalizer_new_fn = void* (*)();
using libvlc_audio_equalizer_release_fn = void (*)(void*);
using libvlc_audio_equalizer_set_preamp_fn = int (*)(void*, float);
using libvlc_audio_equalizer_set_amp_at_index_fn = int (*)(void*, float, unsigned);
using libvlc_audio_equalizer_get_preset_count_fn = unsigned (*)();
using libvlc_audio_equalizer_get_preset_name_fn = const char* (*)(unsigned);
using libvlc_audio_equalizer_new_from_preset_fn = void* (*)(unsigned);
using libvlc_media_player_set_equalizer_fn = int (*)(libvlc_media_player_t*, void*);
struct libvlc_log_t_;
using libvlc_log_cb_fn = void (*)(void*, int, const struct libvlc_log_t_*, const char*, va_list);
using libvlc_log_set_fn = void (*)(libvlc_instance_t*, libvlc_log_cb_fn, void*);
using libvlc_log_unset_fn = void (*)(libvlc_instance_t*);
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
    // Planar I420 storage: Y (stride*height) + U + V (uvStride*chromaH each).
    // Decoding straight to the decoder's native planar format keeps VLC's
    // chroma-converter chain (the observed crash site on odd geometries) out
    // of the picture; conversion to displayable BGRA happens below via
    // libswscale under this same mutex.
    std::vector<unsigned char> pixels;
    // Previous pixel generations, kept alive for VLC's display queue: a
    // picture locked before a format renegotiation is still referenced by the
    // display thread AFTER Unlock, so freeing/reallocating the storage
    // underneath it is a use-after-free (faults later inside VLC's plane
    // copy with a stale pointer). Bounded history; drained at retire.
    std::vector<std::vector<unsigned char>> retiredPixels;
    // Converted BGRA staging (width*4*height) for GL upload / cairo paint.
    std::vector<unsigned char> rgba;
    // Decoded-frame generation the staging was converted from. Repeated draws
    // of the same frame (forced renders while paused, overlay-only ticks)
    // reuse the conversion instead of re-running swscale on the GTK thread.
    unsigned long long rgbaFrameCount = 0;
    struct SwsContext* sws = nullptr;
    // Geometry the cached sws context was built for (revalidated per call).
    unsigned swsW = 0;
    unsigned swsH = 0;
    // Session geometry lock (see vlcVideoFormat): first real size wins.
    bool sizeLocked = false;
    unsigned lockedW = 0;
    unsigned lockedH = 0;
    unsigned width = 0;
    unsigned height = 0;
    unsigned stride = 0;
    unsigned uvStride = 0;
    unsigned chromaH = 0;
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
libvlc_audio_set_delay_fn g_vlcAudioSetDelay = nullptr;
libvlc_audio_get_delay_fn g_vlcAudioGetDelay = nullptr;
libvlc_audio_set_track_fn g_vlcAudioSetTrack = nullptr;
libvlc_audio_get_track_fn g_vlcAudioGetTrack = nullptr;
libvlc_audio_get_track_count_fn g_vlcAudioGetTrackCount = nullptr;
libvlc_audio_get_track_description_fn g_vlcAudioGetTrackDesc = nullptr;
libvlc_video_set_spu_delay_fn g_vlcVideoSetSpuDelay = nullptr;
libvlc_video_get_spu_delay_fn g_vlcVideoGetSpuDelay = nullptr;
libvlc_video_set_spu_fn g_vlcVideoSetSpu = nullptr;
libvlc_video_get_spu_fn g_vlcVideoGetSpu = nullptr;
libvlc_video_get_spu_count_fn g_vlcVideoGetSpuCount = nullptr;
libvlc_video_get_spu_description_fn g_vlcVideoGetSpuDesc = nullptr;
libvlc_video_set_track_fn g_vlcVideoSetTrack = nullptr;
libvlc_video_get_track_fn g_vlcVideoGetTrack = nullptr;
libvlc_video_get_track_count_fn g_vlcVideoGetTrackCount = nullptr;
libvlc_video_set_aspect_ratio_fn g_vlcVideoSetAspect = nullptr;
libvlc_video_set_crop_geometry_fn g_vlcVideoSetCrop = nullptr;
libvlc_video_take_snapshot_fn g_vlcVideoTakeSnapshot = nullptr;
libvlc_media_player_add_slave_fn g_vlcPlayerAddSlave = nullptr;
libvlc_track_description_list_release_fn g_vlcTrackDescRelease = nullptr;
libvlc_media_player_get_chapter_count_fn g_vlcPlayerGetChapterCount = nullptr;
libvlc_media_player_get_chapter_fn g_vlcPlayerGetChapter = nullptr;
libvlc_media_player_set_chapter_fn g_vlcPlayerSetChapter = nullptr;
libvlc_video_set_subtitle_text_scale_fn g_vlcVideoSetSubScale = nullptr;
libvlc_audio_equalizer_new_fn g_vlcEqNew = nullptr;
libvlc_audio_equalizer_release_fn g_vlcEqRelease = nullptr;
libvlc_audio_equalizer_set_preamp_fn g_vlcEqSetPreamp = nullptr;
libvlc_audio_equalizer_set_amp_at_index_fn g_vlcEqSetAmp = nullptr;
libvlc_audio_equalizer_get_preset_count_fn g_vlcEqPresetCount = nullptr;
libvlc_audio_equalizer_get_preset_name_fn g_vlcEqPresetName = nullptr;
libvlc_audio_equalizer_new_from_preset_fn g_vlcEqNewFromPreset = nullptr;
libvlc_media_player_set_equalizer_fn g_vlcPlayerSetEq = nullptr;
libvlc_log_set_fn g_vlcLogSet = nullptr;
libvlc_log_unset_fn g_vlcLogUnset = nullptr;
// Set when VLC's own log reports a broken display chain for the current
// geometry ("Failed to create video converter", filter recursion). Those
// sessions decode but can never present and historically end in a native
// vout crash at teardown — Kotlin falls back to mpv while it still can.
std::atomic<bool> g_vlcVoutBroken{false};
// Cached VLC subtitle style (set from Kotlin prefs before startVlc; freetype
// options are media-level and only apply at session start).
std::string g_vlcSubFont;
int g_vlcSubSizePx = 0;
std::string g_vlcSubColorHex;
int g_vlcSubBgOpacity = -1;
std::mutex g_vlcStyleMutex;
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
std::mutex g_vlcRetiredMutex;
// Serializes VLC player teardowns: rapid link/backend switches spawn one
// worker each, and overlapping playerStop/playerRelease calls plus retired
// frame reclamation race the previous session's still-joining decoder/vout
// threads (observed as memcpy faults in VLC's picture copy path at switch
// time). libvlc joins a player's threads inside stop/release, so running one
// teardown at a time makes frame reclamation provably safe.
std::mutex g_vlcTeardownMutex;

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

static mpv_render_context* mpvActiveContext() { return g_activeMpvRenderContext; }

static void mpvAttachContext(mpv_handle* handle, mpv_render_context* renderContext) {
    if (!handle || !renderContext) return;
    g_mpvRenderContexts[handle] = renderContext;
    g_activeMpvRenderContext = renderContext;
}

static void mpvDetachContext(mpv_handle* handle) {
    if (!handle) return;
    auto it = g_mpvRenderContexts.find(handle);
    if (it == g_mpvRenderContexts.end()) return;
    mpv_render_context* renderContext = it->second;
    g_mpvRenderContexts.erase(it);
    if (g_activeMpvRenderContext == renderContext) {
        g_activeMpvRenderContext = g_mpvRenderContexts.empty()
            ? nullptr : g_mpvRenderContexts.rbegin()->second;
    }
    if (renderContext && g_renderContextFree) {
        if (g_renderContextSetUpdateCallback) {
            g_renderContextSetUpdateCallback(renderContext, nullptr, nullptr);
        }
        g_renderContextFree(renderContext);
    }
}

static void mpvDetachAllContexts() {
    while (!g_mpvRenderContexts.empty()) {
        mpvDetachContext(g_mpvRenderContexts.begin()->first);
    }
    g_activeMpvRenderContext = nullptr;
}

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
            // compositor (Phase 3+): while hidden nothing is drawn over the
            // video and normal watching costs nothing.
            const std::string value = extractJsonString(payload, "value");
            g_controlsVisible = (value == "1");
            return;
        }
        if (eventType == "viewportDiag") {
            static bool viewportLogged = false;
            if (!viewportLogged) {
                viewportLogged = true;
                LOG_TO_FILE("[NativeBridge:Linux] page viewport: "
                    << extractJsonString(payload, "value"));
            }
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
    // Parity symbols (optional): missing entries only disable the matching
    // VLC control; every call site null-checks before use.
    LOAD_VLC_SYMBOL(g_vlcAudioSetDelay, "libvlc_audio_set_delay");
    LOAD_VLC_SYMBOL(g_vlcAudioGetDelay, "libvlc_audio_get_delay");
    LOAD_VLC_SYMBOL(g_vlcAudioSetTrack, "libvlc_audio_set_track");
    LOAD_VLC_SYMBOL(g_vlcAudioGetTrack, "libvlc_audio_get_track");
    LOAD_VLC_SYMBOL(g_vlcAudioGetTrackCount, "libvlc_audio_get_track_count");
    LOAD_VLC_SYMBOL(g_vlcAudioGetTrackDesc, "libvlc_audio_get_track_description");
    LOAD_VLC_SYMBOL(g_vlcVideoSetSpuDelay, "libvlc_video_set_spu_delay");
    LOAD_VLC_SYMBOL(g_vlcVideoGetSpuDelay, "libvlc_video_get_spu_delay");
    LOAD_VLC_SYMBOL(g_vlcVideoSetSpu, "libvlc_video_set_spu");
    LOAD_VLC_SYMBOL(g_vlcVideoGetSpu, "libvlc_video_get_spu");
    LOAD_VLC_SYMBOL(g_vlcVideoGetSpuCount, "libvlc_video_get_spu_count");
    LOAD_VLC_SYMBOL(g_vlcVideoGetSpuDesc, "libvlc_video_get_spu_description");
    LOAD_VLC_SYMBOL(g_vlcVideoSetTrack, "libvlc_video_set_track");
    LOAD_VLC_SYMBOL(g_vlcVideoGetTrack, "libvlc_video_get_track");
    LOAD_VLC_SYMBOL(g_vlcVideoGetTrackCount, "libvlc_video_get_track_count");
    LOAD_VLC_SYMBOL(g_vlcVideoSetAspect, "libvlc_video_set_aspect_ratio");
    LOAD_VLC_SYMBOL(g_vlcVideoSetCrop, "libvlc_video_set_crop_geometry");
    LOAD_VLC_SYMBOL(g_vlcVideoTakeSnapshot, "libvlc_video_take_snapshot");
    LOAD_VLC_SYMBOL(g_vlcPlayerAddSlave, "libvlc_media_player_add_slave");
    LOAD_VLC_SYMBOL(g_vlcTrackDescRelease, "libvlc_track_description_list_release");
    LOAD_VLC_SYMBOL(g_vlcPlayerGetChapterCount, "libvlc_media_player_get_chapter_count");
    LOAD_VLC_SYMBOL(g_vlcPlayerGetChapter, "libvlc_media_player_get_chapter");
    LOAD_VLC_SYMBOL(g_vlcPlayerSetChapter, "libvlc_media_player_set_chapter");
    LOAD_VLC_SYMBOL(g_vlcVideoSetSubScale, "libvlc_video_set_subtitle_text_scale");
    LOAD_VLC_SYMBOL(g_vlcEqNew, "libvlc_audio_equalizer_new");
    LOAD_VLC_SYMBOL(g_vlcEqRelease, "libvlc_audio_equalizer_release");
    LOAD_VLC_SYMBOL(g_vlcEqSetPreamp, "libvlc_audio_equalizer_set_preamp");
    LOAD_VLC_SYMBOL(g_vlcEqSetAmp, "libvlc_audio_equalizer_set_amp_at_index");
    LOAD_VLC_SYMBOL(g_vlcEqPresetCount, "libvlc_audio_equalizer_get_preset_count");
    LOAD_VLC_SYMBOL(g_vlcEqPresetName, "libvlc_audio_equalizer_get_preset_name");
    LOAD_VLC_SYMBOL(g_vlcEqNewFromPreset, "libvlc_audio_equalizer_new_from_preset");
    LOAD_VLC_SYMBOL(g_vlcPlayerSetEq, "libvlc_media_player_set_equalizer");
    LOAD_VLC_SYMBOL(g_vlcLogSet, "libvlc_log_set");
    LOAD_VLC_SYMBOL(g_vlcLogUnset, "libvlc_log_unset");

    LOAD_VLC_SYMBOL(g_vlcVideoSetCallbacks, "libvlc_video_set_callbacks");

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

// libvlc logger callback (anonymous-namespace C++ linkage): runs on VLC's
// internal logger thread, so it only formats into a stack buffer and flips an
// atomic. Never blocks, never allocates globals, never touches widgets.
static void vlcLogWatchCb(void*, int, const struct libvlc_log_t_*, const char* fmt, va_list args) {
    if (!fmt) return;
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, args);
    if (strstr(buf, "Failed to create video converter") ||
        strstr(buf, "video output creation failed") ||
        strstr(buf, "Too high level of recursion")) {
        g_vlcVoutBroken.store(true, std::memory_order_relaxed);
    }
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

// ---------------------------------------------------------------------------
// Modern GL overlay renderer (core-profile safe).
// The GtkGLArea context is CORE PROFILE (probe: "4.6 (Core Profile)"), where
// the legacy fixed-function pipeline (glBegin/glOrtho) does not exist and
// silently draws nothing — the cause of the previous black overlay. This
// renderer uses shaders + VBO, which work in any profile. All GL entry points
// are resolved at runtime via getGlProcAddress (the bridge never links libGL).
// ---------------------------------------------------------------------------

struct OverlayGl {
    bool ready = false;
    GLuint program = 0;
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint videoVbo = 0;
    GLint locTex = -1;
};
static OverlayGl g_overlayGl;

static GLuint compileShader(GLenum type, const char* source, const char* name) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        LOG_TO_FILE("[NativeBridge:Linux] overlay shader compile failed (" << name << "): " << log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// Full-screen textured quad (two triangles), UV 0..1 with (0,0) at bottom-left
// in GL convention; callers flip UVs when the source has a top-left origin.
static bool ensureOverlayProgram() {
    if (g_overlayGl.ready) return true;
        const char* vs = R"GLSL(#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }
)GLSL";
    const char* fs = R"GLSL(#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
void main() { fragColor = texture(uTex, vUV); }
)GLSL";
    GLuint vsh = compileShader(GL_VERTEX_SHADER, vs, "vertex");
    GLuint fsh = compileShader(GL_FRAGMENT_SHADER, fs, "fragment");
    if (!vsh || !fsh) return false;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vsh);
    glAttachShader(prog, fsh);
    glLinkProgram(prog);
    glDeleteShader(vsh);
    glDeleteShader(fsh);
    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        GLint len = 0;
        glGetProgramInfoLog(prog, sizeof(log) - 1, &len, log);
        LOG_TO_FILE("[NativeBridge:Linux] overlay program link failed: " << log);
        glDeleteShader(0);
        return false;
    }
    g_overlayGl.program = prog;
    g_overlayGl.locTex = glGetUniformLocation(prog, "uTex");

    // Full-screen quad (two triangles), positions in NDC. UVs are flipped so
    // texture row 0 (image top-left for cairo ARGB32) appears at the SCREEN
    // top-left, matching the video orientation in the FBO.
    const float verts[] = {
        -1.0f, -1.0f,  0.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 1.0f,
         1.0f,  1.0f,  1.0f, 0.0f,
        -1.0f, -1.0f,  0.0f, 1.0f,
         1.0f,  1.0f,  1.0f, 0.0f,
        -1.0f,  1.0f,  0.0f, 0.0f,
    };
    glGenVertexArrays(1, &g_overlayGl.vao);
    glBindVertexArray(g_overlayGl.vao);
    glGenBuffers(1, &g_overlayGl.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_overlayGl.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                                reinterpret_cast<void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glGenBuffers(1, &g_overlayGl.videoVbo);
    glBindVertexArray(0);
    g_overlayGl.ready = true;
    return true;
}

// (Re)bind the static fullscreen overlay vertices. The VLC video quad reuses
// the same VAO with its own VBO, so the overlay re-selects its buffer on
// every draw instead of relying on init-time state.
static void bindOverlayVerts() {
    glBindVertexArray(g_overlayGl.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_overlayGl.vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
}

// Draw a texture over the whole video surface. Called from renderMpvFrame with
// the GtkGLArea context current, AFTER mpv rendered the video. WebKit control
// snapshots are premultiplied -> (GL_ONE, GL_ONE_MINUS_SRC_ALPHA).
// UVs are flipped so (0,0) maps to the top-left of the screen for cairo-style
// sources.
static bool drawOverlayTexture(GLuint tex, int texW, int texH, bool premultiplied) {
    if (!g_overlayGl.ready && !ensureOverlayProgram()) return false;
    if (!tex || texW <= 0 || texH <= 0) return false;
    glUseProgram(g_overlayGl.program);
    if (g_overlayGl.locTex >= 0) glUniform1i(g_overlayGl.locTex, 0);
    glActiveTexture(GL_TEXTURE0);
    bindOverlayVerts();
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glEnable(GL_BLEND);
    if (premultiplied) {
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
    glUseProgram(0);
    return true;
}

// Phase 4: draw the REAL controls snapshot over the video. Called from
// renderMpvFrame with the GtkGLArea context current, AFTER mpv rendered the
// video. Grabs the latest snapshot under the lifecycle lock and references it
// so the GTK thread can safely replace it.
// NOTE (verified by runtime alpha probe): WebKit snapshots arrive as
// PREMULTIPLIED alpha (15126/15126 translucent pixels satisfied C <= A),
// so blend with (GL_ONE, GL_ONE_MINUS_SRC_ALPHA). The earlier straight-alpha
// assumption washed out translucent icons; cairo ARGB32 is premultiplied.
static bool drawControlsSnapshot(int width, int height) {
    cairo_surface_t* surf = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        if (g_snapSurf) surf = cairo_surface_reference(g_snapSurf);
    }
    if (!surf) return false;
    const int sw = cairo_image_surface_get_width(surf);
    const int sh = cairo_image_surface_get_height(surf);
    if (sw <= 0 || sh <= 0) {
        cairo_surface_destroy(surf);
        return false;
    }
    cairo_surface_flush(surf);
    // Reuse one GL texture across frames; realloc only when the snapshot
    // size changes. Per-frame gen/delete caused driver churn and stutter.
    // Skip the upload entirely when the stored snapshot did not change since
    // the last upload (static UI over playing video costs zero this way).
    static unsigned long long uploadedSerial = 0;
    static int uploadedW = 0;
    static int uploadedH = 0;
    if (g_overlayTex == 0) glGenTextures(1, &g_overlayTex);
    const bool freshTex = (g_overlayTexW == 0 && g_overlayTexH == 0);
    glBindTexture(GL_TEXTURE_2D, g_overlayTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    unsigned long long serial = 0;
    {
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        serial = g_snapSerial;
    }
    const unsigned char* pixels = cairo_image_surface_get_data(surf);
    if (freshTex || serial != uploadedSerial || sw != uploadedW || sh != uploadedH) {
        if (sw != g_overlayTexW || sh != g_overlayTexH) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw, sh, 0, GL_BGRA,
                         GL_UNSIGNED_BYTE, pixels);
            g_overlayTexW = sw;
            g_overlayTexH = sh;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sw, sh, GL_BGRA,
                            GL_UNSIGNED_BYTE, pixels);
        }
        uploadedSerial = serial;
        uploadedW = sw;
        uploadedH = sh;
    }
    const bool ok = drawOverlayTexture(g_overlayTex, sw, sh, true);
    cairo_surface_destroy(surf);
    (void)width; (void)height;
    return ok;
}

// VLC backend shares the mpv overlay pipeline: its CPU-decoded frames are
// uploaded as a GL texture and composited in the same GtkGLArea pass (same
// vsync, same snapshot overlay) instead of a software cairo repaint.
static bool vlcEnsureRgba(VlcFrameBuffer* frame);
static bool vlcVideoActive() {
    auto* frame = g_vlcFrame.load(std::memory_order_acquire);
    if (!frame) return false;
    std::lock_guard<std::mutex> lock(frame->mutex);
    return frame->active;
}

// Draw the latest VLC frame aspect-fit over black, then the shared controls
// snapshot on top. Called from renderMpvFrame with the GtkGLArea context
// current. Returns false when no VLC session owns the surface.
static bool drawVlcGlFrame(int width, int height) {
    if (!g_overlayGl.ready && !ensureOverlayProgram()) return false;
    auto* frame = g_vlcFrame.load(std::memory_order_acquire);
    if (!frame) return false;
    std::lock_guard<std::mutex> lock(frame->mutex);
    if (!frame->active || frame->width == 0 || frame->height == 0) return false;
    // Convert planar I420 to BGRA staging (same mutex guards the sws state).
    if (!vlcEnsureRgba(frame)) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        drawControlsSnapshot(width, height);
        return true;
    }
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    {
        const int vw = static_cast<int>(frame->width);
        const int vh = static_cast<int>(frame->height);
        if (g_videoTex == 0) glGenTextures(1, &g_videoTex);
        glBindTexture(GL_TEXTURE_2D, g_videoTex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (vw != g_videoTexW || vh != g_videoTexH) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, vw, vh, 0, GL_BGRA,
                         GL_UNSIGNED_BYTE, frame->rgba.data());
            g_videoTexW = vw;
            g_videoTexH = vh;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, vw, vh, GL_BGRA,
                            GL_UNSIGNED_BYTE, frame->rgba.data());
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // Aspect-fit quad in NDC; UVs follow the overlay convention so the
        // cairo top-left row lands on the screen top-left.
        const float s = std::min(
            static_cast<float>(width) / static_cast<float>(vw),
            static_cast<float>(height) / static_cast<float>(vh));
        const float dw = static_cast<float>(vw) * s / static_cast<float>(width);
        const float dh = static_cast<float>(vh) * s / static_cast<float>(height);
        const float verts[] = {
            -dw, -dh,  0.0f, 1.0f,
             dw, -dh,  1.0f, 1.0f,
             dw,  dh,  1.0f, 0.0f,
            -dw, -dh,  0.0f, 1.0f,
             dw,  dh,  1.0f, 0.0f,
            -dw,  dh,  0.0f, 0.0f,
        };
        glUseProgram(g_overlayGl.program);
        if (g_overlayGl.locTex >= 0) glUniform1i(g_overlayGl.locTex, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(g_overlayGl.vao);
        glBindBuffer(GL_ARRAY_BUFFER, g_overlayGl.videoVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                              reinterpret_cast<void*>(2 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glDisable(GL_BLEND);
        glBindTexture(GL_TEXTURE_2D, g_videoTex);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
        glUseProgram(0);
        g_vlcPresentedFrames.fetch_add(1, std::memory_order_relaxed);
    }
    drawControlsSnapshot(width, height);
    return true;
}

// ---- Offscreen controls window: size sync + frame-clock forcing -----------
// A composite-redirected (never-presented) window's GTK frame clock stalls:
// gtk_window_resize and CSS layout never apply on their own. This tick runs
// while the player is open, keeps the window matched to the REAL video host
// size (the 200x200 trap), and forces the UPDATE+LAYOUT phases so the page
// re-lays-out and its animations keep running.

struct SnapCtx {
    guint gen;
};

// Completion of the async controls snapshot: store the pixels for the GL
// thread. Runs on the GTK thread like the tick that issued it;
// renderMpvFrame consumes g_snapSurf on the GL thread.
void onOverlaySnapshot(GObject* src, GAsyncResult* res, gpointer data) {
    auto* ctx = static_cast<SnapCtx*>(data);
    const guint gen = ctx->gen;
    delete ctx;
    GError* err = nullptr;
    cairo_surface_t* surf =
        webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(src), res, &err);
    if (err) g_error_free(err);

    Window xidCopy = 0;
    {
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        if (g_teardownRequested) {
            if (surf) cairo_surface_destroy(surf);
            return;
        }
        xidCopy = g_controlsXid;
    }
    if (gen != g_snapGen) {
        // A watchdog reset invalidated this request; drop the late result.
        if (surf) cairo_surface_destroy(surf);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        g_snapInFlight = false;
        g_snapWaitTicks = 0;
    }
    if (!surf) {
        LOG_TO_FILE("[NativeBridge:Linux] snapshot completed with null surface");
        return;
    }
    if (cairo_image_surface_get_format(surf) != CAIRO_FORMAT_ARGB32 ||
        cairo_image_surface_get_width(surf) <= 0 ||
        cairo_image_surface_get_height(surf) <= 0) {
        LOG_TO_FILE("[NativeBridge:Linux] snapshot completed with invalid surface");
        cairo_surface_destroy(surf);
        return;
    }
    // Drop snapshots SMALLER than the window (stale mid-resize frame that
    // would stretch into a blurry postage stamp). Larger snapshots (e.g. the
    // page briefly laying out taller than the window) are accepted and drawn;
    // freezing the overlay is worse than a few percent of stretch.
    // DIAG (temporary): log XID liveness + accepted sizes to prove whether
    // completions land on reused surfaces.
    if (xidCopy != 0) {
        XWindowAttributes wa;
        const bool xidOk = XGetWindowAttributes(gdk_x11_display_get_xdisplay(gdk_display_get_default()),
                                                xidCopy, &wa);
        if (!xidOk) {
            LOG_TO_FILE("[NativeBridge:Linux] DIAG snapshot completion: controls XID dead");
        }
        if (xidOk &&
                (cairo_image_surface_get_width(surf) < wa.width ||
                 cairo_image_surface_get_height(surf) < wa.height)) {
                static guint dropCount = 0;
                if ((++dropCount % 60) == 1) {
                    LOG_TO_FILE("[NativeBridge:Linux] dropping stale-size snapshot "
                        << cairo_image_surface_get_width(surf) << "x"
                        << cairo_image_surface_get_height(surf) << " vs window "
                        << wa.width << "x" << wa.height << " (drop #" << dropCount << ")");
                }
                cairo_surface_destroy(surf);
                return;
        }
    }
    cairo_surface_flush(surf);
    // Alpha convention probe: for PREMULTIPLIED data every channel satisfies
    // C <= A; straight-alpha pixels routinely violate it. Retried until a
    // snapshot with real translucent content arrives (the first frames are
    // often fully opaque/blank), then logged once to settle which blend
    // equation the GL compositor must use.
    {
        static bool alphaProbed = false;
        static guint alphaTries = 0;
        if (!alphaProbed && alphaTries < 30) {
            const int sw = cairo_image_surface_get_width(surf);
            const int sh = cairo_image_surface_get_height(surf);
            const int stride = cairo_image_surface_get_stride(surf);
            const unsigned char* px = cairo_image_surface_get_data(surf);
            long semi = 0, consistent = 0, violated = 0;
            for (int y = 0; y < sh; y += 8) {
                for (int x = 0; x < sw; x += 8) {
                    const unsigned char* p = px + y * stride + x * 4;
                    const unsigned int b = p[0], g = p[1], r = p[2], a = p[3];
                    if (a > 8 && a < 250) {
                        ++semi;
                        if (r <= a && g <= a && b <= a) ++consistent;
                        else ++violated;
                    }
                }
            }
            if (semi > 200) {
                alphaProbed = true;
                LOG_TO_FILE("[NativeBridge:Linux] alpha probe: semi=" << semi
                    << " premultiplied-consistent=" << consistent
                    << " straight-violations=" << violated);
            } else {
                ++alphaTries;
            }
        }
    }
    {
        static int lastSnapW = 0, lastSnapH = 0;
        const int sw = cairo_image_surface_get_width(surf);
        const int sh = cairo_image_surface_get_height(surf);
        if (sw != lastSnapW || sh != lastSnapH) {
            lastSnapW = sw;
            lastSnapH = sh;
            LOG_TO_FILE("[NativeBridge:Linux] controls snapshot size now "
                << sw << "x" << sh);
        }
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        if (g_snapSurf) cairo_surface_destroy(g_snapSurf);
        g_snapSurf = surf;
        ++g_snapSerial;
        g_lastSnapDoneUs = g_get_monotonic_time();
    }
}

// Request one overlay snapshot, paired with a repaint invalidate. Called only
// from the render path (same GTK thread), so every snapshot is consumed by a
// composite exactly once: zero waste, self-throttling to the display rate.
// Throttled to ~15fps: fullscreen software snapshots + GL uploads every frame
// saturate weak iGPUs (stuttering video AND overlay). UI animation stays
// smooth at 15fps; the upload-skip in drawControlsSnapshot removes the rest.
static void requestOverlaySnapshot(GtkWidget* controlsWin, WebKitWebView* webView) {
    if (!controlsWin || !GTK_IS_WIDGET(controlsWin) || !webView || !WEBKIT_IS_WEB_VIEW(webView)) return;
    {
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        if (g_teardownRequested || g_webView != webView || g_snapInFlight) return;
        if (g_get_monotonic_time() - g_lastSnapReqUs < 66000) return;
        g_snapInFlight = true;
        g_snapWaitTicks = 0;
        if (!g_snapCancel) g_snapCancel = g_cancellable_new();
    }
    GdkWindow* visWin = gtk_widget_get_window(controlsWin);
    if (visWin) gdk_window_invalidate_rect(visWin, nullptr, FALSE);
    GCancellable* cancelCopy = nullptr;
    guint genCopy = 0;
    {
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        cancelCopy = g_snapCancel;
        genCopy = g_snapGen;
    }
    g_lastSnapReqUs = g_get_monotonic_time();
    webkit_web_view_get_snapshot(
        webView, WEBKIT_SNAPSHOT_REGION_VISIBLE,
        WEBKIT_SNAPSHOT_OPTIONS_TRANSPARENT_BACKGROUND,
        cancelCopy, onOverlaySnapshot, new SnapCtx{genCopy});
}

gboolean compositeTick(gpointer) {
    // Take GObject refs under lock so teardown (which nulls + destroys the
    // widgets on another path) cannot free them while this tick runs.
    GtkWidget* controlsWin = nullptr;
    WebKitWebView* webView = nullptr;
    GtkGLArea* glArea = nullptr;
    Window controlsXid = 0;
    Window hostId = 0;
    {
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        if (g_teardownRequested || !g_webView) return G_SOURCE_REMOVE;
        controlsWin = g_controlsWindow;
        webView = g_webView;
        glArea = g_glArea;
        controlsXid = g_controlsXid;
        hostId = g_hostWindowId;
        if (controlsWin && GTK_IS_WIDGET(controlsWin)) g_object_ref(controlsWin);
        else controlsWin = nullptr;
        if (webView && WEBKIT_IS_WEB_VIEW(webView)) g_object_ref(webView);
        else webView = nullptr;
        if (glArea && GTK_IS_GL_AREA(glArea)) g_object_ref(glArea);
        else glArea = nullptr;
        if (!webView) {
            if (controlsWin) g_object_unref(controlsWin);
            if (glArea) g_object_unref(glArea);
            return G_SOURCE_CONTINUE;
        }
    }

    if (controlsWin && controlsXid != 0 && hostId != 0 && GTK_IS_WIDGET(controlsWin)) {
        GdkWindow* cwGdk = gtk_widget_get_window(controlsWin);
        Display* dpy = gdk_x11_display_get_xdisplay(gdk_display_get_default());
        XWindowAttributes hostWa;
        if (cwGdk && dpy && XGetWindowAttributes(dpy, hostId, &hostWa) != 0 &&
            hostWa.width > 0 && hostWa.height > 0) {
            int scale = gdk_window_get_scale_factor(cwGdk);
            if (scale < 1) scale = 1;
            const int logicalW = hostWa.width / scale;
            const int logicalH = hostWa.height / scale;
            XWindowAttributes cwWa;
            // X window is in physical pixels; GTK allocation is in logical.
            const bool mismatch =
                XGetWindowAttributes(dpy, controlsXid, &cwWa) == 0 ||
                cwWa.width != hostWa.width || cwWa.height != hostWa.height;
            if (mismatch && GTK_IS_WIDGET(controlsWin) && WEBKIT_IS_WEB_VIEW(webView)) {
                XResizeWindow(dpy, controlsXid,
                              static_cast<unsigned int>(hostWa.width),
                              static_cast<unsigned int>(hostWa.height));
                XFlush(dpy);
                gdk_window_resize(cwGdk, logicalW, logicalH);
                gtk_window_resize(GTK_WINDOW(controlsWin), logicalW, logicalH);
                GtkAllocation alloc = {0, 0, logicalW, logicalH};
                gtk_widget_size_allocate(controlsWin, &alloc);
                gtk_widget_size_allocate(GTK_WIDGET(webView), &alloc);
                // A redirected window is never presented, so WebKit does not
                // notice the new size and keeps serving the OLD rendered
                // framebuffer: snapshots stay stale ("static image with wrong
                // dimensions"). Invalidate so the page actually repaints at
                // the new size on the next forced frame-clock cycle.
                gdk_window_invalidate_rect(cwGdk, nullptr, FALSE);
                GdkWindow* wvWin = gtk_widget_get_window(GTK_WIDGET(webView));
                if (wvWin && wvWin != cwGdk) {
                    gdk_window_invalidate_rect(wvWin, nullptr, FALSE);
                }
            }
        }
    }
    // Force the frame clock so the stalled redirected window actually applies
    // the layout above and keeps the page's CSS animations running. Skipped
    // while hidden: nobody composites then, and each forced phase costs a
    // full software repaint (fan noise for zero visible benefit).
    if (g_controlsVisible && controlsWin && GTK_IS_WIDGET(controlsWin)) {
        GdkWindow* fcWin = gtk_widget_get_window(controlsWin);
        if (fcWin) {
            GdkFrameClock* fc = gdk_window_get_frame_clock(fcWin);
            if (fc) {
                gdk_frame_clock_request_phase(
                    fc, static_cast<GdkFrameClockPhase>(
                            GDK_FRAME_CLOCK_PHASE_UPDATE |
                            GDK_FRAME_CLOCK_PHASE_LAYOUT));
            }
        }
        // The video plug is reparented/reshown on reveal, PiP, and fullscreen
        // changes; those paths can re-stack the plug ABOVE the controls
        // window, stealing its input. Re-raise rarely (not every ~300ms) to
        // avoid focus fights; visibility/resize paths handle the common case.
        static guint raiseCounter = 0;
        if (g_controlsVisible && ((++raiseCounter % 120) == 0)) {
            gdk_window_raise(fcWin);
        }
    }

    // Snapshot gate: while the controls are hidden, drop the last snapshot
    // so the GL thread draws nothing (normal watching costs zero).
    // Transition log is permanent lightweight observability (fires rarely):
    // a gate stuck closed is a black UI with no other symptom.
    {
        static bool lastGate = true;
        const bool gate = g_controlsVisible.load(std::memory_order_relaxed);
        if (gate != lastGate) {
            lastGate = gate;
            LOG_TO_FILE("[NativeBridge:Linux] snapshot gate "
                << (gate ? "OPEN" : "CLOSED"));
        }
    }
    if (!g_controlsVisible) {
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        if (g_snapSurf) {
            cairo_surface_destroy(g_snapSurf);
            g_snapSurf = nullptr;
        }
        if (controlsWin) g_object_unref(controlsWin);
        if (webView) g_object_unref(webView);
        if (glArea) g_object_unref(glArea);
        return G_SOURCE_CONTINUE;
    }
    // Watchdog (time-based): a hung web process never calls back. Snapshots
    // are requested from the render path now; the tick only reaps a stuck
    // one so the pipeline restarts on its own.
    {
        const gint64 tickNow = g_get_monotonic_time();
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        if (g_snapInFlight && (tickNow - g_lastSnapReqUs) > 2500000) {
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
    }
    // Overlay-driven renders: neither engine reliably queues a render when it
    // has no new frame (mpv while buffering/paused; VLC likewise). Then the
    // probing banner/controls would never composite and the canvas stays
    // black. When the overlay is visible but no engine update arrived for
    // >100ms, force renders at ~30fps so snapshots still reach the screen
    // (each render also requests the next snapshot: demand-driven pipeline).
    // While video plays, engine updates drive rendering and this stays quiet.
    {
        bool hasSnap = false;
        {
            std::lock_guard<std::mutex> lock(g_lifecycleMutex);
            hasSnap = (g_snapSurf != nullptr);
        }
        if (g_controlsVisible && glArea && GTK_IS_GL_AREA(glArea)) {
            const gint64 now = g_get_monotonic_time();
            const bool snapStale = (now - g_lastSnapDoneUs) > 100000;
            static gint64 lastForcedUs = 0;
            // Force on missing DRAWS, not on quiet engine: mpv chatters
            // (update events) throughout buffering while drawing nothing, and
            // gating on engine freshness starved the probing banner to black
            // on direct (no-scrape) launches.
            if ((hasSnap || snapStale) && (now - g_lastRenderDoneUs) > 100000 && (now - lastForcedUs) > 33000) {
                lastForcedUs = now;
                gtk_gl_area_queue_render(glArea);
            }
        }
    }
    // VLC shares the GL pass above (vlcVideoDisplay queues GtkGLArea renders
    // per frame; the forced-render block covers idle/buffering), so no cairo
    // repaint timer is needed here.
    if (controlsWin) g_object_unref(controlsWin);
    if (webView) g_object_unref(webView);
    if (glArea) g_object_unref(glArea);
    return G_SOURCE_CONTINUE;
}

void startCompositeTimer() {
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    if (g_compositeTimer == 0 && !g_teardownRequested) {
        // 60fps while visible for original smoothness (was 33ms/30fps).
        g_compositeTimer = g_timeout_add(16, compositeTick, nullptr);
    }
}

// Pristine compositor state for a reused surface. Without this, session ≥2
// inherits session 1's snapshot pixels, GL texture sizes, snap flags and
// visibility gate (black/frozen/flickering UI). Called on the GTK thread
// right after a successful reattach; the incoming session's pushes and
// snapshots rebuild everything within milliseconds, exactly like session 1.
static void resetReuseRenderStateOnGtk() {
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
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
    g_overlayTex = 0;
    g_overlayTexW = 0;
    g_overlayTexH = 0;
    g_videoTex = 0;
    g_videoTexW = 0;
    g_videoTexH = 0;
    g_mpvFirstFrameLogged = false;
    // Assume visible: the session's pushes correct this within milliseconds,
    // but a stale hidden gate would black out the whole session (no banner,
    // no controls) with no recovery path.
    g_controlsVisible = true;
    g_lastSnapReqUs = 0;
    g_lastSnapDoneUs = 0;
    g_lastMpvUpdateUs.store(0, std::memory_order_relaxed);
    if (g_compositeTimer == 0 && !g_teardownRequested) {
        g_compositeTimer = g_timeout_add(16, compositeTick, nullptr);
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
    // GL context may be gone here; the driver reclaims the textures with it.
    g_overlayTex = 0;
    g_overlayTexW = 0;
    g_overlayTexH = 0;
    g_videoTex = 0;
    g_videoTexW = 0;
    g_videoTexH = 0;
}


void mpvRenderUpdateCallback(void*) {
    {
        std::lock_guard<std::mutex> renderLock(g_renderMutex);
        if (!mpvActiveContext()) return;
    }
    g_lastMpvUpdateUs.store(g_get_monotonic_time(), std::memory_order_relaxed);
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
        if (!g_teardownRequested && g_glArea && mpvActiveContext()) {
            gtk_gl_area_queue_render(g_glArea);
        }
    });
}

// Widget references held for the duration of one render (released at every
// exit via RAII): the post-composite snapshot request below needs live
// objects even if teardown lands mid-frame.
struct RenderWidgetRefs {
    GtkWidget* controlsWin = nullptr;
    WebKitWebView* webView = nullptr;
    ~RenderWidgetRefs() {
        if (controlsWin) g_object_unref(controlsWin);
        if (webView) g_object_unref(webView);
    }
};

gboolean renderMpvFrame(GtkGLArea* area, GdkGLContext*, gpointer) {
    // A draw was attempted on the GTK thread: record it so the tick's
    // forced-render logic measures real draws, not engine chatter (mpv emits
    // update events constantly while buffering, yet draws nothing until the
    // first frame — measuring events instead starved the banner to black).
    g_lastRenderDoneUs = g_get_monotonic_time();
    std::lock_guard<std::mutex> lock(g_renderMutex);
    RenderWidgetRefs refs;
    {
        std::lock_guard<std::mutex> lifecycleLock(g_lifecycleMutex);
        if (!g_teardownRequested && g_webView) {
            if (g_controlsWindow && GTK_IS_WIDGET(g_controlsWindow)) {
                g_object_ref(g_controlsWindow);
                refs.controlsWin = g_controlsWindow;
            }
            if (WEBKIT_IS_WEB_VIEW(g_webView)) {
                g_object_ref(g_webView);
                refs.webView = g_webView;
            }
        }
    }

    const int width = std::max(gtk_widget_get_allocated_width(GTK_WIDGET(area)), 1);
    const int height = std::max(gtk_widget_get_allocated_height(GTK_WIDGET(area)), 1);
    // GtkGLArea renders into a GTK-owned framebuffer. FBO 0 is not
    // guaranteed to be the active target.
    using GlGetIntegervFn = void (*)(unsigned int, int*);
    auto glGetIntegerv = reinterpret_cast<GlGetIntegervFn>(
        getGlProcAddress(nullptr, "glGetIntegerv"));
    int activeFbo = 0;
    if (glGetIntegerv) {
        constexpr unsigned int kDrawFramebufferBinding = 0x8CA6;
        glGetIntegerv(kDrawFramebufferBinding, &activeFbo);
    }
    if (activeFbo != 0) glBindFramebuffer(GL_FRAMEBUFFER, activeFbo);
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    // VLC backend: same GL pass as mpv (video texture + shared snapshot
    // overlay, same vsync) instead of the software cairo repaint.
    if (vlcVideoActive()) {
        drawVlcGlFrame(width, height);
        // Demand-driven snapshot: one request per displayed frame keeps the
        // overlay live at exactly the display rate (or the forced rate while
        // idle) — no timer, no waste, no backlog.
        if (g_controlsVisible) requestOverlaySnapshot(refs.controlsWin, refs.webView);
        return TRUE;
    }

    mpv_render_context* activeRenderContext = mpvActiveContext();
    if (!activeRenderContext || !g_renderContextRender) {
        // No engine owns the surface (backend-switch gap): keep compositing
        // the latest snapshot — usually the probing banner the new session
        // just reset — instead of freezing the previous engine's last frame.
        // (Takes g_lifecycleMutex like drawControlsSnapshot already does;
        // both run on the GTK thread outside nested render callbacks.)
        bool hasSnap = false;
        {
            std::lock_guard<std::mutex> lock(g_lifecycleMutex);
            hasSnap = g_controlsVisible && (g_snapSurf != nullptr);
        }
        if (hasSnap) {
            drawControlsSnapshot(width, height);
            if (g_controlsVisible) requestOverlaySnapshot(refs.controlsWin, refs.webView);
            return TRUE;
        }
        return FALSE;
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

    const int result = g_renderContextRender(activeRenderContext, params);
    if (result < 0) {
        LOG_TO_FILE("[NativeBridge:Linux] MPV Render API frame failed: " << result);
        return FALSE;
    }
    // mpv may leave a different framebuffer bound after its internal render
    // passes; if we draw into that, the overlay lands in a buffer that is
    // never presented (the controls stayed invisible while glGetError was
    // clean). Rebind the FBO we asked mpv to render into, THEN composite.
    if (activeFbo != 0) glBindFramebuffer(GL_FRAMEBUFFER, activeFbo);
    // Flicker-free overlay: composite the controls snapshot inside the same
    // GL pass as the video (no transparent X window involved).
    drawControlsSnapshot(width, height);
    if (g_controlsVisible) requestOverlaySnapshot(refs.controlsWin, refs.webView);
    g_renderContextReportSwap(activeRenderContext);
    return TRUE;
}

// VLC frames used to be painted by a software cairo handler. They now ride
// the shared GtkGLArea pass above (drawVlcGlFrame); this handler stays only
// as a fallback and must not propagate the draw to the parent.
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
    // Empirical refusal: height 1058 crashes libvlc 3.0.23's vout in every
    // observed session (6/6 native faults, zero healthy counterexamples;
    // sibling heights 738/802/992/1040 all play cleanly). Refusing the format
    // fails the session fast and cleanly BEFORE any display chain or wild
    // copy exists; the existing broken-vout watcher then reroutes to mpv
    // (which plays the same content flawlessly) with a toast. Never silently
    // broaden this without new crash evidence.
    if (*height == 1058) {
        g_vlcVoutBroken.store(true, std::memory_order_relaxed);
        LOG_TO_FILE("[NativeBridge:Linux] VLC refusing known-crashing geometry "
            << *width << "x" << *height << "; rerouting to mpv");
        return 0;
    }
    // Lock to the first REAL geometry for the whole session. Adaptive HLS
    // renditions flap by a fixed delta (observed 1040<->1058 and 464<->482,
    // always 18 lines), and every change rebuilds VLC's display/converter
    // chain — the rebuilds correlate 1:1 with the native vout faults. With a
    // fixed geometry VLC scales content instead (its solid scaler path) and
    // the chain is built once. Tiny probing sizes never lock; the fingerprint
    // log below proves per-session whether any renegotiation survived.
    if (!frame->sizeLocked && *width >= 640 && *height >= 360) {
        frame->lockedW = *width;
        frame->lockedH = *height;
        frame->sizeLocked = true;
        LOG_TO_FILE("[NativeBridge:Linux] VLC geometry locked to "
            << frame->lockedW << "x" << frame->lockedH);
    }
    if (frame->sizeLocked) {
        *width = frame->lockedW;
        *height = frame->lockedH;
    }
    frame->width = *width;
    frame->height = *height;
    frame->stride = *width;
    frame->uvStride = (*width + 1) / 2;
    frame->chromaH = (*height + 1) / 2;
    if (!frame->pixels.empty()) {
        // Retire (never free-in-place): display references obtained from an
        // earlier Lock may still be queued downstream. Cap the history; the
        // flap alternates between two sizes so two generations suffice, four
        // is margin. Drained at session retire.
        frame->retiredPixels.emplace_back(std::move(frame->pixels));
        while (frame->retiredPixels.size() > 4) frame->retiredPixels.erase(frame->retiredPixels.begin());
    }
    frame->pixels.resize(
        static_cast<size_t>(frame->stride) * frame->height +
        static_cast<size_t>(frame->uvStride) * frame->chromaH * 2);
    std::fill(frame->pixels.begin(), frame->pixels.end(), 0);
    frame->rgba.assign(static_cast<size_t>(frame->width) * 4 * frame->height, 0);
    frame->rgbaFrameCount = 0;
    if (frame->sws) {
        sws_freeContext(frame->sws);
        frame->sws = nullptr;
    }
    frame->configured = true;
    // Diagnostic fingerprint (one line per session): crash forensics compares
    // the fault address against this buffer range to attribute VLC picture
    // copy faults to our buffer vs VLC-internal pictures. Pointer printed via
    // void* so no sticky hex flags leak into the shared log stream.
    LOG_TO_FILE("[NativeBridge:Linux] VLC video buffer base="
        << static_cast<const void*>(frame->pixels.data()) << " bytes="
        << static_cast<unsigned long long>(frame->pixels.size())
        << " (I420 " << frame->width << "x" << frame->height
        << " stride=" << frame->stride << ")");

    // I420 planar: decoder-native, no VLC-side conversion. Three planes;
    // odd heights use rounded-up chroma (standard broadcast layout).
    chroma[0] = 'I';
    chroma[1] = '4';
    chroma[2] = '2';
    chroma[3] = '0';
    pitches[0] = frame->stride;
    pitches[1] = frame->uvStride;
    pitches[2] = frame->uvStride;
    lines[0] = frame->height;
    lines[1] = frame->chromaH;
    lines[2] = frame->chromaH;
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
    unsigned char* base = frame->pixels.data();
    planes[0] = base;
    planes[1] = base + static_cast<size_t>(frame->stride) * frame->height;
    planes[2] = base + static_cast<size_t>(frame->stride) * frame->height +
        static_cast<size_t>(frame->uvStride) * frame->chromaH;
    return frame;
}

// Convert the latest decoded I420 frame to BGRA staging for display.
// Caller must hold frame->mutex. Returns false when there is nothing usable.
static bool vlcEnsureRgba(VlcFrameBuffer* frame) {
    if (!frame || !frame->configured || frame->pixels.empty() ||
        frame->width == 0 || frame->height == 0) {
        return false;
    }
    const size_t want = static_cast<size_t>(frame->width) * 4 * frame->height;
    if (frame->rgba.size() != want) frame->rgba.assign(want, 0);
    if (frame->rgbaFrameCount == frame->frameCount && !frame->rgba.empty()) return true;
    // The geometry can shift without a format callback (crop-only changes):
    // a stale context converts new planes with old dims (green garbage until
    // the next renegotiation), so revalidate dimensions on every call.
    if (!frame->sws || frame->swsW != frame->width || frame->swsH != frame->height) {
        if (frame->sws) {
            sws_freeContext(frame->sws);
            frame->sws = nullptr;
        }
        frame->sws = sws_getCachedContext(
            nullptr,
            static_cast<int>(frame->width), static_cast<int>(frame->height), AV_PIX_FMT_YUV420P,
            static_cast<int>(frame->width), static_cast<int>(frame->height), AV_PIX_FMT_BGRA,
            SWS_BICUBIC, nullptr, nullptr, nullptr);
        if (frame->sws) {
            frame->swsW = frame->width;
            frame->swsH = frame->height;
        } else {
            return false;
        }
        // HD content is BT.709; SD stays BT.601 (libswscale default).
        if (frame->height >= 720) {
            int sRange, dRange, brightness, contrast, saturation;
            const int* invTbl = nullptr;
            const int* tbl = nullptr;
            if (sws_getColorspaceDetails(frame->sws, const_cast<int**>(&invTbl), &sRange,
                                         const_cast<int**>(&tbl), &dRange,
                                         &brightness, &contrast, &saturation) >= 0) {
                sws_setColorspaceDetails(frame->sws, sws_getCoefficients(SWS_CS_ITU709),
                                         sRange, tbl, dRange,
                                         brightness, contrast, saturation);
            }
        }
    }
    const unsigned char* base = frame->pixels.data();
    const unsigned char* src[3] = {
        base,
        base + static_cast<size_t>(frame->stride) * frame->height,
        base + static_cast<size_t>(frame->stride) * frame->height +
            static_cast<size_t>(frame->uvStride) * frame->chromaH,
    };
    const int srcStride[3] = {
        static_cast<int>(frame->stride),
        static_cast<int>(frame->uvStride),
        static_cast<int>(frame->uvStride),
    };
    unsigned char* dst[1] = { frame->rgba.data() };
    const int dstStride[1] = { static_cast<int>(frame->width) * 4 };
    const bool converted = sws_scale(frame->sws, src, srcStride, 0,
                                     static_cast<int>(frame->height), dst, dstStride) > 0;
    if (converted) frame->rgbaFrameCount = frame->frameCount;
    return converted;
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
    g_lastVlcFrameUs.store(g_get_monotonic_time(), std::memory_order_relaxed);
    if (firstFrame) {
        LOG_TO_FILE("[NativeBridge:Linux] First VLC video callback frame: "
            << frameWidth << "x" << frameHeight);
    }

    invokeOnGtkThread([] {
        // Unified GL pass: queue a GtkGLArea render; renderMpvFrame takes the
        // VLC branch (video texture + shared snapshot overlay, same vsync).
        std::lock_guard<std::mutex> lifecycleLock(g_lifecycleMutex);
        std::lock_guard<std::mutex> renderLock(g_renderMutex);
        if (!g_teardownRequested && g_glArea) {
            gtk_gl_area_queue_render(g_glArea);
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

    const double targetWidth = std::max(1, gtk_widget_get_allocated_width(widget));
    const double targetHeight = std::max(1, gtk_widget_get_allocated_height(widget));

    auto* frame = g_vlcFrame.load();
    if (frame) {
        std::lock_guard<std::mutex> framelock(frame->mutex);
        if (frame->active && frame->configured && !frame->pixels.empty() &&
            frame->width > 0 && frame->height > 0 &&
            vlcEnsureRgba(frame)) {
            auto* image = cairo_image_surface_create_for_data(
                frame->rgba.data(),
                CAIRO_FORMAT_ARGB32,
                static_cast<int>(frame->width),
                static_cast<int>(frame->height),
                static_cast<int>(frame->width) * 4
            );
            if (cairo_surface_status(image) == CAIRO_STATUS_SUCCESS) {
                const double vscale = std::min(
                    targetWidth / static_cast<double>(frame->width),
                    targetHeight / static_cast<double>(frame->height));
                const double drawWidth = static_cast<double>(frame->width) * vscale;
                const double drawHeight = static_cast<double>(frame->height) * vscale;
                cairo_save(cairo);
                cairo_translate(cairo, (targetWidth - drawWidth) / 2.0, (targetHeight - drawHeight) / 2.0);
                cairo_scale(cairo, vscale, vscale);
                cairo_set_source_surface(cairo, image, 0.0, 0.0);
                cairo_pattern_set_filter(cairo_get_source(cairo), CAIRO_FILTER_BILINEAR);
                cairo_paint(cairo);
                cairo_restore(cairo);
            }
            cairo_surface_destroy(image);
        }
    }

    // Paint the latest controls snapshot over the canvas. Runs even when no
    // video frame exists yet (buffering / failed stream) so the probing
    // banner is visible instead of pure black — same guarantee as the mpv
    // forced-render path.
    if (g_controlsVisible) {
        cairo_surface_t* overlay = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_lifecycleMutex);
            if (g_snapSurf) overlay = cairo_surface_reference(g_snapSurf);
        }
        if (overlay) {
            const int ow = cairo_image_surface_get_width(overlay);
            const int oh = cairo_image_surface_get_height(overlay);
            if (ow > 0 && oh > 0) {
                cairo_save(cairo);
                cairo_scale(cairo,
                    targetWidth / static_cast<double>(ow),
                    targetHeight / static_cast<double>(oh));
                cairo_set_source_surface(cairo, overlay, 0.0, 0.0);
                cairo_pattern_set_filter(cairo_get_source(cairo), CAIRO_FILTER_BILINEAR);
                cairo_paint(cairo);
                cairo_restore(cairo);
            }
            cairo_surface_destroy(overlay);
        }
    }

    // Stop draw propagation: returning FALSE lets the parent GtkOverlay paint
    // over the finished frame (a per-frame overpaint = visible flicker on the
    // software cairo path). The mpv GL path already returns TRUE.
    return TRUE;
}

bool attachMpvRenderOnGtk(mpv_handle* handle) {
    if (!handle || g_teardownRequested || !g_glArea) return mpvActiveContext() != nullptr;
    if (mpvActiveContext()) return true;
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
        // Root-fix phase 0.2: identify the ACTUAL GL context profile before
        // any overlay drawing code is written. Legacy fixed-function GL
        // (glBegin/glOrtho) only exists in compatibility-profile contexts; in
        // a core-profile context those calls silently draw nothing (the black
        // overlay from the previous iteration). Resolve symbols indirectly so
        // this probe works regardless of link-time GL availability.
        if (!g_glContextProbed) {
            g_glContextProbed = true;
            using GetStringFn = const unsigned char* (*)(unsigned int);
            using GetIntegervFn = void (*)(unsigned int, int*);
            auto glGetStringFn = reinterpret_cast<GetStringFn>(
                getGlProcAddress(nullptr, "glGetString"));
            auto glGetIntegervFn = reinterpret_cast<GetIntegervFn>(
                getGlProcAddress(nullptr, "glGetIntegerv"));
            const char* ver = glGetStringFn ? (const char*)glGetStringFn(0x1F02) : "?";
            const char* rend = glGetStringFn ? (const char*)glGetStringFn(0x1F01) : "?";
            const char* glsl = glGetStringFn ? (const char*)glGetStringFn(0x8B8C) : "?";
            int profileMask = 0;
            if (glGetIntegervFn) glGetIntegervFn(0x9126, &profileMask);  // GL_CONTEXT_PROFILE_MASK
            const char* profile = (profileMask & 0x00000001) ? "CORE"
                : (profileMask & 0x00000002) ? "COMPATIBILITY" : "unknown";
            LOG_TO_FILE("[NativeBridge:Linux] GL context probe: version="
                << (ver ? ver : "?") << " renderer=" << (rend ? rend : "?")
                << " glsl=" << (glsl ? glsl : "?")
                << " profileMask=0x" << std::hex << profileMask << std::dec << " (" << profile << ")");
        }
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
        mpvAttachContext(handle, renderContext);
        g_mpvFirstFrameLogged = false;
    }
    g_renderContextSetUpdateCallback(renderContext, mpvRenderUpdateCallback, nullptr);
    gtk_widget_queue_draw(GTK_WIDGET(g_glArea));
    LOG_TO_FILE("[NativeBridge:Linux] MPV Render API attached to GtkGLArea");
    return true;
}

void detachMpvRenderOnGtk() {
    std::lock_guard<std::mutex> lock(g_renderMutex);
    mpvDetachAllContexts();
}

// Detach (and free) exactly one handle's render context. Called from engine
// teardown BEFORE mpv stop/terminate so terminate_destroy never observes an
// attached context (which aborts the process). Safe to call with no context.
void detachMpvRenderForHandle(mpv_handle* handle) {
    std::lock_guard<std::mutex> lock(g_renderMutex);
    mpvDetachContext(handle);
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
        // Never block the GTK loop on a teardown worker holding this across
        // a stalled playerStop/playerRelease: skip this 80ms tick instead.
        std::unique_lock<std::mutex> vlcLock(g_vlcMutex, std::try_to_lock);
        if (!vlcLock.owns_lock()) return G_SOURCE_CONTINUE;
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
    {
        std::lock_guard<std::mutex> vlcLock(g_vlcMutex);
        player = g_vlcPlayer;
        g_vlcPlayer = nullptr;
        // NOTE: the libvlc instance is process-lifetime (see ensureVlcInstance):
        // creating/releasing it per session reloads ~50 native plugins every
        // time, which both slows session start and churns native loader state
        // concurrently with network threads. Never release it here.
    }
    if (g_vlcArea) gtk_widget_hide(g_vlcArea);
    if (g_glArea) gtk_widget_show(GTK_WIDGET(g_glArea));
    if (!player && !frame) return;

    // Player stop/release can block for tens of seconds on a stalled network
    // input (link switch to a dead server froze the whole UI here). Never
    // block the GTK loop on it: tear down on a worker. The UI stays alive
    // and the next session starts immediately; late callbacks observe
    // active=false via their own frame pointer.
    std::thread([player, frame] {
        // See g_vlcTeardownMutex: never overlap two teardowns. This worker
        // holds g_vlcMutex across stop+release so no getter/control can touch
        // a player being freed (use-after-free -> heap smash surfacing later
        // in VLC's picture path). Control paths use try_lock and skip while
        // a teardown owns it; getters have bounded futures.
        {
            std::lock_guard<std::mutex> teardownLock(g_vlcTeardownMutex);
            std::lock_guard<std::mutex> vlcLock(g_vlcMutex);
            // Quiesce first: pause lets the vout drain its pending pictures
            // while everything is still valid. Releasing (or observing) a
            // mid-copy broken converter chain is what faults at teardown.
            if (player && g_vlcPlayerPause) g_vlcPlayerPause(player);
        }
        // Let the paused pipeline settle with no locks held (decode threads
        // release the frame mutex as they unwind; the barrier below confirms).
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::lock_guard<std::mutex> teardownLock(g_vlcTeardownMutex);
        std::lock_guard<std::mutex> vlcLock(g_vlcMutex);
        if (player) {
            if (g_vlcPlayerStop) g_vlcPlayerStop(player);
            if (g_vlcPlayerRelease) g_vlcPlayerRelease(player);
        }
        if (frame) {
            std::unique_lock<std::mutex> frameLock(frame->mutex);
            const bool callbacksDone = frame->callbacksCv.wait_for(frameLock, std::chrono::seconds(2), [frame] { return frame->callbacksInFlight == 0; });
            if (!callbacksDone) {
                LOG_TO_FILE("[NativeBridge:Linux] VLC callback barrier timed out; retaining session context");
            }
            if (frame->sws) {
                sws_freeContext(frame->sws);
                frame->sws = nullptr;
            }
            // Never free storage here: VLC's display side may still reference
            // planes obtained before Unlock (not covered by the in-flight
            // barrier). Zero-fill keeps any stale reference mapped and black
            // instead of dangling; memory is reclaimed with the frame object.
            std::fill(frame->pixels.begin(), frame->pixels.end(), 0);
            for (auto& old : frame->retiredPixels) std::fill(old.begin(), old.end(), 0);
            std::fill(frame->rgba.begin(), frame->rgba.end(), 0);
            frame->width = 0;
            frame->height = 0;
            frame->stride = 0;
            frame->frameCount = 0;
            std::lock_guard<std::mutex> retiredLock(g_vlcRetiredMutex);
            g_vlcRetiredFrames.emplace_back(frame);
            while (g_vlcRetiredFrames.size() > 4) g_vlcRetiredFrames.erase(g_vlcRetiredFrames.begin());
        }
    }).detach();
}

bool startVlcOnGtk(
    const std::string& url,
    const std::string& title,
    const std::string& userAgent,
    const std::string& referer,
    long long startMs
) {
    // NOTE (mpv-only application): the entire VLC engine below is DORMANT.
    // No Kotlin entry point references it (see NativePlayerBridge), so none
    // of this code can run: no threads, no sessions, no callbacks. It is kept
    // to preserve a compilable reference, not to execute. Do not rewire it
    // without re-proving teardown safety (see git history: vmem/converter
    // heap faults with HLS content).
    if (url.empty() || !g_vlcArea || !ensureVlcSymbols()) return false;
    stopVlcOnGtk();

    // Process-lifetime libvlc instance (see ensureVlcInstance): creating and
    // releasing it per session reloads dozens of native plugins every time
    // and churns loader state concurrently with network threads.
    libvlc_instance_t* vlcInstance = nullptr;
    {
        std::lock_guard<std::mutex> vlcLock(g_vlcMutex);
        vlcInstance = g_vlcInstance;
    }
    if (!vlcInstance) {
        // libVLC discovers system plugins from libvlccore. Packaged/custom VLC
        // layouts can override that location explicitly without changing the
        // process-wide environment. When the environment is not configured,
        // derive the plugin directory from the loaded libVLC path before falling
        // back to the common multi-arch locations.
        std::vector<std::string> vlcOptionStorage;
        std::vector<const char*> vlcOptions;
        // Diagnostic only (CS3_VLC_VERBOSE=2 in env): full module/format
        // chatter on stderr to catch what precedes a vout crash.
        if (g_getenv("CS3_VLC_VERBOSE")) vlcOptions.push_back("--verbose=2");
        // Diagnostic gate (CS3_VLC_NOSPU=1 in env): disable the spu decoder
        // path entirely for one run. Core forensics implicates the spu blend
        // chain (subpicture_Delete + filter_NewBlend + wild release-call) in
        // the native vout crash; this proves or exonerates it in one session.
        if (g_getenv("CS3_VLC_NOSPU")) vlcOptions.push_back("--no-spu");
        const std::string pluginPath = discoverVlcPluginPath();
        if (!pluginPath.empty()) {
            vlcOptionStorage.emplace_back("--plugin-path=" + pluginPath);
            vlcOptions.push_back(vlcOptionStorage.back().c_str());
            LOG_TO_FILE("[NativeBridge:Linux] Using VLC plugin path: " << pluginPath);
        }
        vlcInstance = g_vlcNew(
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
        std::lock_guard<std::mutex> vlcLock(g_vlcMutex);
        g_vlcInstance = vlcInstance;
        // Watch this instance's log for a broken display chain from the very
        // first session (subscription is idempotent per instance).
        if (g_vlcLogSet) g_vlcLogSet(vlcInstance, vlcLogWatchCb, nullptr);
    }
    auto* media = g_vlcMediaNewLocation(vlcInstance, url.c_str());
    if (!media) {
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
    // Subtitle style from the cached prefs (media-level freetype options only
    // take effect at session start; live size tweaks use text-scale instead).
    {
        std::lock_guard<std::mutex> styleLock(g_vlcStyleMutex);
        if (!g_vlcSubFont.empty()) {
            const std::string option = ":freetype-font=" + g_vlcSubFont;
            g_vlcMediaAddOption(media, option.c_str());
        }
        if (g_vlcSubSizePx > 0) {
            const std::string option = ":freetype-fontsize=" + std::to_string(g_vlcSubSizePx);
            g_vlcMediaAddOption(media, option.c_str());
        }
        if (g_vlcSubColorHex.size() == 6) {
            const std::string option = ":freetype-color=0x" + g_vlcSubColorHex;
            g_vlcMediaAddOption(media, option.c_str());
        }
        if (g_vlcSubBgOpacity >= 0 && g_vlcSubBgOpacity <= 255) {
            const std::string option = ":freetype-background-opacity=" + std::to_string(g_vlcSubBgOpacity);
            g_vlcMediaAddOption(media, option.c_str());
        }
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
    g_vlcPresentedFrames.store(0, std::memory_order_relaxed);
    g_lastVlcFrameUs.store(0, std::memory_order_relaxed);
    // Fresh session, fresh verdict: a previous session may have flagged the
    // shared instance.
    g_vlcVoutBroken.store(false, std::memory_order_relaxed);
    g_vlcVideoSetCallbacks(
        player,
        vlcVideoLock,
        vlcVideoUnlock,
        vlcVideoDisplay,
        frame
    );
    g_vlcVideoSetFormatCallbacks(player, vlcVideoFormat, vlcVideoCleanup);
    // Unified GL pass: VLC video rides GtkGLArea like mpv (same vsync +
    // overlay). The cairo drawing area stays hidden; its draw handler is only
    // a fallback now.
    gtk_widget_hide(g_vlcArea);
    gtk_widget_show(GTK_WIDGET(g_glArea));

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
void reattachControlsToHost(Window hostWindow) {
    if (!g_controlsWindow || g_controlsXid == 0 || hostWindow == 0) return;
    auto* display = gdk_display_get_default();
    if (!display || !GDK_IS_X11_DISPLAY(display)) return;
    auto* xDisplay = gdk_x11_display_get_xdisplay(display);
    // GDK-level reparent keeps pointer/keyboard dispatch working (mirrors the
    // creation sequence); raw X reparent is the fallback. Trapped: the target
    // Canvas may be dying concurrently during episode transitions.
    gdk_x11_display_error_trap_push(display);
    GdkWindow* controlsGdkWin = gtk_widget_get_window(g_controlsWindow);
    if (controlsGdkWin) {
        GdkWindow* hostGdk = gdk_x11_window_foreign_new_for_display(display, hostWindow);
        if (hostGdk) {
            gdk_window_reparent(controlsGdkWin, hostGdk, 0, 0);
            g_object_unref(hostGdk);
        } else {
            XReparentWindow(xDisplay, g_controlsXid, hostWindow, 0, 0);
        }
        XMapWindow(xDisplay, g_controlsXid);
        XFlush(xDisplay);
        gdk_window_raise(controlsGdkWin);
    } else {
        XReparentWindow(xDisplay, g_controlsXid, hostWindow, 0, 0);
        XMapWindow(xDisplay, g_controlsXid);
        XFlush(xDisplay);
    }
    { const gint trapErrors = gdk_x11_display_error_trap_pop(display); (void)trapErrors; }
}

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
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        if (g_webView) webkit_web_view_stop_loading(g_webView);
    }

    // Do not leave the plug under the AWT Canvas. AWT destroys its X11 child
    // window as soon as the Compose player is removed, which would invalidate
    // every GtkWidget/GdkWindow pointer kept for the next player session.
    if (g_plug && g_parkingWindowId != 0) {        auto* plugWindow = gtk_widget_get_window(g_plug);
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
        // The controls window was reparented under the same AWT Canvas at
        // creation. Park it too: AWT destroys Canvas children on removal and
        // a server-dead controls XID breaks snapshots, input, and (via GDK)
        // can invalidate the widget pointers kept for the next session.
        if (g_controlsXid != 0 && g_parkingWindowId != 0) {
            auto* display = gdk_display_get_default();
            if (display && GDK_IS_X11_DISPLAY(display)) {
                auto* xDisplay = gdk_x11_display_get_xdisplay(display);
                // The Canvas may already be half-destroyed here; trap
                // BadWindow instead of letting it cascade into GDK warnings
                // and async X errors at teardown.
                gdk_x11_display_error_trap_push(display);
                XReparentWindow(xDisplay, g_controlsXid, g_parkingWindowId, 0, 0);
                XUnmapWindow(xDisplay, g_controlsXid);
                XFlush(xDisplay);
                { const gint trapErrors = gdk_x11_display_error_trap_pop(display); (void)trapErrors; }
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
    // back degraded alpha in snapshots; the software path is correct and the
    // controls page is cheap to render. The page is offscreen, so disabling
    // accelerated compositing cannot cause the ghosting seen with an on-screen
    // overlay. Set before the web process spawns (overwrite=0 keeps overrides).
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
    // Offscreen snapshot path uses WebKit software rendering for alpha-correct
    // snapshots (see DISABLE_DMABUF/COMPOSITING above). The old on-screen
    // accelerated path is gone with the transparent stacking window.
    LOG_TO_FILE("[NativeBridge:Linux] WebKit offscreen snapshot path; software compositing for alpha");
    if (auto* rgbaVisual = gdk_screen_get_rgba_visual(gtk_widget_get_screen(plug))) {
        gtk_widget_set_visual(plug, rgbaVisual);
        gtk_widget_set_visual(GTK_WIDGET(webView), rgbaVisual);
    }
    gtk_widget_set_app_paintable(plug, TRUE);
    gtk_widget_set_can_focus(GTK_WIDGET(webView), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(webView), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(webView), TRUE);
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
    // Keep the native VLC surface below the shared WebView controls. GTK
    // composites overlays in insertion order; adding VLC after WebKit makes
    // the video area the topmost opaque child and hides the loading screen
    // and controls behind a black rectangle.
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
                    << std::hex << controlsXid << std::dec);
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
        // Process-lifetime references: this tree is created once and parked
        // between sessions; it must never be freed while any tick or JNI path
        // may still read the globals (a dangling GtkWidget crashes the type
        // check itself, before any guard can run). Deliberately never
        // released; the OS reclaims them at process exit.
        if (webView) g_object_ref(webView);
        if (controlsWindow) g_object_ref(controlsWindow);
        if (glArea) g_object_ref(glArea);
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

    // Flicker-free overlay: keep the offscreen controls window size-synced and
    // its frame clock alive while the player is open.
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
    {
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        if (g_webView) webkit_web_view_stop_loading(g_webView);
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
    // Release the process-lifetime libvlc objects here at process teardown
    // (the instance is intentionally kept across player sessions; see
    // startVlcOnGtk). Blocking is fine on this terminal path. Serialized
    // with session teardown workers via g_vlcTeardownMutex.
    {
        std::lock_guard<std::mutex> teardownLock(g_vlcTeardownMutex);
        std::lock_guard<std::mutex> vlcLock(g_vlcMutex);
        libvlc_media_player_t* player = nullptr;
        libvlc_instance_t* instance = nullptr;
        {
            player = g_vlcPlayer;
            instance = g_vlcInstance;
            g_vlcPlayer = nullptr;
            g_vlcInstance = nullptr;
        }
        if (player && g_vlcPlayerRelease) g_vlcPlayerRelease(player);
        if (instance && g_vlcRelease) g_vlcRelease(instance);
    }
    if (controlsWindow) {
        if (controlsXid != 0 && GDK_IS_X11_WINDOW(gtk_widget_get_window(controlsWindow))) {
            XCompositeUnredirectWindow(
                gdk_x11_display_get_xdisplay(gdk_display_get_default()),
                controlsXid, CompositeRedirectManual);
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
                    // The controls window was parked with the plug; move it
                    // under the new Canvas so GDK input dispatch, snapshots
                    // and X11 round-trips target a live XID again.
                    reattachControlsToHost(static_cast<Window>(hostWindowId));
                    // Pristine compositor state: drop session 1's snapshot
                    // pixels, texture sizes, snap flags and visibility gate so
                    // this session rebuilds exactly like the first one.
                    resetReuseRenderStateOnGtk();
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

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_detachMpvRender(
    JNIEnv*, jobject, jlong handle)
{
    auto completed = std::make_shared<std::promise<void>>();
    auto result = completed->get_future();
    postUiTask([handle, completed] {
        detachMpvRenderForHandle(reinterpret_cast<mpv_handle*>(handle));
        completed->set_value();
    });
    if (result.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        LOG_TO_FILE("[NativeBridge:Linux] Timed out while detaching MPV Render API");
    }
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
        std::unique_lock<std::mutex> vlcLock(g_vlcMutex, std::try_to_lock);
        if (!vlcLock.owns_lock()) return;  // teardown in progress; drop stale control
        if (g_vlcPlayer && g_vlcPlayerPause) g_vlcPlayerPause(g_vlcPlayer);
    });
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_playVlc(
    JNIEnv*, jobject)
{
    postUiTask([] {
        std::unique_lock<std::mutex> vlcLock(g_vlcMutex, std::try_to_lock);
        if (!vlcLock.owns_lock()) return;  // teardown in progress; drop stale control
        if (g_vlcPlayer && g_vlcPlayerPlay) g_vlcPlayerPlay(g_vlcPlayer);
    });
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_seekVlc(
    JNIEnv*, jobject, jlong positionMs)
{
    postUiTask([positionMs] {
        std::unique_lock<std::mutex> vlcLock(g_vlcMutex, std::try_to_lock);
        if (!vlcLock.owns_lock()) return;  // teardown in progress; drop stale control
        if (g_vlcPlayer && g_vlcPlayerSetTime) {
            g_vlcPlayerSetTime(g_vlcPlayer, std::max<jlong>(0, positionMs));
        }
    });
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_setVlcVolume(
    JNIEnv*, jobject, jint volume)
{
    postUiTask([volume] {
        std::unique_lock<std::mutex> vlcLock(g_vlcMutex, std::try_to_lock);
        if (!vlcLock.owns_lock()) return;  // teardown in progress; drop stale control
        if (g_vlcPlayer && g_vlcAudioSetVolume) {
            g_vlcAudioSetVolume(g_vlcPlayer, std::clamp(static_cast<int>(volume), 0, 200));
        }
    });
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_setVlcRate(
    JNIEnv*, jobject, jfloat rate)
{
    postUiTask([rate] {
        std::unique_lock<std::mutex> vlcLock(g_vlcMutex, std::try_to_lock);
        if (!vlcLock.owns_lock()) return;  // teardown in progress; drop stale control
        if (g_vlcPlayer && g_vlcPlayerSetRate) {
            g_vlcPlayerSetRate(g_vlcPlayer, rate > 0.05f ? rate : 1.0f);
        }
    });
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_setVlcMute(
    JNIEnv*, jobject, jboolean muted)
{
    postUiTask([muted] {
        std::unique_lock<std::mutex> vlcLock(g_vlcMutex, std::try_to_lock);
        if (!vlcLock.owns_lock()) return;  // teardown in progress; drop stale control
        if (g_vlcPlayer && g_vlcAudioSetMute) g_vlcAudioSetMute(g_vlcPlayer, muted ? 1 : 0);
    });
}

// VLC parity controls (mpv path untouched). libvlc delays are microseconds.
JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_setVlcAudioDelay(
    JNIEnv*, jobject, jlong delayMs)
{
    postUiTask([delayMs] {
        std::unique_lock<std::mutex> vlcLock(g_vlcMutex, std::try_to_lock);
        if (!vlcLock.owns_lock()) return;  // teardown in progress; drop stale control
        if (g_vlcPlayer && g_vlcAudioSetDelay) {
            g_vlcAudioSetDelay(g_vlcPlayer, static_cast<long long>(delayMs) * 1000LL);
        }
    });
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_setVlcSpuDelay(
    JNIEnv*, jobject, jlong delayMs)
{
    postUiTask([delayMs] {
        std::unique_lock<std::mutex> vlcLock(g_vlcMutex, std::try_to_lock);
        if (!vlcLock.owns_lock()) return;  // teardown in progress; drop stale control
        if (g_vlcPlayer && g_vlcVideoSetSpuDelay) {
            g_vlcVideoSetSpuDelay(g_vlcPlayer, static_cast<long long>(delayMs) * 1000LL);
        }
    });
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_setVlcAudioTrack(
    JNIEnv*, jobject, jint trackId)
{
    postUiTask([trackId] {
        std::unique_lock<std::mutex> vlcLock(g_vlcMutex, std::try_to_lock);
        if (!vlcLock.owns_lock()) return;  // teardown in progress; drop stale control
        if (g_vlcPlayer && g_vlcAudioSetTrack) g_vlcAudioSetTrack(g_vlcPlayer, trackId);
    });
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_setVlcSpuTrack(
    JNIEnv*, jobject, jint trackId)
{
    postUiTask([trackId] {
        std::unique_lock<std::mutex> vlcLock(g_vlcMutex, std::try_to_lock);
        if (!vlcLock.owns_lock()) return;  // teardown in progress; drop stale control
        if (g_vlcPlayer && g_vlcVideoSetSpu) g_vlcVideoSetSpu(g_vlcPlayer, trackId);
    });
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_setVlcVideoTrack(
    JNIEnv*, jobject, jint trackId)
{
    postUiTask([trackId] {
        std::unique_lock<std::mutex> vlcLock(g_vlcMutex, std::try_to_lock);
        if (!vlcLock.owns_lock()) return;  // teardown in progress; drop stale control
        if (g_vlcPlayer && g_vlcVideoSetTrack) g_vlcVideoSetTrack(g_vlcPlayer, trackId);
    });
}

JNIEXPORT jboolean JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_addSubtitleVlc(
    JNIEnv* env, jobject, jstring path)
{
    const std::string pathValue = toUtf8(env, path);
    if (pathValue.empty()) return JNI_FALSE;
    auto completed = std::make_shared<std::promise<bool>>();
    auto result = completed->get_future();
    postUiTask([pathValue, completed] {
        bool ok = false;
        std::lock_guard<std::mutex> vlcLock(g_vlcMutex);
        if (g_vlcPlayer && g_vlcPlayerAddSlave) {
            // libvlc_media_slave_subtitle is the first enum value (0).
            ok = g_vlcPlayerAddSlave(g_vlcPlayer, 0, pathValue.c_str(), 1) == 0;
        }
        completed->set_value(ok);
    });
    if (result.wait_for(std::chrono::seconds(5)) != std::future_status::ready) return JNI_FALSE;
    return result.get() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_addAudioSlaveVlc(
    JNIEnv* env, jobject, jstring path)
{
    const std::string pathValue = toUtf8(env, path);
    if (pathValue.empty()) return JNI_FALSE;
    auto completed = std::make_shared<std::promise<bool>>();
    auto result = completed->get_future();
    postUiTask([pathValue, completed] {
        bool ok = false;
        std::lock_guard<std::mutex> vlcLock(g_vlcMutex);
        if (g_vlcPlayer && g_vlcPlayerAddSlave) {
            // libvlc_media_slave_audio follows subtitle in the slave enum (1).
            ok = g_vlcPlayerAddSlave(g_vlcPlayer, 1, pathValue.c_str(), 1) == 0;
        }
        completed->set_value(ok);
    });
    if (result.wait_for(std::chrono::seconds(5)) != std::future_status::ready) return JNI_FALSE;
    return result.get() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_setVlcSubtitleStyle(
    JNIEnv* env, jobject, jstring font, jint sizePx, jstring colorHex, jint bgOpacity)
{
    const std::string fontValue = toUtf8(env, font);
    const std::string colorValue = toUtf8(env, colorHex);
    std::lock_guard<std::mutex> styleLock(g_vlcStyleMutex);
    g_vlcSubFont = fontValue;
    g_vlcSubSizePx = sizePx > 0 ? sizePx : 0;
    std::string hex;
    for (char c : colorValue) {
        if (c == '#') continue;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) hex += c;
    }
    g_vlcSubColorHex = (hex.size() == 6 || hex.size() == 8) ? hex.substr(hex.size() == 8 ? 2 : 0, 6) : "";
    g_vlcSubBgOpacity = (bgOpacity >= 0 && bgOpacity <= 255) ? bgOpacity : -1;
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_setVlcSubtitleTextScale(
    JNIEnv*, jobject, jint percent)
{
    postUiTask([percent] {
        std::unique_lock<std::mutex> vlcLock(g_vlcMutex, std::try_to_lock);
        if (!vlcLock.owns_lock()) return;  // teardown in progress; drop stale control
        if (g_vlcPlayer && g_vlcVideoSetSubScale && percent > 0) {
            g_vlcVideoSetSubScale(g_vlcPlayer, percent);
        }
    });
}

JNIEXPORT jboolean JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_applyVlcEqPreset(
    JNIEnv* env, jobject, jstring preset)
{
    const std::string presetValue = toUtf8(env, preset);
    auto completed = std::make_shared<std::promise<bool>>();
    auto result = completed->get_future();
    postUiTask([presetValue, completed] {
        bool ok = false;
        std::lock_guard<std::mutex> vlcLock(g_vlcMutex);
        if (g_vlcPlayer && g_vlcPlayerSetEq) {
            void* eq = nullptr;
            // Prefer the named factory preset when the runtime offers it.
            if (g_vlcEqNewFromPreset && g_vlcEqPresetCount && g_vlcEqPresetName && !presetValue.empty()) {
                const unsigned n = g_vlcEqPresetCount();
                for (unsigned i = 0; i < n; ++i) {
                    const char* name = g_vlcEqPresetName(i);
                    if (name && strcasecmp(name, presetValue.c_str()) == 0) {
                        eq = g_vlcEqNewFromPreset(i);
                        break;
                    }
                }
            }
            if (!eq && g_vlcEqNew) {
                // Flat fallback: fresh equalizer defaults to zeroed bands.
                eq = g_vlcEqNew();
            }
            if (eq) {
                ok = g_vlcPlayerSetEq(g_vlcPlayer, eq) == 0;
                if (g_vlcEqRelease) g_vlcEqRelease(eq);
            }
        }
        completed->set_value(ok);
    });
    if (result.wait_for(std::chrono::seconds(5)) != std::future_status::ready) return JNI_FALSE;
    return result.get() ? JNI_TRUE : JNI_FALSE;
}

// Process shutdown: quit the GTK loop and join its thread so a parked
// reusable surface does not terminate the process via a joinable
// std::thread destructor (SIGABRT "terminate called without an active
// exception"). Safe to call when no surface was ever created.
JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_shutdownNative(
    JNIEnv*, jobject)
{
    GMainLoop* loop = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        loop = g_mainLoop;
        g_teardownRequested = true;
    }
    if (loop) g_main_loop_quit(loop);
    if (g_uiThread.joinable() && g_uiThread.get_id() != std::this_thread::get_id()) {
        g_uiThread.join();
    }
}

JNIEXPORT jboolean JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_isVlcVoutBroken(
    JNIEnv*, jobject)
{
    return g_vlcVoutBroken.load(std::memory_order_relaxed) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_getVlcPresentedCount(
    JNIEnv*, jobject)
{
    return g_vlcPresentedFrames.load(std::memory_order_relaxed);
}

JNIEXPORT jlong JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_getVlcDecoderAgeMs(
    JNIEnv*, jobject)
{
    const gint64 last = g_lastVlcFrameUs.load(std::memory_order_relaxed);
    if (last == 0) return -1;
    return (g_get_monotonic_time() - last) / 1000;
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_setVlcAspectRatio(
    JNIEnv* env, jobject, jstring ratio)
{
    const std::string ratioValue = toUtf8(env, ratio);
    postUiTask([ratioValue] {
        std::unique_lock<std::mutex> vlcLock(g_vlcMutex, std::try_to_lock);
        if (!vlcLock.owns_lock()) return;  // teardown in progress; drop stale control
        if (g_vlcPlayer && g_vlcVideoSetAspect) {
            g_vlcVideoSetAspect(g_vlcPlayer, ratioValue.empty() ? nullptr : ratioValue.c_str());
        }
    });
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_setVlcCropGeometry(
    JNIEnv* env, jobject, jstring geometry)
{
    const std::string geometryValue = toUtf8(env, geometry);
    postUiTask([geometryValue] {
        std::unique_lock<std::mutex> vlcLock(g_vlcMutex, std::try_to_lock);
        if (!vlcLock.owns_lock()) return;  // teardown in progress; drop stale control
        if (g_vlcPlayer && g_vlcVideoSetCrop) {
            g_vlcVideoSetCrop(g_vlcPlayer, geometryValue.empty() ? nullptr : geometryValue.c_str());
        }
    });
}

JNIEXPORT jint JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_getVlcChapter(
    JNIEnv*, jobject)
{
    // Runs on the caller (IO) thread, never GTK: pure libvlc getters are
    // thread-safe. Bounded wait so a teardown worker stalling the mutex
    // degrades to a default instead of piling up.
    std::unique_lock<std::mutex> vlcLock(g_vlcMutex, std::defer_lock);
    if (!vlcLock.try_lock()) return -1;
    if (g_vlcPlayer && g_vlcPlayerGetChapter) return g_vlcPlayerGetChapter(g_vlcPlayer);
    return -1;
}

JNIEXPORT jint JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_getVlcChapterCount(
    JNIEnv*, jobject)
{
    // Direct on the caller thread (see getVlcChapter): never blocks GTK.
    std::unique_lock<std::mutex> vlcLock(g_vlcMutex, std::defer_lock);
    if (!vlcLock.try_lock()) return 0;
    if (g_vlcPlayer && g_vlcPlayerGetChapterCount) return g_vlcPlayerGetChapterCount(g_vlcPlayer);
    return 0;
}

JNIEXPORT jboolean JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_takeSnapshotVlc(
    JNIEnv* env, jobject, jstring path)
{
    const std::string pathValue = toUtf8(env, path);
    if (pathValue.empty()) return JNI_FALSE;
    auto completed = std::make_shared<std::promise<bool>>();
    auto result = completed->get_future();
    postUiTask([pathValue, completed] {
        bool ok = false;
        std::lock_guard<std::mutex> vlcLock(g_vlcMutex);
        if (g_vlcPlayer && g_vlcVideoTakeSnapshot) {
            ok = g_vlcVideoTakeSnapshot(g_vlcPlayer, 0, pathValue.c_str(), 0, 0) == 0;
        }
        completed->set_value(ok);
    });
    if (result.wait_for(std::chrono::seconds(5)) != std::future_status::ready) return JNI_FALSE;
    return result.get() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_setVlcChapter(
    JNIEnv*, jobject, jint chapter)
{
    postUiTask([chapter] {
        std::unique_lock<std::mutex> vlcLock(g_vlcMutex, std::try_to_lock);
        if (!vlcLock.owns_lock()) return;  // teardown in progress; drop stale control
        if (g_vlcPlayer && g_vlcPlayerSetChapter && chapter >= 0) {
            g_vlcPlayerSetChapter(g_vlcPlayer, chapter);
        }
    });
}

// Returns a JSON array string: [{"id":2,"name":"English","selected":true}, ...]
// kind: 0 = audio, 1 = spu/subtitle, 2 = video(ES ids only). Empty array ("[]")
// when unsupported or unavailable — never null.
JNIEXPORT jstring JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_getVlcTrackList(
    JNIEnv* env, jobject, jint kind)
{
    // Direct on the caller (IO) thread, never GTK: pure libvlc getters are
    // thread-safe. Bounded wait so a teardown worker stalling the mutex
    // degrades to "[]" instead of piling up blocked futures.
    std::string json = "[";
    {
        std::unique_lock<std::mutex> vlcLock(g_vlcMutex, std::defer_lock);
        if (vlcLock.try_lock() && g_vlcPlayer) {
            int current = -2;
            int count = 0;
            void* desc = nullptr;
            if (kind == 0 && g_vlcAudioGetTrack && g_vlcAudioGetTrackCount && g_vlcAudioGetTrackDesc) {
                current = g_vlcAudioGetTrack(g_vlcPlayer);
                count = g_vlcAudioGetTrackCount(g_vlcPlayer);
                desc = g_vlcAudioGetTrackDesc(g_vlcPlayer);
            } else if (kind == 1 && g_vlcVideoGetSpu && g_vlcVideoGetSpuCount && g_vlcVideoGetSpuDesc) {
                current = g_vlcVideoGetSpu(g_vlcPlayer);
                count = g_vlcVideoGetSpuCount(g_vlcPlayer);
                desc = g_vlcVideoGetSpuDesc(g_vlcPlayer);
            } else if (kind == 2 && g_vlcVideoGetTrack && g_vlcVideoGetTrackCount) {
                // ES track names are not exposed portably; ids are enough for
                // the quality switcher. No description list needed here.
                current = g_vlcVideoGetTrack(g_vlcPlayer);
                count = g_vlcVideoGetTrackCount(g_vlcPlayer);
            }
            if (desc) {
                // libvlc_track_description_t { int i_id; char* psz_name; next* }
                struct TrackDesc { int id; char* name; TrackDesc* next; };
                bool first = true;
                for (TrackDesc* d = static_cast<TrackDesc*>(desc); d; d = d->next) {
                    if (!first) json += ",";
                    first = false;
                    json += "{\"id\":" + std::to_string(d->id) + ",\"name\":\"";
                    for (const char* c = d->name ? d->name : ""; *c; ++c) {
                        if (*c == '"' || *c == '\\') json += '\\';
                        json += *c;
                    }
                    json += "\",\"selected\":";
                    json += (d->id == current ? "true" : "false");
                    json += "}";
                }
                if (g_vlcTrackDescRelease) g_vlcTrackDescRelease(desc);
                (void)count;
            } else if (kind == 2 && count > 0) {
                for (int i = 0; i < count; ++i) {
                    if (i) json += ",";
                    json += "{\"id\":" + std::to_string(i) + ",\"name\":\"Track " +
                        std::to_string(i + 1) + "\",\"selected\":" +
                        (i == current ? "true" : "false") + "}";
                }
            }
        }
        json += "]";
    }
    return env->NewStringUTF(json.c_str());
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
        // Keep the offscreen controls window matched when resize events DO
        // arrive (the compositeTick host-size guard covers the no-event case).
        if (g_controlsWindow && width > 0 && height > 0) {
            GdkWindow* cwGdk = gtk_widget_get_window(g_controlsWindow);
            if (cwGdk) {
                int scale = gdk_window_get_scale_factor(cwGdk);
                if (scale < 1) scale = 1;
                // width/height from AWT are physical pixels; GTK takes logical.
                const int logicalW = width / scale;
                const int logicalH = height / scale;
                gdk_window_resize(cwGdk, logicalW, logicalH);
                gtk_window_resize(GTK_WINDOW(g_controlsWindow), logicalW, logicalH);
                GtkAllocation alloc = {0, 0, logicalW, logicalH};
                gtk_widget_size_allocate(g_controlsWindow, &alloc);
                if (g_webView) gtk_widget_size_allocate(GTK_WIDGET(g_webView), &alloc);
            }
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
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        if (!g_teardownRequested && g_webView && !value.empty()) {
            webkit_web_view_load_uri(g_webView, value.c_str());
        }
    });
}

JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_openDevTools(
    JNIEnv*, jobject)
{
    postUiTask([] {
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
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
