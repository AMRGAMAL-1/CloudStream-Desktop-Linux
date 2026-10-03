package com.lagradost.cloudstream3.desktop.subtitles

import com.fasterxml.jackson.module.kotlin.jacksonObjectMapper
import com.lagradost.cloudstream3.SubtitleFile
import com.lagradost.cloudstream3.app
import com.lagradost.cloudstream3.desktop.player.LanguagePriorityHelper
import com.lagradost.cloudstream3.desktop.player.PlayerConfig
import com.lagradost.cloudstream3.newSubtitleFile
import com.lagradost.common.logging.AppLogger
import com.lagradost.common.platform.PlatformPaths
import com.lagradost.common.storage.DesktopDataStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.MediaType.Companion.toMediaTypeOrNull
import okhttp3.RequestBody.Companion.toRequestBody
import java.io.ByteArrayInputStream
import java.io.File
import java.net.URLEncoder
import java.util.zip.ZipInputStream

object SubsourceSubtitleProvider {
    private const val TAG = "SubsourceSubtitles"
    private const val OFFICIAL_API_URL = "https://api.subsource.net/api/v1"
    private const val BASIC_API_URL = "https://api.subsource.net/api"

    private val mapper = jacksonObjectMapper()
    private val jsonMediaType = "application/json; charset=utf-8".toMediaTypeOrNull()

    private val BASIC_HEADERS = mapOf(
        "Accept" to "application/json, text/plain, */*",
        "Content-Type" to "application/json",
        "Origin" to "https://subsource.net",
        "Referer" to "https://subsource.net/",
        "User-Agent" to "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36",
    )

    suspend fun searchAndCollectSubtitles(
        title: String?,
        imdbId: String?,
        season: Int?,
        episode: Int?,
        year: Int?,
        isSeries: Boolean,
        onSubtitle: (SubtitleFile) -> Unit,
    ) = withContext(Dispatchers.IO) {
        val enabled = DesktopDataStore.getKey<Boolean>(PlayerConfig.PREF_SUBSOURCE_ENABLED) ?: true
        if (!enabled) return@withContext

        val cleanTitle = title?.trim()?.takeIf { it.isNotBlank() } ?: return@withContext
        val apiKey = com.lagradost.cloudstream3.syncproviders.AccountManager.cachedAccounts["subsource"]?.firstOrNull()?.token?.accessToken?.trim()?.takeIf { it.isNotBlank() }
            ?: DesktopDataStore.getKey<String>(PlayerConfig.PREF_SUBSOURCE_API_KEY)?.trim()?.takeIf { it.isNotBlank() }

        var collectedCount = 0

        // 1. If user has configured an API key, attempt Official API mode first
        if (apiKey != null) {
            try {
                collectedCount = searchOfficialApi(apiKey, cleanTitle, imdbId, season, episode, year, isSeries, onSubtitle)
            } catch (e: Exception) {
                AppLogger.w(TAG, "Official API query failed: ${e.message}, falling back to basic mode")
            }
        } else {
            AppLogger.i(TAG, "SubSource API key not configured. Obtain a free key from subsource.net/my-profile to enable official SubSource subtitles.")
        }

        // 2. If no API key or official query yielded no results, query Basic public mode (if available)
        if (collectedCount == 0 && apiKey == null) {
            try {
                searchBasicApi(cleanTitle, season, episode, year, isSeries, onSubtitle)
            } catch (e: Exception) {
                AppLogger.w(TAG, "Basic API query failed: ${e.message}")
            }
        }
    }

    private fun sanitizeTitle(rawTitle: String): String {
        return rawTitle
            .replace(Regex("""(?i)\s*-\s*s\d+e\d+.*"""), "")
            .replace(Regex("""(?i)\s*-\s*\d+x\d+.*"""), "")
            .replace(Regex("""(?i)\s*s\d+e\d+.*"""), "")
            .replace(Regex("""\s+"""), " ")
            .trim()
    }

    private fun toSubsourceLanguage(lang: String?): String? {
        if (lang.isNullOrBlank() || lang.equals("all", ignoreCase = true)) return null
        val norm = LanguageNormalizer.normalize(lang)
        return norm.displayName.lowercase()
    }

