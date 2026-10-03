package com.lagradost.cloudstream3.syncproviders.providers

import com.lagradost.cloudstream3.desktop.player.PlayerConfig
import com.lagradost.cloudstream3.desktop.subtitles.SubsourceSubtitleProvider
import com.lagradost.cloudstream3.subtitles.AbstractSubtitleEntities.SubtitleEntity
import com.lagradost.cloudstream3.subtitles.AbstractSubtitleEntities.SubtitleSearch
import com.lagradost.cloudstream3.syncproviders.AccountManager
import com.lagradost.cloudstream3.syncproviders.AuthData
import com.lagradost.cloudstream3.syncproviders.AuthLoginRequirement
import com.lagradost.cloudstream3.syncproviders.AuthLoginResponse
import com.lagradost.cloudstream3.syncproviders.AuthToken
import com.lagradost.cloudstream3.syncproviders.AuthUser
import com.lagradost.cloudstream3.syncproviders.SubtitleAPI
import com.lagradost.common.storage.DesktopDataStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

class SubsourceApi : SubtitleAPI() {
    override val name = "SubSource"
    override val idPrefix = "subsource"

    override val icon = null
    override val hasInApp = true
    override val inAppLoginRequirement = AuthLoginRequirement(apiKey = true)
    override val requiresLogin = false
    override val createAccountUrl = "https://subsource.net/my-profile"

    override suspend fun login(form: AuthLoginResponse): AuthToken? {
        val apiKey = form.apiKey?.trim()?.takeIf { it.isNotBlank() } ?: return null
        val (isValid, msg) = SubsourceSubtitleProvider.testApiKey(apiKey)
        if (!isValid) {
            throw Exception(msg)
        }
        DesktopDataStore.setKey(PlayerConfig.PREF_SUBSOURCE_API_KEY, apiKey)
        return AuthToken(accessToken = apiKey, payload = "SubSource User")
    }

    override suspend fun user(token: AuthToken?): AuthUser? {
        val name = token?.payload ?: return null
        return AuthUser(id = name.hashCode(), name = name)
    }

    override suspend fun search(
        auth: AuthData?,
        query: SubtitleSearch,
    ): List<SubtitleEntity>? = withContext(Dispatchers.IO) {
        val resolvedKey = auth?.token?.accessToken
            ?: AccountManager.cachedAccounts[idPrefix]?.firstOrNull()?.token?.accessToken
            ?: DesktopDataStore.getKey<String>(PlayerConfig.PREF_SUBSOURCE_API_KEY)?.trim()?.takeIf { it.isNotBlank() }

        if (resolvedKey.isNullOrBlank()) return@withContext emptyList()

        val rawResults = SubsourceSubtitleProvider.searchPipeline(
            query = query.query,
            imdbId = query.imdbId,
            season = query.seasonNumber,
            episode = query.epNumber,
            lang = query.lang,
            apiKeyOverride = resolvedKey,
        )

        rawResults.map { map ->
            SubtitleEntity(
                idPrefix = idPrefix,
                name = (map["name"] as? String) ?: "SubSource Subtitle",
                lang = (map["lang"] as? String) ?: "en",
                data = (map["data"] as? String) ?: "",
                source = name,
                epNumber = map["epNumber"] as? Int ?: query.epNumber,
                seasonNumber = map["seasonNumber"] as? Int ?: query.seasonNumber,
                year = query.year,
            )
        }
    }

    override suspend fun load(auth: AuthData?, subtitle: SubtitleEntity): String? {
        if (subtitle.data.isBlank()) return null
        return SubsourceSubtitleProvider.loadSubtitle(subtitle.data)
    }
}
