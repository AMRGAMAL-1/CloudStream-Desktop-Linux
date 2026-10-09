package com.lagradost.cloudstream3.desktop.player.webview

import com.lagradost.common.logging.AppLogger
import java.io.File
import java.util.concurrent.atomic.AtomicBoolean

object NativePlayerBridge {
    private val preloadStarted = AtomicBoolean(false)
    private val libraryLoaded = AtomicBoolean(false)
    @Volatile private var loadFailure: String? = null
    private val isWindows = System.getProperty("os.name", "").contains("win", ignoreCase = true)

    init {
        try {
            val resourcesDir = System.getProperty("compose.application.resources.dir")

            if (isWindows) {
                // Try absolute paths first (release mode / installed Windows build).
                val webviewDll = if (resourcesDir != null) java.io.File(resourcesDir, "jni/WebView2Loader.dll") else null
                val playerDll = if (resourcesDir != null) java.io.File(resourcesDir, "jni/player_bridge.dll") else null

                if (webviewDll?.exists() == true && playerDll?.exists() == true) {
                    System.load(webviewDll.absolutePath)
                    System.load(playerDll.absolutePath)
                } else {
                    System.loadLibrary("WebView2Loader")
                    System.loadLibrary("player_bridge")
                }
            } else {
                // Linux ships the bridge as libplayer_bridge.so and uses the system
                // WebKitGTK runtime. Never attempt to load the Windows WebView2 DLLs.
                //
                // The Compose/jpackage layout puts the app resources under
                // $APPDIR/lib/app/resources, but older build configurations emitted
                // a wrong "compose.application.resources.dir=$APPDIR/resources".
                // Resolve the bridge from every known layout so the packaged
                // DEB/RPM/AppImage/tar.gz cannot silently lose the native player.
                val resourcesDir = System.getProperty("compose.application.resources.dir")
                val javaHome = File(System.getProperty("java.home", "."))
                val linuxBridge = buildList {
                    resourcesDir?.let { add(File(it, "jni/libplayer_bridge.so")) }
                    resourcesDir?.let { add(File(File(it).parentFile, "app/resources/jni/libplayer_bridge.so")) }
                    // $APPDIR/lib/app/resources derived from the runtime home.
                    add(javaHome.resolve("../../lib/app/resources/jni/libplayer_bridge.so").normalize())
                    add(javaHome.resolve("../app/resources/jni/libplayer_bridge.so").normalize())
                    // Explicit -Djava.library.path locations (may be relative).
                    System.getProperty("java.library.path", "")
                        .split(File.pathSeparator)
                        .filter { it.isNotBlank() }
                        .forEach { add(File(it, "libplayer_bridge.so")) }
                    // Development tree layout used by ./gradlew run.
                    add(File("desktop-app/appResources/linux/jni/libplayer_bridge.so"))
                }.firstOrNull { it.isFile }
                if (linuxBridge != null) {
                    System.load(linuxBridge.absolutePath)
                } else {
                    System.loadLibrary("player_bridge")
                }
            }
            libraryLoaded.set(true)
            AppLogger.i("Successfully loaded player_bridge native library")
        } catch (e: Throwable) {
            loadFailure = e.message ?: e::class.java.simpleName
            AppLogger.e("Failed to load player_bridge native library: $loadFailure")
        }
    }

    /** True only when all JNI entry points can be called safely. */
    @JvmStatic
    fun isAvailable(): Boolean = libraryLoaded.get()

    /** A short, actionable error for the UI and diagnostics. */
    @JvmStatic
    fun unavailableReason(): String = loadFailure
        ?.let { "Linux native player bridge could not be loaded: $it" }
        ?: "Linux native player bridge is unavailable"

    /**
     * Initializes the native player host.
     * @param hostHwnd The native AWT Canvas handle (HWND on Windows, X11 Window id on Linux).
     * @return The native render target handle, or 0 if initialization failed.
     */
    external fun initWebView(hostHwnd: Long, width: Int, height: Int): Long

    /**
     * Connects an initialized Linux MPV handle to the GTK Render API surface.
     * Windows continues to use the native WebView2/MPV window embedding path.
     */
    external fun attachMpvRender(handle: Long): Boolean
    /** Detach this handle's render context. Must precede mpv stop/terminate. */
    external fun detachMpvRender(handle: Long)

    // NOTE: mpv-only application. The native VLC engine still exists in C++
    // but is dormant (no Kotlin entry points reference it), so it can never
    // start threads or sessions. See surface_linux.cpp.
    external fun shutdownNative()

    /**
     * Resizes the native child window.
     */
    external fun resizeWebView(width: Int, height: Int)

    /**
     * Enables or disables true borderless fullscreen on the native window.
     * Uses per-window state tracking (thread-safe).
     */
    @JvmStatic
    external fun setFullscreen(hwnd: Long, fullscreen: Boolean, x: Int, y: Int, width: Int, height: Int)

    /**
     * Installs/removes the PiP-only top-level window subclass that blocks
     * WM_DPICHANGED to prevent AWT from resizing the PiP window on monitor change.
     */
    @JvmStatic
    external fun setPipSubclass(hwnd: Long, enable: Boolean)

    /** Enables/disables the Linux X11 always-on-top PiP window state and geometry. */
    @JvmStatic
    external fun setPipWindow(hwnd: Long, pip: Boolean, x: Int, y: Int, width: Int, height: Int): Boolean

    /** Starts a compositor-managed move of the Linux X11 top-level window. */
    @JvmStatic
    external fun startWindowDrag(hwnd: Long)