    private fun extractReleaseName(item: com.fasterxml.jackson.databind.JsonNode, fallback: String): String {
        val releaseInfo = item.get("releaseInfo")
        return when {
            releaseInfo?.isArray == true -> {
                val parts = (0 until releaseInfo.size()).mapNotNull { releaseInfo.get(it)?.asText()?.takeIf { s -> s.isNotBlank() } }
                if (parts.isNotEmpty()) parts.joinToString(" ") else fallback
            }
            releaseInfo?.isTextual == true -> releaseInfo.asText().takeIf { it.isNotBlank() } ?: fallback
            else -> item.get("releaseName")?.asText()?.takeIf { it.isNotBlank() } ?: fallback
        }
    }

    /**
     * Standard subtitle search pipeline integration invoked concurrently by SubtitlePipeline.
     */
    suspend fun searchPipeline(
        query: String,
        imdbId: String? = null,
        season: Int? = null,
        episode: Int? = null,
        lang: String? = null,
        apiKeyOverride: String? = null,
    ): List<Map<String, Any?>> = withContext(Dispatchers.IO) {
        val enabled = DesktopDataStore.getKey<Boolean>(PlayerConfig.PREF_SUBSOURCE_ENABLED) ?: true
        if (!enabled) return@withContext emptyList()

        val cleanTitle = query.trim().takeIf { it.isNotBlank() } ?: return@withContext emptyList()
        val apiKey = apiKeyOverride?.trim()?.takeIf { it.isNotBlank() }
            ?: com.lagradost.cloudstream3.syncproviders.AccountManager.cachedAccounts["subsource"]?.firstOrNull()?.token?.accessToken?.trim()?.takeIf { it.isNotBlank() }
            ?: DesktopDataStore.getKey<String>(PlayerConfig.PREF_SUBSOURCE_API_KEY)?.trim()?.takeIf { it.isNotBlank() }

        if (apiKey == null) {
            AppLogger.i(TAG, "SubSource API key not configured. Obtain a free key from subsource.net/my-profile to enable SubSource subtitles in player search.")
            return@withContext emptyList()
        }

        val results = mutableListOf<Map<String, Any?>>()
        try {
            val headers = mapOf(
                "X-API-Key" to apiKey,
                "User-Agent" to "CloudStream Desktop",
            )

            val cleanImdb = imdbId?.takeIf { it.startsWith("tt", ignoreCase = true) }
            val searchUrl = if (cleanImdb != null) {
                "$OFFICIAL_API_URL/movies/search?searchType=imdb&imdb=$cleanImdb"
            } else {
                val sanitized = sanitizeTitle(cleanTitle)
                val encoded = URLEncoder.encode(sanitized.lowercase(), "UTF-8")
                "$OFFICIAL_API_URL/movies/search?searchType=text&q=$encoded" + if (season != null) "&season=$season" else ""
            }

            val searchResp = runCatching { app.get(searchUrl, headers = headers, timeout = 12000L) }.getOrNull()
            if (searchResp == null || !searchResp.isSuccessful) {
                AppLogger.w(TAG, "SubSource movie search returned ${searchResp?.code ?: "error"}")
                return@withContext emptyList()
            }
            val searchBody = searchResp.text.trimStart()
            if (!searchBody.startsWith("{") && !searchBody.startsWith("[")) {
                AppLogger.w(TAG, "SubSource movie search returned non-JSON body")
                return@withContext emptyList()
            }

            val rootNode = mapper.readTree(searchBody)
            val dataArray = rootNode.get("data")?.takeIf { it.isArray } ?: return@withContext emptyList()

            var bestMovieId: Int? = null
            for (item in dataArray) {
                val id = item.get("movieId")?.asInt() ?: item.get("id")?.asInt() ?: continue
                bestMovieId = id
                break
            }
            val movieId = bestMovieId ?: return@withContext emptyList()

            var subUrl = "$OFFICIAL_API_URL/subtitles?movieId=$movieId"
            val subsourceLang = toSubsourceLanguage(lang)
            if (subsourceLang != null) {
                subUrl += "&language=${URLEncoder.encode(subsourceLang, "UTF-8")}"
            }

            val subResp = runCatching { app.get(subUrl, headers = headers, timeout = 12000L) }.getOrNull()
            if (subResp == null || !subResp.isSuccessful) {
                AppLogger.w(TAG, "SubSource subtitles query returned ${subResp?.code ?: "error"}")
                return@withContext emptyList()
            }
            val subBody = subResp.text.trimStart()
            if (!subBody.startsWith("{") && !subBody.startsWith("[")) {
                AppLogger.w(TAG, "SubSource subtitles query returned non-JSON body")
                return@withContext emptyList()
            }

            val subRootNode = mapper.readTree(subBody)
            val subsArray = subRootNode.get("data")?.takeIf { it.isArray } ?: return@withContext emptyList()

            for (item in subsArray) {
                val subId = item.get("subtitleId")?.asInt() ?: item.get("id")?.asInt() ?: continue
                val rawLang = item.get("language")?.asText() ?: item.get("lang")?.asText() ?: "English"
                val releaseName = extractReleaseName(item, cleanTitle)
                val norm = LanguageNormalizer.normalize(rawLang)

                if (season != null && episode != null) {
                    val epPadded = episode.toString().padStart(2, '0')
                    val epRegex = Regex("(?i)(?:s0*${season}e0*$episode|s0*${season}\\.e0*$episode|${season}x0*$episode|e0*$episode\\b)")
                    if (!releaseName.contains(epPadded, ignoreCase = true) && !epRegex.containsMatchIn(releaseName)) {
                        continue
                    }
                }

                results.add(
                    mapOf(
                        "idPrefix" to "subsource",
                        "name" to releaseName,
                        "lang" to rawLang,
                        "langName" to norm.displayName,
                        "langBadge" to norm.badge,
                        "data" to subId.toString(),
                        "source" to "SubSource",
                        "seasonNumber" to season,
                        "epNumber" to episode,
                    ),
                )
            }
        } catch (e: Exception) {
            AppLogger.w(TAG, "searchPipeline failed: ${e.message}")
        }

        return@withContext results
    }

