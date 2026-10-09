package com.lagradost.cloudstream3.desktop.init

import com.lagradost.common.logging.AppLogger
import java.awt.GraphicsEnvironment
import java.awt.Rectangle
import java.awt.Window
import javax.swing.JFrame

private const val LINUX_FULLSCREEN_RESTORE_BOUNDS = "cloudstream.linux.fullscreen.restoreBounds"
private const val LINUX_FULLSCREEN_RESTORE_STATE = "cloudstream.linux.fullscreen.restoreExtendedState"
private const val LINUX_FULLSCREEN_RETRY_TIMER = "cloudstream.linux.fullscreen.retryTimer"

interface Kernel32 : com.sun.jna.Library {
    fun SetEnvironmentVariableW(name: com.sun.jna.WString, value: com.sun.jna.WString): Boolean
    companion object {
        val INSTANCE: Kernel32 by lazy {
            com.sun.jna.Native.load("kernel32", Kernel32::class.java) as Kernel32
        }
    }
}

interface ExtUser32 : com.sun.jna.Library {
    fun ReleaseCapture(): Boolean
    companion object {
        val INSTANCE: ExtUser32 by lazy {
            com.sun.jna.Native.load("user32", ExtUser32::class.java) as ExtUser32
        }
    }
}

fun initWindowsEnvironment() {
    // Disable AWT background erasing globally to prevent white flashes when Canvas components mount
    System.setProperty("sun.awt.noerasebackground", "true")

    if (System.getProperty("os.name").lowercase().contains("win")) {
        try {
            Kernel32.INSTANCE.SetEnvironmentVariableW(
                com.sun.jna.WString("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS"),
                com.sun.jna.WString("--allow-file-access-from-files --disable-web-security --allow-running-insecure-content --default-background-color=00000000 --disk-cache-size=1 --disable-application-cache --aggressive-cache-discard"),
            )
        } catch (e: Exception) {
            com.lagradost.common.logging.AppLogger.e("WindowsWindowManager", "initWindowsEnvironment failed", e)
        }
    }
}

// Windows Borderless Fullscreen via C++ JNI bridge (NativePlayerBridge)
fun enterWindowsFullscreen(frame: javax.swing.JFrame) {
    if (!System.getProperty("os.name", "").lowercase().contains("win")) {
        enterLinuxFullscreen(frame)
        return
    }
    try {
        val hwnd = com.sun.jna.Native.getComponentID(frame)
        com.lagradost.cloudstream3.desktop.player.webview.NativePlayerBridge.setFullscreen(
            hwnd = hwnd,
            fullscreen = true,
            x = 0,
            y = 0,
            width = 0,
            height = 0,
        )
        AppLogger.i("Entered borderless fullscreen via C++ bridge (hwnd=0x${hwnd.toString(16)})")
    } catch (e: Exception) {
        AppLogger.e("enterWindowsFullscreen failed: ${e.message}")
        AppLogger.e("WindowsWindowManager", "enterWindowsFullscreen failed", e)
        runCatching {
            java.awt.GraphicsEnvironment.getLocalGraphicsEnvironment().defaultScreenDevice.fullScreenWindow = frame
        }
    }
}

fun exitWindowsFullscreen(frame: javax.swing.JFrame) {
    if (!System.getProperty("os.name", "").lowercase().contains("win")) {
        exitLinuxFullscreen(frame)
        return
    }
    try {
        val hwnd = com.sun.jna.Native.getComponentID(frame)
        com.lagradost.cloudstream3.desktop.player.webview.NativePlayerBridge.setFullscreen(
            hwnd = hwnd,
            fullscreen = false,
            x = 0,
            y = 0,
            width = 0,
            height = 0,
        )
        AppLogger.i("Exited borderless fullscreen via C++ bridge (hwnd=0x${hwnd.toString(16)})")
    } catch (e: Exception) {
        AppLogger.e("exitWindowsFullscreen failed: ${e.message}")
        AppLogger.e("WindowsWindowManager", "exitWindowsFullscreen failed", e)
        runCatching {
            java.awt.GraphicsEnvironment.getLocalGraphicsEnvironment().defaultScreenDevice.fullScreenWindow = null
        }
    }
}

/**
 * Treeland/XWayland may hide the desktop panel when AWT enters fullscreen but
 * leave the client surface at the previous maximum-window work area. Explicitly
 * apply the complete monitor bounds after entering fullscreen so the hidden
 * panel area is actually covered by the application surface.
 */
