package com.lagradost.cloudstream3.desktop.ui.screens.player

import com.lagradost.cloudstream3.SubtitleFile
import com.lagradost.cloudstream3.utils.ExtractorLink
import java.util.concurrent.ConcurrentHashMap

object LinkCache {
    data class CachedLinks(
        val links: List<ExtractorLink>,
        val subtitles: List<SubtitleFile>,
        val timestamp: Long,
    )

    private val cache = ConcurrentHashMap<String, CachedLinks>()
    private const val CACHE_DURATION_MS = 20 * 60 * 1000L // 20 minutes
    private const val MAX_ENTRIES = 50

    fun get(episodeId: String): CachedLinks? {
        val entry = cache[episodeId] ?: return null
        if (System.currentTimeMillis() - entry.timestamp > CACHE_DURATION_MS) {
            cache.remove(episodeId)
            return null
        }
        return entry
    }

    fun remove(episodeId: String) {
        cache.remove(episodeId)
    }

    fun clearAll() {
        cache.clear()
    }

    private fun prune(now: Long) {
        // 1. Prune all expired entries
        cache.entries.removeIf { now - it.value.timestamp > CACHE_DURATION_MS }

        // 2. Enforce maximum capacity bound (evict oldest)
        if (cache.size >= MAX_ENTRIES) {
            val oldestKey = cache.minByOrNull { it.value.timestamp }?.key
            if (oldestKey != null) {
                cache.remove(oldestKey)
            }
        }
    }

    fun set(episodeId: String, links: List<ExtractorLink>, subtitles: List<SubtitleFile>) {
        set(listOf(episodeId), links, subtitles)
    }

    fun set(keys: Collection<String>, links: List<ExtractorLink>, subtitles: List<SubtitleFile>) {
        val validKeys = keys.filter { it.isNotBlank() }
        if (validKeys.isEmpty()) return
        val now = System.currentTimeMillis()
        prune(now)
        val entry = CachedLinks(
            links = links,
            subtitles = subtitles,
            timestamp = now,
        )
        validKeys.forEach { cache[it] = entry }
    }

    fun addLinks(keys: Collection<String>, newLinks: List<ExtractorLink>, newSubtitles: List<SubtitleFile> = emptyList()) {
        val validKeys = keys.filter { it.isNotBlank() }
        if (validKeys.isEmpty() || (newLinks.isEmpty() && newSubtitles.isEmpty())) return
        val now = System.currentTimeMillis()
        val existing = validKeys.firstNotNullOfOrNull { cache[it] }
        val mergedLinks = if (existing != null) {
            (existing.links + newLinks).distinctBy { it.url }
        } else {
            newLinks.distinctBy { it.url }
        }
        val mergedSubs = if (existing != null) {
            (existing.subtitles + newSubtitles).distinctBy { it.url }
        } else {
            newSubtitles.distinctBy { it.url }
        }
        prune(now)
        val entry = CachedLinks(
            links = mergedLinks,
            subtitles = mergedSubs,
            timestamp = now,
        )
        validKeys.forEach { cache[it] = entry }
    }
}