    /**
     * Resolves and downloads subtitle data for playback.
     */
    suspend fun loadSubtitle(data: String): String? = withContext(Dispatchers.IO) {
        if (data.isBlank()) return@withContext null

        val localFile = File(data)
        if (localFile.exists() && localFile.isFile) {
            return@withContext localFile.absolutePath
        }

        val cacheDir = File(PlatformPaths.cacheDir, "subtitles").also { it.mkdirs() }
        val safeId = data.replace(Regex("[^a-zA-Z0-9_-]"), "_")
        val cachedFile = File(cacheDir, "subsource_$safeId.srt")
        if (cachedFile.exists() && cachedFile.length() > 0L) {
            return@withContext cachedFile.absolutePath
        }

        val apiKey = com.lagradost.cloudstream3.syncproviders.AccountManager.cachedAccounts["subsource"]?.firstOrNull()?.token?.accessToken?.trim()?.takeIf { it.isNotBlank() }
            ?: DesktopDataStore.getKey<String>(PlayerConfig.PREF_SUBSOURCE_API_KEY)?.trim()?.takeIf { it.isNotBlank() }
        val headers = mutableMapOf("User-Agent" to "CloudStream Desktop")
        if (apiKey != null) {
            headers["X-API-Key"] = apiKey
        }

        val downloadUrl = if (data.startsWith("http", ignoreCase = true)) {
            data
        } else {
            "$OFFICIAL_API_URL/subtitles/$data/download"
        }

        val rawBytes = runCatching {
            val resp = app.get(downloadUrl, headers = headers, timeout = 15000L).okhttpResponse
            if (!resp.isSuccessful) {
                AppLogger.w(TAG, "SubSource download HTTP failed with status ${resp.code}")
                return@runCatching null
            }
            resp.body.bytes()
        }.getOrNull() ?: return@withContext null

        if (rawBytes.isEmpty()) return@withContext null

        val extracted = saveExtractedSubtitle(safeId, rawBytes)
        return@withContext extracted?.absolutePath
    }

