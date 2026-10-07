#include "../include/player_bridge_common.h"

#ifndef _WIN32
#include <codecvt>
#include <cstdlib>
#include <filesystem>
#include <locale>
#include <sys/stat.h>
#include <unistd.h>
#include <X11/Xlib.h>
#endif

// Global variable definitions
std::ofstream g_logFile;
std::mutex g_logMutex;

#ifndef _WIN32
std::string nativeLogPath() {
    const char* stateHome = std::getenv("XDG_STATE_HOME");
    std::filesystem::path stateRoot;
    if (stateHome && *stateHome && std::filesystem::path(stateHome).is_absolute()) {
        stateRoot = stateHome;
    } else if (const char* home = std::getenv("HOME"); home && *home) {
        stateRoot = std::filesystem::path(home) / ".local" / "state";
    } else {
        stateRoot = "/tmp";
    }

    const auto logDirectory = stateRoot / "CloudStreamDesktop" / "logs";
    std::error_code error;
    std::filesystem::create_directories(logDirectory, error);
    if (error) {
        return (std::filesystem::path("/tmp") /
            ("cloudstream_native_" + std::to_string(static_cast<unsigned long>(getuid())) + ".log")).string();
    }
    const auto path = logDirectory / "native.log";
    ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
    return path.string();
}
#endif

JavaVM*   g_jvm            = nullptr;
jobject   g_listener       = nullptr;
jmethodID g_listenerMethod = nullptr;
std::mutex g_listenerMutex;

mpv_handle* g_mpvHandle = nullptr;
std::mutex  g_mpvMutex;
bool        g_statsVisible = false;

mpv_get_property_fn        g_mpv_get_property        = nullptr;
mpv_get_property_string_fn g_mpv_get_property_string = nullptr;
mpv_free_fn                g_mpv_free                = nullptr;
mpv_command_string_fn      g_mpv_command_string      = nullptr;
mpv_set_property_string_fn g_mpv_set_property_string = nullptr;

#ifdef _WIN32
extern UINT_PTR g_syncTimer;
extern HWND     g_messageHwnd;
#endif

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_jvm = vm;
#ifndef _WIN32
    // The bridge uses Xlib from the GTK thread and from JNI/AWT callbacks.
    // Enable Xlib's internal locking before GTK creates or accesses its
    // display connection so concurrent reparent/resize/fullscreen calls do
    // not race inside Xlib.
    if (XInitThreads() == 0) {
        LOG_TO_FILE("[NativeBridge:Linux] XInitThreads() failed; Xlib calls must remain serialized");
    }
#endif
    return JNI_VERSION_1_6;
}

// JNI Event Dispatcher
void dispatchPlayerEvent(const std::wstring& message) {
    std::lock_guard<std::mutex> listenerLock(g_listenerMutex);
    if (!g_jvm || !g_listener || !g_listenerMethod) return;

    JNIEnv* env = nullptr;
    jint getEnvStat = g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6);
    
    if (getEnvStat == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThreadAsDaemon((void**)&env, nullptr) != JNI_OK) {
            return;
        }
    } else if (getEnvStat == JNI_EVERSION) {
        return;
    }

    jstring jType = env->NewStringUTF("message");
#ifdef _WIN32
    jstring jVal = env->NewString((const jchar*)message.data(), (jsize)message.length());
#else
    // wchar_t is 32-bit on Linux; do not reinterpret it as UTF-16 JNI data.
    // The player bridge payload is UTF-8 JSON, so convert it explicitly.
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    const std::string utf8 = converter.to_bytes(message);
    jstring jVal = env->NewStringUTF(utf8.c_str());
#endif
    env->CallVoidMethod(g_listener, g_listenerMethod, jType, jVal);
    env->DeleteLocalRef(jType);
    env->DeleteLocalRef(jVal);
}

extern "C" {

// setEventListener
JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_setEventListener(
    JNIEnv* env, jobject thiz, jobject listener)
{
    std::lock_guard<std::mutex> listenerLock(g_listenerMutex);
    if (g_listener) { env->DeleteGlobalRef(g_listener); g_listener = nullptr; }
    if (listener) {
        g_listener = env->NewGlobalRef(listener);
        jclass clazz       = env->GetObjectClass(listener);
        g_listenerMethod   = env->GetMethodID(clazz, "onPlayerEvent",
                                              "(Ljava/lang/String;Ljava/lang/String;)V");
        env->DeleteLocalRef(clazz);
    }
}

// startMpvSync
JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_startMpvSync(
    JNIEnv* env, jobject thiz, jlong mpvPtr)
{
    {
        std::lock_guard<std::mutex> lock(g_mpvMutex);
        g_mpvHandle = (mpv_handle*)mpvPtr;
    }
#ifdef _WIN32
    postUiTask([]() {
        if (!g_syncTimer && g_messageHwnd) {
            g_syncTimer = SetTimer(g_messageHwnd, 0x4E51, 16, nullptr);
            LOG_TO_FILE("[NativeBridge] MPV native UI sync timer started");
        }
    });
#else
    startMpvSyncPlatform();
#endif
}

// stopMpvSync
JNIEXPORT void JNICALL Java_com_lagradost_cloudstream3_desktop_player_webview_NativePlayerBridge_stopMpvSync(
    JNIEnv* env, jobject thiz)
{
    {
        std::lock_guard<std::mutex> lock(g_mpvMutex);
        g_mpvHandle = nullptr; // Null out immediately to prevent timer from using a destroyed handle
    }
#ifdef _WIN32
    postUiTask([]() {
        if (g_syncTimer && g_messageHwnd) {
            KillTimer(g_messageHwnd, g_syncTimer);
            g_syncTimer = 0;
            LOG_TO_FILE("[NativeBridge] MPV native UI sync timer stopped");
        }
    });
#else
    stopMpvSyncPlatform();
#endif
}

} // extern "C"