private fun enterLinuxFullscreen(frame: JFrame) {
    val device = frame.graphicsConfiguration?.device
        ?: GraphicsEnvironment.getLocalGraphicsEnvironment().defaultScreenDevice
    val monitorBounds = frame.graphicsConfiguration?.bounds?.let { Rectangle(it) }
        ?: Rectangle(device.defaultConfiguration.bounds)

    if (frame.rootPane.getClientProperty(LINUX_FULLSCREEN_RESTORE_BOUNDS) == null) {
        frame.rootPane.putClientProperty(
            LINUX_FULLSCREEN_RESTORE_BOUNDS,
            Rectangle(frame.bounds),
        )
        // Remember whether the window was maximized: exiting fullscreen must
        // restore MAXIMIZED_BOTH, not just the old bounds (a maximized window
        // forced to NORMAL + work-area bounds never behaves like other Linux
        // apps afterwards — minimize/restore included).
        frame.rootPane.putClientProperty(
            LINUX_FULLSCREEN_RESTORE_STATE,
            frame.extendedState,
        )
    }

    runCatching {
        val hwnd = com.sun.jna.Native.getComponentID(frame)
        frame.extendedState = JFrame.NORMAL
        // Apply the client geometry first, then ask the compositor for EWMH
        // fullscreen. On Treeland/XWayland the configure generated by
        // setBounds can otherwise arrive after the fullscreen request and
        // replace the monitor rectangle with the old work area.
        frame.setBounds(monitorBounds)
        frame.validate()
        com.lagradost.cloudstream3.desktop.player.webview.NativePlayerBridge.setFullscreen(
            hwnd = hwnd,
            fullscreen = true,
            x = monitorBounds.x,
            y = monitorBounds.y,
            width = monitorBounds.width,
            height = monitorBounds.height,
        )
        frame.toFront()

        // Treeland may issue one final work-area configure after the first
        // request. Reassert the same request once, after that configure has
        // settled; this is intentionally bounded to one retry.
        (frame.rootPane.getClientProperty(LINUX_FULLSCREEN_RETRY_TIMER) as? javax.swing.Timer)?.stop()
        val retryTimer = javax.swing.Timer(180, null)
        retryTimer.isRepeats = false
        retryTimer.addActionListener {
            // Never steal the window back while it is minimized: the timer
            // condition (displayable + visible) stays true for an iconified
            // frame and the reassert would yank it out of minimize.
            if (frame.isDisplayable && frame.isVisible &&
                (frame.extendedState and JFrame.ICONIFIED) == 0
            ) {
                runCatching {
                    com.lagradost.cloudstream3.desktop.player.webview.NativePlayerBridge.setFullscreen(
                        hwnd = com.sun.jna.Native.getComponentID(frame),
                        fullscreen = true,
                        x = monitorBounds.x,
                        y = monitorBounds.y,
                        width = monitorBounds.width,
                        height = monitorBounds.height,
                    )
                    frame.setBounds(monitorBounds)
                    frame.validate()
                    frame.toFront()
                    AppLogger.i("LinuxFullscreen", "Reasserted fullscreen bounds=$monitorBounds")
                }.onFailure { error ->
                    AppLogger.e("LinuxFullscreen", "Fullscreen reassert failed: ${error.message}", error)
                }
            }
            frame.rootPane.putClientProperty(LINUX_FULLSCREEN_RETRY_TIMER, null)
        }
        frame.rootPane.putClientProperty(LINUX_FULLSCREEN_RETRY_TIMER, retryTimer)
        retryTimer.start()
        AppLogger.i(
            "LinuxFullscreen",
            "Entered fullscreen bounds=$monitorBounds device=${device.getIDstring()}",
        )
    }.onFailure { error ->
        AppLogger.e("LinuxFullscreen", "Failed to enter fullscreen: ${error.message}", error)
        // Fallback for systems where the native bridge cannot be loaded.
        runCatching {
            device.fullScreenWindow = frame
            frame.setBounds(monitorBounds)
            frame.validate()
        }
    }
}