    private suspend fun searchOfficialApi(
        apiKey: String,
        title: String,
        imdbId: String?,
        season: Int?,
        episode: Int?,
        year: Int?,
        isSeries: Boolean,
        onSubtitle: (SubtitleFile) -> Unit,
    ): Int {
        val headers = mapOf(
            "X-API-Key" to apiKey,
            "User-Agent" to "CloudStream Desktop",
        )

        val cleanImdb = imdbId?.takeIf { it.startsWith("tt", ignoreCase = true) }
        val searchUrl = if (cleanImdb != null) {
            "$OFFICIAL_API_URL/movies/search?searchType=imdb&imdb=$cleanImdb"
        } else {
            val sanitized = sanitizeTitle(title)
            val encoded = URLEncoder.encode(sanitized.lowercase(), "UTF-8")
            "$OFFICIAL_API_URL/movies/search?searchType=text&q=$encoded" + if (season != null) "&season=$season" else ""
        }

        val resp = runCatching { app.get(searchUrl, headers = headers, timeout = 12000L) }.getOrNull()
        if (resp == null || !resp.isSuccessful) return 0
        val respText = resp.text.trimStart()
        if (!respText.startsWith("{") && !respText.startsWith("[")) return 0
        val rootNode = mapper.readTree(respText)
        val dataArray = rootNode.get("data")?.takeIf { it.isArray } ?: return 0

        var bestMovieId: Int? = null
        for (item in dataArray) {
            val id = item.get("movieId")?.asInt() ?: item.get("id")?.asInt() ?: continue
            val itemYear = item.get("releaseYear")?.asInt() ?: item.get("year")?.asInt()
            if (year != null && itemYear != null && Math.abs(year - itemYear) > 1) continue
            bestMovieId = id
            break
        }
        if (bestMovieId == null && dataArray.size() > 0) {
            bestMovieId = dataArray.get(0).get("movieId")?.asInt() ?: dataArray.get(0).get("id")?.asInt()
        }
        val movieId = bestMovieId ?: return 0

        var subUrl = "$OFFICIAL_API_URL/subtitles?movieId=$movieId"
        val subResp = runCatching { app.get(subUrl, headers = headers, timeout = 12000L) }.getOrNull()
        if (subResp == null || !subResp.isSuccessful) return 0
        val subRespText = subResp.text.trimStart()
        if (!subRespText.startsWith("{") && !subRespText.startsWith("[")) return 0
        val subRootNode = mapper.readTree(subRespText)
        val subsArray = subRootNode.get("data")?.takeIf { it.isArray } ?: return 0

        val preferredLangs = LanguagePriorityHelper.getOrderedSubtitleLanguages()
        var emitted = 0

        for (item in subsArray) {
            if (emitted >= 4) break
            val subId = item.get("subtitleId")?.asInt() ?: item.get("id")?.asInt() ?: continue
            val rawLang = item.get("language")?.asText() ?: item.get("lang")?.asText() ?: "English"
            val releaseName = extractReleaseName(item, title)

            if (isSeries && episode != null) {
                val epPadded = episode.toString().padStart(2, '0')
                val epRegex = Regex("(?i)(?:s0*${season ?: 1}e0*$episode|s0*${season ?: 1}\\.e0*$episode|${season ?: 1}x0*$episode|e0*$episode\\b)")
                if (!releaseName.contains(epPadded, ignoreCase = true) && !epRegex.containsMatchIn(releaseName)) {
                    continue
                }
            }

            if (preferredLangs.isNotEmpty() && !matchesAnyPreferred(rawLang, preferredLangs)) {
                continue
            }

            val downloadUrl = "$OFFICIAL_API_URL/subtitles/$subId/download"
            val dlBytes = runCatching {
                val dlResp = app.get(downloadUrl, headers = headers, timeout = 15000L).okhttpResponse
                if (!dlResp.isSuccessful) null else dlResp.body.bytes()
            }.getOrNull() ?: continue

            val cachedFile = saveExtractedSubtitle(subId.toString(), dlBytes) ?: continue
            val norm = LanguageNormalizer.normalize(rawLang)
            val subFile = newSubtitleFile(
                lang = "${norm.displayName} - $releaseName (SubSource)",
                url = cachedFile.absolutePath,
            )
            onSubtitle(subFile)
            emitted++
        }

        return emitted
    }

