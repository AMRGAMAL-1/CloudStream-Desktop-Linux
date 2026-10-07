package com.lagradost.common.platform

import java.io.File

/**
 * Cross-platform path resolution for the CloudStream Desktop client.
 *
 * Replaces all direct `System.getenv("APPDATA")` calls with proper
 * OS-aware paths that work on Windows, macOS, and Linux.
 *
 * Directory layout per OS:
 *   Windows: %APPDATA%/CloudStreamDesktop/
 *   macOS:   ~/Library/Application Support/CloudStreamDesktop/
 *   Linux:   ~/.local/share/CloudStreamDesktop/
 */
object PlatformPaths {
    enum class OS { WINDOWS, MACOS, LINUX, UNKNOWN }

    private val customDataDir: File? by lazy {
        System.getProperty("cloudstream.data.dir")
            ?.takeIf { it.isNotBlank() }
            ?.let(::File)
    }

    private val portableDataDir: File? by lazy {
        val workingDirectory = File(System.getProperty("user.dir", "."))
        if (File(workingDirectory, "portable.txt").isFile) {
            File(workingDirectory, "CloudStreamData")
        } else {
            null
        }
    }

    private fun createDirectory(directory: File): File = directory.also {
        if (!it.exists()) it.mkdirs()
    }

    private fun homeDirectory(): File = File(System.getProperty("user.home", "."))

    private fun linuxDirectory(environmentName: String, fallback: File): File {
        val configured = System.getenv(environmentName)
            ?.takeIf { it.isNotBlank() }
            ?.let(::File)
        return configured ?: fallback
    }

    val currentOS: OS by lazy {
        val osName = System.getProperty("os.name").lowercase()
        when {
            osName.contains("win") -> OS.WINDOWS
            osName.contains("mac") -> OS.MACOS
            osName.contains("nix") || osName.contains("nux") || osName.contains("aix") -> OS.LINUX
            else -> OS.UNKNOWN
        }
    }

    /** The base application data directory, OS-aware. */
    val appDataDir: File by lazy {
        customDataDir?.let { return@lazy createDirectory(it) }
        portableDataDir?.let { return@lazy createDirectory(it) }

        val basePath =
            when (currentOS) {
                OS.WINDOWS -> {
                    val appData = System.getenv("APPDATA")
                    if (!appData.isNullOrEmpty()) appData else System.getProperty("user.home")
                }
                OS.MACOS -> System.getProperty("user.home") + "/Library/Application Support"
                OS.LINUX -> linuxDirectory(
                    "XDG_DATA_HOME",
                    File(homeDirectory(), ".local/share"),
                ).path
                OS.UNKNOWN -> System.getProperty("user.home")
            }
        createDirectory(File(basePath, "CloudStreamDesktop"))
    }

    /** Directory for user configuration on Linux, following the XDG Base Directory specification. */
    val configDir: File by lazy {
        when {
            customDataDir != null || portableDataDir != null -> createDirectory(File(appDataDir, "config"))
            currentOS == OS.LINUX -> createDirectory(
                File(
                    linuxDirectory("XDG_CONFIG_HOME", File(homeDirectory(), ".config")),
                    "CloudStreamDesktop",
                ),
            )
            else -> appDataDir
        }
    }

    /** Directory for persistent data store (bookmarks, history, preferences). */
    val dataDir: File by lazy {
        File(appDataDir, "data").also { it.mkdirs() }
    }

    /** Directory for SharedPreferences JSON files. */
    val sharedPrefsDir: File by lazy {
        if (currentOS == OS.LINUX && customDataDir == null && portableDataDir == null) {
            val xdgDirectory = File(configDir, "shared_prefs")
            val legacyDirectory = File(appDataDir, "shared_prefs")
            // Keep existing installations working without an explicit migration:
            // use the legacy location until the user data is next written there.
            if (!xdgDirectory.exists() && legacyDirectory.exists()) {
                legacyDirectory
            } else {
                createDirectory(xdgDirectory)
            }
        } else {
            createDirectory(File(appDataDir, "shared_prefs"))
        }
    }

    /** Directory for installed extensions/plugins. */
    val extensionsDir: File by lazy {
        File(appDataDir, "Extensions").also { it.mkdirs() }
    }

    /** Directory for cache files. */
    val cacheDir: File by lazy {
        if (currentOS == OS.LINUX && customDataDir == null && portableDataDir == null) {
            createDirectory(
                File(
                    linuxDirectory("XDG_CACHE_HOME", File(homeDirectory(), ".cache")),
                    "CloudStreamDesktop",
                ),
            )
        } else {
            createDirectory(File(appDataDir, "cache"))
        }
    }

    /** Directory for log files. */
    val logsDir: File by lazy {
        if (currentOS == OS.LINUX && customDataDir == null && portableDataDir == null) {
            createDirectory(
                File(
                    linuxDirectory("XDG_STATE_HOME", File(homeDirectory(), ".local/state")),
                    "CloudStreamDesktop/logs",
                ),
            )
        } else {
            createDirectory(File(appDataDir, "logs"))
        }
    }

    /** Directory for extracted MPV shaders. */
    val shadersDir: File by lazy {
        File(appDataDir, "shaders").also { it.mkdirs() }
    }

    /** Directory for user-provided custom fonts. */
    val fontsDir: File by lazy {
        File(appDataDir, "fonts").also { it.mkdirs() }
    }

    /** Directory for video player screenshots. */
    val screenshotsDir: File
        get() {
            val customPath = com.lagradost.common.storage.DesktopDataStore.getKey<String>("player_screenshot_dir")
            if (!customPath.isNullOrBlank()) {
                val f = File(customPath)
                if (f.exists() || f.mkdirs()) return f
            }
            val userHome = System.getProperty("user.home") ?: ""
            val picturesDir = File(userHome, "Pictures")
            val targetDir = if (picturesDir.exists() && picturesDir.isDirectory) {
                File(picturesDir, "CloudStream")
            } else {
                File(appDataDir, "screenshots")
            }
            return targetDir.also { it.mkdirs() }
        }
}
