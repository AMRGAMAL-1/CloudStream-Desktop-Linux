package com.lagradost.cloudstream3.desktop

import com.fasterxml.jackson.annotation.JsonIgnoreProperties
import com.fasterxml.jackson.module.kotlin.jacksonObjectMapper
import com.fasterxml.jackson.module.kotlin.readValue
import com.lagradost.cloudstream3.desktop.updates.AppUpdateChannel
import com.lagradost.common.logging.AppLogger
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request

@JsonIgnoreProperties(ignoreUnknown = true)
data class GitHubRelease(
    val tag_name: String,
    val name: String,
    val body: String?,
    val html_url: String,
    val published_at: String,
    val prerelease: Boolean = false,
    val draft: Boolean = false,
)

object AppUpdater {
    private val client = OkHttpClient()
    private val mapper = jacksonObjectMapper()

    private val _latestRelease = MutableStateFlow<GitHubRelease?>(null)
    val latestRelease: StateFlow<GitHubRelease?> = _latestRelease.asStateFlow()

    private var hasChecked = false

    suspend fun checkForUpdates(force: Boolean = false) {
        if (hasChecked && !force) return
        withContext(Dispatchers.IO) {
            try {
                val repository = AppUpdateChannel.repository()
                if (repository == null) {
                    AppLogger.i("AppUpdater", AppUpdateChannel.statusDescription())
                    _latestRelease.value = null
                    hasChecked = true
                    return@withContext
                }

                val url = "https://api.github.com/repos/$repository/releases?per_page=20"
                val request = Request.Builder()
                    .url(url)
                    .header("Accept", "application/vnd.github.v3+json")
                    .build()
                client.newCall(request).execute().use { response ->
                    if (response.isSuccessful) {
                        response.body?.string()?.let { bodyString ->
                            val releases = mapper.readValue<List<GitHubRelease>>(bodyString)
                            val release = releases
                                .filter {
                                    AppUpdateChannel.isSupportedRelease(
                                        it.tag_name,
                                        it.draft,
                                        it.prerelease,
                                    )
                                }
                                .maxWithOrNull { left, right ->
                                    compareVersions(
                                        AppUpdateChannel.releaseVersion(left.tag_name).orEmpty(),
                                        AppUpdateChannel.releaseVersion(right.tag_name).orEmpty(),
                                    )
                                }
                                ?: return@use
                            val remoteVersion = AppUpdateChannel.releaseVersion(release.tag_name)
                                ?: return@use

                            if (compareVersions(remoteVersion, AppConfig.APP_VERSION) > 0) {
                                _latestRelease.value = release
                            } else {
                                _latestRelease.value = null
                            }
                        }
                    }
                }
                hasChecked = true
            } catch (e: Exception) {
                AppLogger.e("Update check failed", e)
            }
        }
    }

    internal fun compareVersions(v1: String, v2: String): Int {
        val clean1 = v1.removePrefix("v").substringBefore('-')
        val clean2 = v2.removePrefix("v").substringBefore('-')
        val parts1 = clean1.split(".").map { it.filter { c -> c.isDigit() }.toIntOrNull() ?: 0 }
        val parts2 = clean2.split(".").map { it.filter { c -> c.isDigit() }.toIntOrNull() ?: 0 }
        val length = maxOf(parts1.size, parts2.size)
        for (i in 0 until length) {
            val p1 = parts1.getOrElse(i) { 0 }
            val p2 = parts2.getOrElse(i) { 0 }
            if (p1 != p2) return p1.compareTo(p2)
        }
        return 0
    }
}