    private suspend fun searchBasicApi(
        title: String,
        season: Int?,
        episode: Int?,
        year: Int?,
        isSeries: Boolean,
        onSubtitle: (SubtitleFile) -> Unit,
    ): Int {
        val searchPayload = mapper.writeValueAsString(mapOf("query" to title))
        val searchResp = runCatching {
            app.post(
                url = "$BASIC_API_URL/searchMovie",
                headers = BASIC_HEADERS,
                requestBody = searchPayload.toRequestBody(jsonMediaType),
                timeout = 12000L,
            )
        }.getOrNull()
        if (searchResp == null || !searchResp.isSuccessful) return 0
        val searchRespText = searchResp.text.trimStart()
        if (!searchRespText.startsWith("{") && !searchRespText.startsWith("[")) return 0

        val rootNode = mapper.readTree(searchRespText)
        if (rootNode.get("success")?.asBoolean() != true) return 0
        val foundArray = rootNode.get("found")?.takeIf { it.isArray } ?: return 0

        var bestLinkName: String? = null
        for (item in foundArray) {
            val linkName = item.get("linkName")?.asText()?.takeIf { it.isNotBlank() } ?: continue
            val itemYear = item.get("releaseYear")?.asInt()
            val itemType = item.get("type")?.asText() ?: ""
            if (isSeries && itemType.equals("Movie", ignoreCase = true)) continue
            if (!isSeries && itemType.contains("Series", ignoreCase = true)) continue
            if (year != null && itemYear != null && Math.abs(year - itemYear) > 1) continue
            bestLinkName = linkName
            break
        }
        if (bestLinkName == null && foundArray.size() > 0) {
            bestLinkName = foundArray.get(0).get("linkName")?.asText()
        }
        val linkName = bestLinkName ?: return 0

        val movieData = mutableMapOf<String, Any>("movieName" to linkName)
        if (isSeries && season != null) {
            movieData["season"] = "season-$season"
        }
        val moviePayload = mapper.writeValueAsString(movieData)

        val movieResp = runCatching {
            app.post(
                url = "$BASIC_API_URL/getMovie",
                headers = BASIC_HEADERS,
                requestBody = moviePayload.toRequestBody(jsonMediaType),
                timeout = 12000L,
            )
        }.getOrNull()
        if (movieResp == null || !movieResp.isSuccessful) return 0
        val movieRespText = movieResp.text.trimStart()
        if (!movieRespText.startsWith("{") && !movieRespText.startsWith("[")) return 0

        val movieNode = mapper.readTree(movieRespText)
        if (movieNode.get("success")?.asBoolean() != true) return 0
        val subsArray = movieNode.get("subs")?.takeIf { it.isArray } ?: return 0

        val preferredLangs = LanguagePriorityHelper.getOrderedSubtitleLanguages()
        var emitted = 0

        for (sub in subsArray) {
            if (emitted >= 4) break
            val link = sub.get("link")?.asText()?.takeIf { it.isNotBlank() } ?: continue
            val rawLang = sub.get("lang")?.asText() ?: "English"
            val releaseName = sub.get("releaseName")?.asText() ?: title

            if (isSeries && episode != null) {
                val epPadded = episode.toString().padStart(2, '0')
                val epRegex = Regex("(?i)(?:s0*${season ?: 1}e0*$episode|s0*${season ?: 1}\\.e0*$episode|${season ?: 1}x0*$episode|e0*$episode\\b)")
                if (!releaseName.contains(epPadded, ignoreCase = true) && !epRegex.containsMatchIn(releaseName)) {
                    continue
                }
            }

            if (preferredLangs.isNotEmpty() && !matchesAnyPreferred(rawLang, preferredLangs)) {
                continue
            }

            val dlPayload = mapper.writeValueAsString(mapOf("id" to link))
            val dlBytes = runCatching {
                val dlResp = app.post(
                    url = "$BASIC_API_URL/downloadSub",
                    headers = BASIC_HEADERS,
                    requestBody = dlPayload.toRequestBody(jsonMediaType),
                    timeout = 15000L,
                ).okhttpResponse
                if (!dlResp.isSuccessful) null else dlResp.body.bytes()
            }.getOrNull() ?: continue

            val cachedFile = saveExtractedSubtitle(link, dlBytes) ?: continue
            val norm = LanguageNormalizer.normalize(rawLang)
            val subFile = newSubtitleFile(
                lang = "${norm.displayName} - $releaseName (SubSource)",
                url = cachedFile.absolutePath,
            )
            onSubtitle(subFile)
            emitted++
        }

        return emitted
    }

