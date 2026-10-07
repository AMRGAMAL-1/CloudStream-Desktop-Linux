package com.lagradost.cloudstream3.desktop

import com.lagradost.common.platform.PlatformPaths
import com.lagradost.cloudstream3.desktop.updates.AppUpdateChannel
import java.io.File
import java.util.concurrent.TimeUnit

/**
 * Read-only diagnostics for Linux support and bug reports.
 *
 * This intentionally uses fixed argv arrays rather than a shell. It reports
 * capability and dependency information without exposing the user's home path
 * or arbitrary environment variables.
 */
object LinuxDiagnostics {
    private val home = File(System.getProperty("user.home", "."))

    fun printVersion(): String = "CloudStream Desktop ${AppConfig.APP_VERSION} (${PlatformPaths.currentOS})"

    fun report(): String {
        val osRelease = readOsRelease()
        val sessionType = System.getenv("XDG_SESSION_TYPE")?.ifBlank { null } ?: "unknown"
        val display = if (System.getenv("DISPLAY").isNullOrBlank()) "unset" else "set"
        val waylandDisplay = if (System.getenv("WAYLAND_DISPLAY").isNullOrBlank()) "unset" else "set"
        val libraries = listOf(
            "GTK 3" to "libgtk-3.so.0",
            "WebKitGTK 4.1" to "libwebkit2gtk-4.1.so.0",
            "MPV" to "libmpv.so.2 or libmpv.so.1",
            "libVLC" to "libvlc.so.5",
            "X11" to "libX11.so.6",
            "OpenGL" to "libGL.so.1",
            "EGL" to "libEGL.so.1",
            "Wayland client" to "libwayland-client.so.0",
        )
        val bridge = nativeBridgeFile()
        val lddOutput = bridge?.let {
            command("/usr/bin/ldd", it.absolutePath)
                ?: command("/usr/sbin/ldd", it.absolutePath)
        }
        val missingBridgeLibraries = lddOutput
            ?.lineSequence()
            ?.filter { it.contains("not found", ignoreCase = true) }
            ?.joinToString("; ")
            ?.ifBlank { null }

        return buildString {
            appendLine("CloudStream Desktop diagnostics")
            appendLine("version=${AppConfig.APP_VERSION}")
            appendLine("os=${osRelease["PRETTY_NAME"] ?: System.getProperty("os.name", "unknown")}")
            appendLine("kernel=${(command("/usr/bin/uname", "-sr") ?: command("/bin/uname", "-sr"))?.trim().orEmpty().ifBlank { "unknown" }}")
            appendLine("architecture=${System.getProperty("os.arch", "unknown")} / ${(command("/usr/bin/uname", "-m") ?: command("/bin/uname", "-m"))?.trim().orEmpty().ifBlank { "unknown" }}")
            appendLine("desktop=${System.getenv("XDG_CURRENT_DESKTOP") ?: "unknown"}")
            appendLine("session=$sessionType display=$display wayland_display=$waylandDisplay")
            appendLine("java=${System.getProperty("java.version", "unknown")}")
            appendLine("java_vm=${System.getProperty("java.vm.name", "unknown")}")
            appendLine("data_dir=${displayPath(PlatformPaths.appDataDir)}")
            appendLine("config_dir=${displayPath(PlatformPaths.configDir)}")
            appendLine("cache_dir=${displayPath(PlatformPaths.cacheDir)}")
            appendLine("logs_dir=${displayPath(PlatformPaths.logsDir)}")
            appendLine("native_bridge=${bridge?.let(::displayPath) ?: "missing"}")
            appendLine(
                "native_bridge_missing=" + when {
                    bridge == null -> "not-inspected"
                    missingBridgeLibraries != null -> missingBridgeLibraries
                    else -> "none"
                }
            )
            appendLine("libraries:")
            libraries.forEach { (label, soname) ->
                val present = if (label == "MPV") {
                    hasLibrary("libmpv.so.2") || hasLibrary("libmpv.so.1")
                } else {
                    hasLibrary(soname)
                }
                appendLine("  $label=$soname:${if (present) "present" else "missing"}")
            }
            appendLine("versions:")
            appendLine("  GTK 3=${pkgConfigVersion("gtk+-3.0")}")
            appendLine("  WebKitGTK 4.1=${pkgConfigVersion("webkit2gtk-4.1")}")
            appendLine("  MPV=${pkgConfigVersion("mpv") ?: executableVersion("mpv")}")
            appendLine("  libVLC=${libVlcStatus()}")
            appendLine("  OpenGL=${openGlStatus()}")
            appendLine("plugins:")
            appendLine("  VLC=${vlcPluginStatus()}")
            appendLine("native_rendering=GTK/WebKitGTK + MPV OpenGL Render API")
            appendLine("wayland_support=${if (sessionType.equals("wayland", true) && display == "set") "XWayland compatibility path" else if (sessionType.equals("wayland", true)) "requires XWayland DISPLAY" else "X11-compatible path"}")
            appendLine("audio=${audioBackendStatus()}")
            appendLine("update_channel=${AppUpdateChannel.statusDescription()}")
        }
    }

    private fun readOsRelease(): Map<String, String> {
        val file = File("/etc/os-release")
        if (!file.isFile) return emptyMap()
        return runCatching {
            file.readLines()
                .mapNotNull { line ->
                    val separator = line.indexOf('=')
                    if (separator <= 0) return@mapNotNull null
                    val key = line.substring(0, separator)
                    val value = line.substring(separator + 1).trim().trim('"')
                    key to value
                }
                .toMap()
        }.getOrDefault(emptyMap())
    }