private fun exitLinuxFullscreen(frame: JFrame) {
    val device = frame.graphicsConfiguration?.device
        ?: GraphicsEnvironment.getLocalGraphicsEnvironment().defaultScreenDevice
    (frame.rootPane.getClientProperty(LINUX_FULLSCREEN_RETRY_TIMER) as? javax.swing.Timer)?.let {
        it.stop()
        frame.rootPane.putClientProperty(LINUX_FULLSCREEN_RETRY_TIMER, null)
    }
    runCatching {
        val hwnd = com.sun.jna.Native.getComponentID(frame)
        com.lagradost.cloudstream3.desktop.player.webview.NativePlayerBridge.setFullscreen(
            hwnd = hwnd,
            fullscreen = false,
            x = 0,
            y = 0,
            width = 0,
            height = 0,
        )
    }.onFailure {
        runCatching { device.fullScreenWindow = null }
    }

    val restoreBounds = frame.rootPane.getClientProperty(LINUX_FULLSCREEN_RESTORE_BOUNDS) as? Rectangle
    val restoreState = frame.rootPane.getClientProperty(LINUX_FULLSCREEN_RESTORE_STATE) as? Int
    frame.rootPane.putClientProperty(LINUX_FULLSCREEN_RESTORE_BOUNDS, null)
    frame.rootPane.putClientProperty(LINUX_FULLSCREEN_RESTORE_STATE, null)
    if (restoreBounds != null) {
        runCatching {
            frame.setBounds(restoreBounds)
            // Restore the maximized state when that is what we had: a plain
            // NORMAL + bounds leaves the window manager treating the app as a
            // floating window (broken minimize/restore vs other Linux apps).
            if (restoreState != null &&
                (restoreState and JFrame.MAXIMIZED_BOTH) == JFrame.MAXIMIZED_BOTH
            ) {
                frame.extendedState = JFrame.MAXIMIZED_BOTH
            }
            frame.validate()
            frame.toFront()
        }
    }
    AppLogger.i("LinuxFullscreen", "Exited fullscreen restoreBounds=$restoreBounds restoreState=$restoreState")
}

// DWM Dark mode title bar + caption colour (via C++ bridge)
private const val WINDOW_BACKGROUND_RGB = 0x0D0D0D
private const val WINDOW_TEXT_RGB = 0xF5F7F8

fun setWindowsDarkMode(window: java.awt.Window) {
    if (!System.getProperty("os.name").lowercase().contains("win")) return
    if (!window.isDisplayable) return
    try {
        val hwnd = com.sun.jna.Native.getComponentID(window)
        com.lagradost.cloudstream3.desktop.player.webview.NativePlayerBridge.applyWindowChrome(
            hwnd = hwnd,
            darkMode = true,
            captionColorRgb = WINDOW_BACKGROUND_RGB,
            borderColorRgb = WINDOW_BACKGROUND_RGB,
            textColorRgb = WINDOW_TEXT_RGB,
        )
    } catch (e: Throwable) {
        com.lagradost.common.logging.AppLogger.e("WindowsWindowManager", "setWindowsDarkMode failed", e)
    }
}

private var prePipBounds: java.awt.Rectangle? = null
private var prePipExtendedState: Int? = null

fun setNativePipMode(window: java.awt.Window, enable: Boolean) {
    if (!window.isDisplayable) return

    val isWindows = System.getProperty("os.name", "").lowercase().contains("win")
    val isLinux = System.getProperty("os.name", "").lowercase().contains("linux")

    if (isLinux) {
        setLinuxPipMode(window, enable)
        return
    }
    if (!isWindows) return

    try {
        val hwnd = com.sun.jna.Native.getComponentID(window)
        if (enable) {
            prePipBounds = window.bounds
            val bounds = window.graphicsConfiguration.bounds
            val scaleX = window.graphicsConfiguration.defaultTransform.scaleX
            val scaleY = window.graphicsConfiguration.defaultTransform.scaleY

            val w = (400 * scaleX).toInt()
            val h_size = (225 * scaleY).toInt()
            val x = bounds.x + bounds.width - w - (20 * scaleX).toInt()
            val y = bounds.y + bounds.height - h_size - (40 * scaleY).toInt()

            // Safely strip borders using C++ bridge
            com.lagradost.cloudstream3.desktop.player.webview.NativePlayerBridge.setFullscreen(
                hwnd = hwnd,
                fullscreen = true,
                x = 0,
                y = 0,
                width = 0,
                height = 0,
            )
            // Install PiP-only subclass to block WM_DPICHANGED on monitor drag
            com.lagradost.cloudstream3.desktop.player.webview.NativePlayerBridge.setPipSubclass(hwnd, true)

            val hWin = com.sun.jna.platform.win32.WinDef.HWND(com.sun.jna.Pointer(hwnd))
            val hwndTopMost = com.sun.jna.platform.win32.WinDef.HWND(com.sun.jna.Pointer(-1L))

            // SWP_NOZORDER is 0x0004, SWP_SHOWWINDOW is 0x0040
            com.sun.jna.platform.win32.User32.INSTANCE.SetWindowPos(
                hWin,
                hwndTopMost,
                x,
                y,
                w,
                h_size,
                0x0040,
            )
        } else {
            // Remove PiP subclass before restoring fullscreen state
            com.lagradost.cloudstream3.desktop.player.webview.NativePlayerBridge.setPipSubclass(hwnd, false)
            com.lagradost.cloudstream3.desktop.player.webview.NativePlayerBridge.setFullscreen(
                hwnd = hwnd,
                fullscreen = false,
                x = 0,
                y = 0,
                width = 0,
                height = 0,
            )
            val hWin = com.sun.jna.platform.win32.WinDef.HWND(com.sun.jna.Pointer(hwnd))
            val hwndNoTopMost = com.sun.jna.platform.win32.WinDef.HWND(com.sun.jna.Pointer(-2L))
            val prev = prePipBounds
            if (prev != null) {
                com.sun.jna.platform.win32.User32.INSTANCE.SetWindowPos(
                    hWin,
                    hwndNoTopMost,
                    prev.x,
                    prev.y,
                    prev.width,
                    prev.height,
                    0x0040,
                )
                prePipBounds = null
            } else {
                com.sun.jna.platform.win32.User32.INSTANCE.SetWindowPos(
                    hWin,
                    hwndNoTopMost,
                    0,
                    0,
                    0,
                    0,
                    0x0003,
                )
            }
        }
    } catch (e: Throwable) {
        com.lagradost.common.logging.AppLogger.e("WindowsWindowManager", "setNativePipMode failed", e)
    }
}