    internal fun matchesAnyPreferred(rawLang: String, preferredLangs: List<String>): Boolean {
        val norm = LanguageNormalizer.normalize(rawLang)
        return preferredLangs.any { pref ->
            LanguageNormalizer.isMatch(pref, norm.code3) ||
                LanguageNormalizer.isMatch(pref, norm.code2) ||
                LanguageNormalizer.isMatch(pref, norm.displayName)
        }
    }

    private fun saveExtractedSubtitle(id: String, rawBytes: ByteArray): File? {
        val cleanBytes = extractSubtitleBytes(rawBytes) ?: return null
        val cacheDir = File(PlatformPaths.cacheDir, "subtitles").also { it.mkdirs() }
        val safeId = id.replace(Regex("[^a-zA-Z0-9_-]"), "_")
        val file = File(cacheDir, "subsource_$safeId.srt")
        if (!file.exists() || file.length() == 0L) {
            runCatching { file.writeBytes(cleanBytes) }.onFailure {
                AppLogger.w(TAG, "Failed to write subtitle file: ${it.message}")
                return null
            }
        }
        return file
    }

    internal fun extractSubtitleBytes(rawBytes: ByteArray): ByteArray? {
        if (rawBytes.size >= 4 && rawBytes[0] == 0x50.toByte() && rawBytes[1] == 0x4B.toByte()) {
            try {
                ZipInputStream(ByteArrayInputStream(rawBytes)).use { zis ->
                    var entry = zis.nextEntry
                    while (entry != null) {
                        val name = entry.name.lowercase()
                        if (!entry.isDirectory && (name.endsWith(".srt") || name.endsWith(".vtt") || name.endsWith(".sub") || name.endsWith(".ass"))) {
                            return zis.readBytes()
                        }
                        entry = zis.nextEntry
                    }
                }
            } catch (e: Exception) {
                AppLogger.w(TAG, "Failed to unzip archive: ${e.message}")
            }
        }
        if (rawBytes.isNotEmpty()) {
            return rawBytes
        }
        return null
    }

    /**
     * Verifies whether the provided API key is valid and responsive against the SubSource official API.
     */
    suspend fun testApiKey(apiKey: String): Pair<Boolean, String> = withContext(Dispatchers.IO) {
        val clean = apiKey.trim()
        if (clean.isBlank()) return@withContext false to "API key cannot be empty"
        try {
            val headers = mapOf(
                "X-API-Key" to clean,
                "User-Agent" to "CloudStream Desktop",
            )
            val resp = app.get("$OFFICIAL_API_URL/movies/search?searchType=text&q=matrix", headers = headers, timeout = 8000L).okhttpResponse
            when (resp.code) {
                200 -> true to "Valid API Key (Connected successfully)"
                401 -> false to "Invalid API Key (Unauthorized - HTTP 401)"
                403 -> false to "API Key forbidden or rate-limited (HTTP 403)"
                else -> false to "HTTP ${resp.code}: Unexpected response"
            }
        } catch (e: Exception) {
            false to "Connection error: ${e.message ?: "Failed to reach server"}"
        }
    }
}