    private fun nativeBridgeFile(): File? {
        val resourcesDir = System.getProperty("compose.application.resources.dir")
        val libraryPathCandidates = System.getProperty("java.library.path", "")
            .split(File.pathSeparator)
            .filter { it.isNotBlank() }
            .map { File(it, "libplayer_bridge.so") }
        val javaHome = File(System.getProperty("java.home", "."))
        val candidates = listOfNotNull(
            resourcesDir?.let { File(it, "jni/libplayer_bridge.so") },
            *libraryPathCandidates.toTypedArray(),
            javaHome.resolve("../app/resources/jni/libplayer_bridge.so").normalize(),
            javaHome.resolve("../../lib/app/resources/jni/libplayer_bridge.so").normalize(),
            File("desktop-app/appResources/linux/jni/libplayer_bridge.so"),
        )
        return candidates.firstOrNull { it.isFile }
    }

    private fun hasLibrary(soname: String): Boolean {
        // Multiarch Debian layouts keep the sonames under the architecture
        // directory, and some sandboxed runtimes cannot execute ldconfig.
        val commonLibraryDirectories = listOf(
            "/lib",
            "/usr/lib",
            "/lib64",
            "/usr/lib64",
            "/lib/x86_64-linux-gnu",
            "/usr/lib/x86_64-linux-gnu",
            "/lib/aarch64-linux-gnu",
            "/usr/lib/aarch64-linux-gnu",
        )
        if (commonLibraryDirectories.any { File(it, soname).isFile }) return true

        // Compose's bundled runtime may provide a reduced PATH without /sbin,
        // while ldconfig is commonly installed there on Debian-family systems.
       val output = command("/sbin/ldconfig", "-p")
           ?: command("/usr/sbin/ldconfig", "-p")
            ?: command("/usr/bin/ldconfig", "-p")
           ?: return false
        return output.lineSequence().any { it.contains(soname) }
    }

    private fun pkgConfigVersion(module: String): String? =
        listOf("/usr/bin/pkg-config", "/usr/local/bin/pkg-config")
            .asSequence()
            .mapNotNull { command(it, "--modversion", module)?.trim() }
            .firstOrNull { it.isNotBlank() }

    private fun executableVersion(executable: String): String =
        listOf("/usr/bin/$executable", "/usr/local/bin/$executable", "/bin/$executable", "/usr/sbin/$executable", "/sbin/$executable")
            .asSequence()
            .mapNotNull { command(it, "--version")?.lineSequence()?.firstOrNull()?.trim() }
            .firstOrNull { it.isNotBlank() }
            ?: "unavailable"

    private fun libVlcStatus(): String =
        executableVersion("vlc").takeUnless { it == "unavailable" }
            ?: if (hasLibrary("libvlc.so.5")) "runtime present (vlc CLI unavailable)" else "unavailable"

    private fun openGlStatus(): String {
        val output = listOf("/usr/bin/glxinfo", "/usr/local/bin/glxinfo")
            .asSequence()
            .mapNotNull { command(it, "-B") }
            .firstOrNull()
        return output?.lineSequence()
            ?.firstOrNull { it.trimStart().startsWith("OpenGL version string:") }
            ?.substringAfter(":")
            ?.trim()
            ?: if (hasLibrary("libGL.so.1")) "libGL present (glxinfo unavailable)" else "unavailable"
    }

    private fun vlcPluginStatus(): String {
       val configured = System.getenv("VLC_PLUGIN_PATH")
           ?.takeIf { it.isNotBlank() }
            ?.let { File(it).takeIf(File::isDirectory)?.let(::displayPath) }
        if (configured != null) return "present:$configured"

        val candidates = listOf(
            "/usr/lib/vlc/plugins",
            "/usr/lib64/vlc/plugins",
            "/usr/lib/x86_64-linux-gnu/vlc/plugins",
            "/usr/lib/aarch64-linux-gnu/vlc/plugins",
        )
        return candidates.firstOrNull { File(it).isDirectory }
            ?.let { "present:$it" }
            ?: "not-detected (set VLC_PLUGIN_PATH for a custom install)"
    }

    private fun audioBackendStatus(): String {
        val pipewire = executableExists("pw-cli") || !System.getenv("PIPEWIRE_REMOTE").isNullOrBlank()
        val pulse = executableExists("pactl") || !System.getenv("PULSE_SERVER").isNullOrBlank()
        val alsa = executableExists("aplay")
        return buildList {
            if (pipewire) add("PipeWire")
            if (pulse) add("PulseAudio/Pulse compatibility")
            if (alsa) add("ALSA")
        }.takeIf { it.isNotEmpty() }?.joinToString(", ")
            ?: "not-detected (libmpv/libVLC choose their host audio backend)"
    }

    private fun executableExists(executable: String): Boolean =
        listOf("/usr/bin/$executable", "/usr/local/bin/$executable", "/bin/$executable", "/usr/sbin/$executable", "/sbin/$executable")
            .any { candidate -> command(candidate, "--help") != null }

    private fun command(vararg args: String): String? {
        return runCatching {
            val process = ProcessBuilder(args.toList())
                .redirectErrorStream(true)
                .start()
            if (!process.waitFor(3, TimeUnit.SECONDS)) {
                process.destroyForcibly()
                return null
            }
            val output = process.inputStream.bufferedReader().use { it.readText().take(64 * 1024) }
            if (process.exitValue() == 0) output else null
        }.getOrNull()
    }

    private fun displayPath(file: File): String {
        val path = runCatching { file.canonicalPath }.getOrDefault(file.absolutePath)
        val homePath = runCatching { home.canonicalPath }.getOrDefault(home.absolutePath)
        return if (path == homePath || path.startsWith("$homePath/")) {
            "~" + path.removePrefix(homePath)
        } else {
            path
        }
    }
}