    /** Starts a compositor-managed resize of the Linux X11 top-level window. */
    @JvmStatic
    external fun startWindowResize(hwnd: Long, direction: Int)

    /**
     * Applies DWM window chrome: dark mode title bar and optional caption/border/text colours.
     * No-op on Windows versions that don't support these DWM attributes.
     */
    @JvmStatic
    external fun applyWindowChrome(hwnd: Long, darkMode: Boolean, captionColorRgb: Int, borderColorRgb: Int, textColorRgb: Int)

    /**
     * Parks the Linux GTK/WebKit surface and detaches the current MPV render
     * context. The surface is reused by the next player session.
     */
    external fun destroyWebView()

    /**
     * Forces OS focus onto the WebView container so keyboard events route properly.
     */
    external fun focusWebView()

    /**
     * Sends a JSON state string to the WebView.
     */
    external fun executeScript(script: String)

    /**
     * Posts a JSON message directly to the WebView2 control using postWebMessageAsJson.
     */
    external fun postMessage(json: String)

    /**
     * Posts a JSON message directly to the WebView2 control using postWebMessageAsJson.
     */
    external fun notifyThemeChange(isDarkMode: Boolean)

    /**
     * Initializes an invisible WebView2 instance in the background to warm up Chromium.
     */
    external fun warmupWebView2(controlsUrl: String? = null)

    /**
     * Shuts down the background warmup thread.
     */
    external fun shutdownWebView2Warmup()

    fun loadPlayerUiResource(path: String): String {
        val devFile = java.io.File("desktop-app/src/main/resources$path")
        if (devFile.exists()) {
            val content = runCatching { devFile.readText(Charsets.UTF_8) }.getOrNull()
            if (!content.isNullOrEmpty()) return content
        }
        return NativePlayerBridge::class.java.getResourceAsStream(path)?.use { it.readBytes().toString(Charsets.UTF_8) } ?: ""
    }

    /**
     * Asynchronously warms up the WebView2 environment if running on Windows.
     * Prevents the 2-second stutter and unrendered DOM flashes when opening the player.
     */
    fun preloadAsync() {
        if (!isWindows) return
        if (!preloadStarted.compareAndSet(false, true)) return

        Thread {
            runCatching {
                AppLogger.i("Starting NativePlayerBridge warmup...")
                val webView2DataDir = java.io.File(System.getProperty("java.io.tmpdir"), "CloudStreamWebView2")
                webView2DataDir.mkdirs()
                val tempFile = java.io.File(webView2DataDir, "cloudstream_controls.html")

                val htmlTemplate = loadPlayerUiResource("/player-ui/player.html")
                val cssContent = loadPlayerUiResource("/player-ui/player.css")
                val jsContent = loadPlayerUiResource("/player-ui/player.js")

                val dynamicFontsCss = com.lagradost.cloudstream3.desktop.ui.theme.CustomFontManager.getDynamicFontFaceCss()
                val finalCss = if (dynamicFontsCss.isNotEmpty()) "$dynamicFontsCss\n$cssContent" else cssContent

                val htmlContent = htmlTemplate
                    .replace("/* CSS_INJECT */", finalCss)
                    .replace("/* JS_INJECT */", jsContent)
                    .replace("{{ACCENT_COLOR}}", "#7C4DFF")
                    .replace("{{ACCENT_COLOR_RGB}}", "124, 77, 255")
                    .replace("{{INITIAL_BACKDROP_URL}}", "")
                    .replace("{{INITIAL_BACKDROP_CLASS}}", "")
                    .replace("{{INITIAL_LOGO_URL}}", "")
                    .replace("{{INITIAL_LOGO_STYLE}}", "display: none;")
                    .replace("{{INITIAL_TITLE}}", "CloudStream")
                    .replace("{{INITIAL_TITLE_STYLE}}", "display: block;")
                    .replace("{{INITIAL_SUBTITLE}}", "")
                    .replace("{{INITIAL_SUBTITLE_STYLE}}", "display: none;")

                if (htmlContent.isNotEmpty()) {
                    tempFile.writeText(htmlContent, Charsets.UTF_8)
                }
                val url = if (tempFile.exists()) tempFile.toURI().toString() else null
                warmupWebView2(url)
            }.onFailure {
                AppLogger.e("Failed to warmup NativePlayerBridge: ${it.message}")
            }
        }.apply {
            name = "cloudstream-native-player-preload"
            isDaemon = true
            start()
        }

        Runtime.getRuntime().addShutdownHook(
            Thread {
                runCatching { shutdownWebView2Warmup() }
            }.apply {
                name = "cloudstream-webview2-warmup-shutdown"
            },
        )
    }

    /**
     * Navigates the WebView to a specific URL (like file:///...)
     */
    external fun loadUrl(url: String)

    /**
     * Starts a direct C++ sync timer for mpv properties (bypassing Kotlin loop overhead).
     */
    external fun startMpvSync(mpvHandle: Long)

    /**
     * MPV requires the native C numeric locale to be set to C before mpv_create().
     */
    external fun configureMpvLocale()

    /**
     * Stops the direct C++ sync timer before destroying the mpv handle to prevent dangling pointer crashes.
     */
    external fun stopMpvSync()

    /**
     * Opens the WebView devtools.
     */
    external fun openDevTools()

    /**
     * Registers a listener to receive events from the WebView JS bridge.
     */
    external fun setEventListener(listener: NativePlayerEventListener?)

    interface NativePlayerEventListener {
        fun onPlayerEvent(type: String, value: String)
    }
}
