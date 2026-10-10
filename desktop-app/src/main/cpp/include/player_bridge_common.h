#pragma once

#include <jni.h>
#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <fstream>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <unordered_map>

#ifndef _WIN32
#include <sys/stat.h>
#include <chrono>
#include <ctime>
#include <cstdio>
#endif

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#include <initguid.h>
#include "WebView2.h"
#endif

// File logging macro for production debugging
extern std::ofstream g_logFile;
extern std::mutex g_logMutex;

#ifdef _WIN32
#define LOG_TO_FILE(msg) \
    do { \
        std::lock_guard<std::mutex> lock(g_logMutex); \
        if (!g_logFile.is_open()) { \
            char tempPath[MAX_PATH]; \
            GetTempPathA(MAX_PATH, tempPath); \
            std::string logPath = std::string(tempPath) + "cloudstream_native.log"; \
            g_logFile.open(logPath, std::ios::app); \
        } \
        g_logFile << msg << std::endl; \
        std::cout << msg << std::endl; \
    } while(0)
#else
std::string nativeLogPath();
// HH:MM:SS.mmm prefix so multi-run native logs stay correlatable with the
// timestamped app log. Inline + lock-free (callers already hold g_logMutex).
inline std::string nativeLogTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmv{};
    localtime_r(&t, &tmv);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d ",
        tmv.tm_hour, tmv.tm_min, tmv.tm_sec, static_cast<int>(ms.count()));
    return std::string(buf);
}
#define LOG_TO_FILE(msg) \
    do { \
        std::lock_guard<std::mutex> lock(g_logMutex); \
        if (!g_logFile.is_open()) { \
            const std::string logPath = nativeLogPath(); \
            g_logFile.open(logPath, std::ios::app); \
            if (!g_logFile.is_open()) { \
                g_logFile.clear(); \
                g_logFile.open(nativeLogPath(), std::ios::app); \
            } \
            if (g_logFile.is_open()) { \
                ::chmod(logPath.c_str(), S_IRUSR | S_IWUSR); \
            } \
        } \
        g_logFile << nativeLogTimestamp() << msg << std::endl; \
        std::cout << nativeLogTimestamp() << msg << std::endl; \
    } while(0)
#endif

// MPV Type Definitions. Linux uses the official headers because its Render API
// needs the complete client/render ABI. Windows keeps the local declarations
// so the existing WebView2 bridge remains self-contained.
#ifndef _WIN32
#include <mpv/client.h>
#endif

extern "C" {
#ifdef _WIN32
typedef struct mpv_handle mpv_handle;
typedef enum mpv_format {
    MPV_FORMAT_NONE             = 0,
    MPV_FORMAT_STRING           = 1,
    MPV_FORMAT_OSD_STRING       = 2,
    MPV_FORMAT_FLAG             = 3,
    MPV_FORMAT_INT64            = 4,
    MPV_FORMAT_DOUBLE           = 5,
    MPV_FORMAT_NODE             = 6,
    MPV_FORMAT_NODE_ARRAY       = 7,
    MPV_FORMAT_NODE_MAP         = 8,
    MPV_FORMAT_BYTE_ARRAY       = 9
} mpv_format;
typedef int  (*mpv_get_property_fn)(mpv_handle *ctx, const char *name, mpv_format format, void *data);
typedef char*(*mpv_get_property_string_fn)(mpv_handle *ctx, const char *name);
typedef void (*mpv_free_fn)(void *data);
typedef int  (*mpv_command_string_fn)(mpv_handle *ctx, const char *args);
typedef int  (*mpv_set_property_string_fn)(mpv_handle *ctx, const char *name, const char *data);
#else
typedef int  (*mpv_get_property_fn)(mpv_handle *ctx, const char *name, mpv_format format, void *data);
typedef char*(*mpv_get_property_string_fn)(mpv_handle *ctx, const char *name);
typedef void (*mpv_free_fn)(void *data);
typedef int  (*mpv_command_string_fn)(mpv_handle *ctx, const char *args);
typedef int  (*mpv_set_property_string_fn)(mpv_handle *ctx, const char *name, const char *data);
#endif
}

// Global JNI & Player State
extern JavaVM*   g_jvm;
extern jobject   g_listener;
extern jmethodID g_listenerMethod;
extern std::mutex g_listenerMutex;

extern mpv_handle* g_mpvHandle;
extern std::mutex  g_mpvMutex;
extern bool        g_statsVisible;

// Function pointers for MPV
extern mpv_get_property_fn        g_mpv_get_property;
extern mpv_get_property_string_fn g_mpv_get_property_string;
extern mpv_free_fn                g_mpv_free;
extern mpv_command_string_fn      g_mpv_command_string;
extern mpv_set_property_string_fn g_mpv_set_property_string;

// Forward declarations
void dispatchPlayerEvent(const std::wstring& message);
void postUiTask(std::function<void()> task);
void processUiTasks();
#ifndef _WIN32
void startMpvSyncPlatform();
void stopMpvSyncPlatform();
#endif
