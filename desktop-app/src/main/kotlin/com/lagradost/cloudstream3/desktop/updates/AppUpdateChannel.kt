package com.lagradost.cloudstream3.desktop.updates

import com.lagradost.cloudstream3.desktop.AppConfig

/**
 * Selects the application update channel without mixing platform releases.
 *
 * Linux releases are intentionally opt-in. They must be published in the
 * Linux fork and use a tag such as `linux-v0.1.10`; a normal upstream tag
 * such as `v0.1.10` is never accepted by the Linux client.
 */
internal object AppUpdateChannel {
    const val LINUX_TAG_PREFIX = "linux-v"

    private val isLinux = System.getProperty("os.name")
        .orEmpty()
        .contains("linux", ignoreCase = true)

    fun repository(): String? {
        val configured = if (isLinux) {
            AppConfig.LINUX_UPDATE_REPO
        } else {
            AppConfig.WINDOWS_UPDATE_REPO
        }.trim()

        return configured.takeIf { it.isNotEmpty() }
    }

    fun releaseVersion(tag: String): String? {
        val normalized = tag.trim()
        if (normalized.isEmpty()) return null

        if (isLinux) {
            if (!normalized.startsWith(LINUX_TAG_PREFIX, ignoreCase = true)) return null
            return normalized.substring(LINUX_TAG_PREFIX.length)
                .removePrefix("v")
                .takeIf { it.isNotBlank() }
        }

        return normalized.removePrefix("v").takeIf { it.isNotBlank() }
    }

    fun isSupportedRelease(tag: String, draft: Boolean, prerelease: Boolean): Boolean {
        return !draft && !prerelease && releaseVersion(tag) != null
    }

    fun statusDescription(): String {
        return if (isLinux) {
            AppConfig.LINUX_UPDATE_REPO.takeIf { it.isNotBlank() }
                ?.let { "Linux update channel: $it (tags: $LINUX_TAG_PREFIX<version>)" }
                ?: "Linux application updates are disabled until LINUX_UPDATE_REPO is configured"
        } else {
            "Windows update channel: ${AppConfig.WINDOWS_UPDATE_REPO}"
        }
    }
}