private fun setLinuxPipMode(window: java.awt.Window, enable: Boolean) {
    val frame = window as? JFrame ?: return
    try {
        val hwnd = com.sun.jna.Native.getComponentID(frame)
        if (enable) {
            if (prePipBounds == null) {
                prePipBounds = java.awt.Rectangle(frame.bounds)
                prePipExtendedState = frame.extendedState
            }

            val bounds = frame.graphicsConfiguration?.bounds
                ?: GraphicsEnvironment.getLocalGraphicsEnvironment().defaultScreenDevice.defaultConfiguration.bounds
            val scaleX = frame.graphicsConfiguration?.defaultTransform?.scaleX ?: 1.0
            val scaleY = frame.graphicsConfiguration?.defaultTransform?.scaleY ?: 1.0
            val pipWidth = (400 * scaleX).toInt().coerceAtLeast(280)
            val pipHeight = (225 * scaleY).toInt().coerceAtLeast(180)
            val pipX = bounds.x + bounds.width - pipWidth - (20 * scaleX).toInt()
            val pipY = bounds.y + bounds.height - pipHeight - (40 * scaleY).toInt()

            // A maximized AWT state would override the small PiP geometry.
            frame.extendedState = JFrame.NORMAL
            frame.setBounds(pipX, pipY, pipWidth, pipHeight)
            frame.validate()

            val applied = com.lagradost.cloudstream3.desktop.player.webview.NativePlayerBridge.setPipWindow(
                hwnd, true, pipX, pipY, pipWidth, pipHeight,
            )
            if (!applied) {
                AppLogger.e("LinuxPiP", "Native X11 PiP request was not accepted")
            }
            frame.toFront()
            AppLogger.i("LinuxPiP", "Entered PiP bounds=${frame.bounds}")
        } else {
            // Remove the compositor's always-on-top state before restoring the app.
            com.lagradost.cloudstream3.desktop.player.webview.NativePlayerBridge.setPipWindow(
                hwnd, false, 0, 0, 0, 0,
            )
            frame.extendedState = JFrame.NORMAL
            prePipBounds?.let { frame.setBounds(java.awt.Rectangle(it)) }
            frame.validate()
            frame.toFront()

            // Treeland uses its own title-bar maximize state, so restoring the
            // old AWT maximized flag is only safe on non-Treeland X11 sessions.
            if (prePipExtendedState == JFrame.MAXIMIZED_BOTH &&
                !System.getenv("XDG_CURRENT_DESKTOP").orEmpty().contains("treeland", ignoreCase = true)
            ) {
                frame.extendedState = prePipExtendedState ?: JFrame.NORMAL
            }
            AppLogger.i("LinuxPiP", "Exited PiP restoreBounds=${prePipBounds}")
            prePipBounds = null
            prePipExtendedState = null
        }
    } catch (e: Throwable) {
        AppLogger.e("LinuxPiP", "setLinuxPipMode failed: ${e.message}", e)
    }
}
