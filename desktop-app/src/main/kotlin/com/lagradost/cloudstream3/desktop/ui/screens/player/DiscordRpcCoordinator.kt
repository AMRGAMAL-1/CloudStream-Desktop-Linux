package com.lagradost.cloudstream3.desktop.ui.screens.player

import com.lagradost.cloudstream3.desktop.discord.DiscordRpcManager
import com.lagradost.cloudstream3.desktop.ui.VideoLaunchData
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

internal object DiscordRpcCoordinator {

    fun updatePlaying(
        launchData: VideoLaunchData?,
        positionSeconds: Long,
        durationSeconds: Long,
        isPaused: Boolean,
    ) {
        if (launchData == null) return
        val showName = launchData.history.showName.takeIf { it.isNotBlank() }
        val title = showName ?: launchData.title ?: "Playing Media"
        val season = launchData.history.season
        val episode = launchData.history.episode
        val episodeInfo = when {
            season != null && episode != null -> "S$season • E$episode"
            episode != null -> "Episode $episode"
            showName != null && launchData.title != null && launchData.title != showName -> launchData.title
            else -> null
        }
        DiscordRpcManager.updatePlaying(
            title = title,
            episodeInfo = episodeInfo,
            positionSeconds = positionSeconds,
            durationSeconds = durationSeconds,
            isPaused = isPaused,
            posterUrl = launchData.history.posterUrl,
        )
    }

    fun attachPauseObserver(
        scope: CoroutineScope,
        playerState: PlayerState,
        getLaunchData: () -> VideoLaunchData?,
    ) {
        // 1. Immediate Pause/Resume reactivity
        scope.launch(Dispatchers.IO) {
            playerState.isPaused.collect { isPaused ->
                val currentData = getLaunchData() ?: return@collect
                val currentPosSec = playerState.positionMs.value / 1000L
                val durSec = playerState.durationMs.value / 1000L
                updatePlaying(
                    launchData = currentData,
                    positionSeconds = currentPosSec,
                    durationSeconds = durSec,
                    isPaused = isPaused,
                )
            }
        }

        // 2. Instant Duration Discovery (fires the moment MPV demuxes stream headers)
        scope.launch(Dispatchers.IO) {
            playerState.durationMs.collect { durMs ->
                if (durMs <= 0L) return@collect
                val currentData = getLaunchData() ?: return@collect
                val currentPosSec = playerState.positionMs.value / 1000L
                val durSec = durMs / 1000L
                val isPaused = playerState.isPaused.value
                updatePlaying(
                    launchData = currentData,
                    positionSeconds = currentPosSec,
                    durationSeconds = durSec,
                    isPaused = isPaused,
                )
            }
        }

        // 3. Instant Seek Synchronization (sampled once per second)
        scope.launch(Dispatchers.IO) {
            var lastEmittedSec = -1L
            playerState.positionMs.collect { posMs ->
                val currentPosSec = posMs / 1000L
                if (currentPosSec == lastEmittedSec) return@collect
                lastEmittedSec = currentPosSec
                val currentData = getLaunchData() ?: return@collect
                val durSec = playerState.durationMs.value / 1000L
                val isPaused = playerState.isPaused.value
                updatePlaying(
                    launchData = currentData,
                    positionSeconds = currentPosSec,
                    durationSeconds = durSec,
                    isPaused = isPaused,
                )
            }
        }
    }
}
