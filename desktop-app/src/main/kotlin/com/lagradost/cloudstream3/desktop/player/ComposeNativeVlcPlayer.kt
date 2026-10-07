package com.lagradost.cloudstream3.desktop.player

import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.awt.SwingPanel
import androidx.compose.ui.graphics.Color
import com.fasterxml.jackson.module.kotlin.jacksonObjectMapper
import com.lagradost.cloudstream3.desktop.player.ipc.PlayerInboundEvent
import com.lagradost.cloudstream3.desktop.player.webview.NativePlayerBridge
import com.lagradost.cloudstream3.desktop.ui.screens.player.PlayerState
import com.lagradost.cloudstream3.utils.ExtractorLink
import com.lagradost.common.logging.AppLogger
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.awt.Canvas
import java.awt.event.ComponentAdapter
import java.awt.event.ComponentEvent
import java.io.File
import java.util.concurrent.atomic.AtomicBoolean

private val vlcObjectMapper = jacksonObjectMapper()

/**
 * VLC counterpart of ComposeNativeWebPlayer.
 *
 * It deliberately reuses the exact same player.html/player.css/player.js
 * surface. The only backend-specific part is the native video engine exposed
 * by NativePlayerBridge; this keeps controls and lifecycle identical to MPV.
 */
@Composable
internal fun ComposeNativeVlcPlayer(
    modifier: Modifier,
    link: ExtractorLink?,
    title: String?,
    backdropUrl: String? = null,
    logoUrl: String? = null,
    seriesPosterUrl: String? = null,
    links: List<ExtractorLink> = emptyList(),
    currentLinkIndex: Int = 0,
    episodes: List<com.lagradost.cloudstream3.Episode> = emptyList(),
    currentEpisodeId: String? = null,
    currentEpisodeNumber: Int? = null,
    currentSeasonNumber: Int? = null,
    subtitles: List<com.lagradost.cloudstream3.SubtitleFile>,
    startPositionMs: Long,
    onPlaybackReady: () -> Unit,
    onPlaybackError: (String) -> Unit,
    onFinished: () -> Unit,
    onPositionChange: (Long, Long) -> Unit,
    onCloseRequest: () -> Unit,
    isExiting: Boolean,
    onSkipScraping: (() -> Unit)?,
    onFullscreenToggle: (() -> Unit)?,
    playerState: PlayerState?,
    onLinkChange: ((String) -> Unit)?,
    onEpisodeChange: ((String) -> Unit)?,
    onNextEpisode: (() -> Unit)?,
    onReplayEpisode: (() -> Unit)?,
    reloadKey: Int,
) {
    val scope = rememberCoroutineScope()
    var surfaceReady by remember { mutableStateOf(false) }
    var uiReady by remember { mutableStateOf(false) }
    val playbackReported = remember { AtomicBoolean(false) }
    val finishedReported = remember { AtomicBoolean(false) }
    val primaryColor = androidx.compose.material3.MaterialTheme.colorScheme.primary
    val accentColorHex = remember(primaryColor) {
        String.format(
            "#%02X%02X%02X",
            (primaryColor.red * 255).toInt(),
            (primaryColor.green * 255).toInt(),
            (primaryColor.blue * 255).toInt(),
        )
    }
    val accentColorRgb = remember(primaryColor) {
        "${(primaryColor.red * 255).toInt()}, ${(primaryColor.green * 255).toInt()}, ${(primaryColor.blue * 255).toInt()}"
    }

    val controller = remember {
        object : PlayerState.VlcController {
            override fun togglePause() {
                if (playerState?.isPaused?.value == true) NativePlayerBridge.playVlc() else NativePlayerBridge.pauseVlc()
            }

            override fun pause() = NativePlayerBridge.pauseVlc()
            override fun play() = NativePlayerBridge.playVlc()
            override fun seekTo(positionMs: Long) = NativePlayerBridge.seekVlc(positionMs)
            override fun seekBy(offsetMs: Long) {
                NativePlayerBridge.seekVlc((playerState?.positionMs?.value ?: 0L) + offsetMs)
            }
            override fun setVolume(volume: Float) = NativePlayerBridge.setVlcVolume(volume.toInt())
            override fun setSpeed(speed: Float) = NativePlayerBridge.setVlcRate(speed)
            override fun setMute(muted: Boolean) = NativePlayerBridge.setVlcMute(muted)
            override fun executeCommand(command: String) {
                when {
                    command == "pause" -> NativePlayerBridge.pauseVlc()
                    command == "play" -> NativePlayerBridge.playVlc()
                    command.startsWith("seek ") && command.contains("absolute-percent") -> {
                        playerState?.durationMs?.value?.let { NativePlayerBridge.seekVlc(it) }
                    }
                    command.startsWith("seek ") -> {
                        val delta = command.removePrefix("seek ").trim().toDoubleOrNull()
                        if (delta != null) NativePlayerBridge.seekVlc((playerState?.positionMs?.value ?: 0L) + (delta * 1000L).toLong())
                    }
                }
            }
            override fun setProperty(property: String, value: String) {
                when (property) {
                    "volume" -> NativePlayerBridge.setVlcVolume(value.toFloatOrNull()?.toInt() ?: 100)
                    "speed" -> NativePlayerBridge.setVlcRate(value.toFloatOrNull() ?: 1f)
                }
            }
        }
    }

    DisposableEffect(controller) {
        playerState?.attachVlcController(controller)
        onDispose { playerState?.detachVlcController(controller) }
    }

    fun loadControlsHtml() {
        val directory = File(System.getProperty("java.io.tmpdir"), "CloudStreamWebView2")
        directory.mkdirs()
        val target = File(directory, "cloudstream_vlc_controls.html")
        val htmlTemplate = NativePlayerBridge.loadPlayerUiResource("/player-ui/player.html")
        val css = NativePlayerBridge.loadPlayerUiResource("/player-ui/player.css")
        val js = NativePlayerBridge.loadPlayerUiResource("/player-ui/player.js")
        val initialTitle = (title ?: "CloudStream").replace("\"", "&quot;").replace("<", "&lt;").replace(">", "&gt;")
        val rawBackdrop = backdropUrl?.let { raw -> if (raw.startsWith("//")) "https:$raw" else raw }
        val initialBackdropUrl = com.lagradost.cloudstream3.desktop.utils.ImageUtils.getCachedDiskFileUri(rawBackdrop) ?: rawBackdrop.orEmpty()
        val initialBackdropClass = if (initialBackdropUrl.isNotEmpty()) "loaded" else ""
        val rawLogo = logoUrl?.let { raw -> if (raw.startsWith("//")) "https:$raw" else raw }
        val initialLogoUrl = com.lagradost.cloudstream3.desktop.utils.ImageUtils.getCachedDiskFileUri(rawLogo) ?: rawLogo.orEmpty()
        val hasLogo = initialLogoUrl.isNotEmpty()
        val initialLogoStyle = if (hasLogo) "display: block;" else "display: none;"
        val initialTitleStyle = if (hasLogo) "display: none;" else "display: block;"
        val dynamicFonts = com.lagradost.cloudstream3.desktop.ui.theme.CustomFontManager.getDynamicFontFaceCss()
        val finalCss = listOf(
            com.lagradost.cloudstream3.desktop.ui.PipState.unsupportedControlsCss,
            dynamicFonts,
            css,
        ).filter { it.isNotBlank() }.joinToString("\n")
        val html = htmlTemplate
            .replace("/* CSS_INJECT */", finalCss)
            .replace("/* JS_INJECT */", js)
            .replace("{{ACCENT_COLOR}}", accentColorHex)
            .replace("{{ACCENT_COLOR_RGB}}", accentColorRgb)
            .replace("{{INITIAL_BACKDROP_URL}}", initialBackdropUrl)
            .replace("{{INITIAL_BACKDROP_CLASS}}", initialBackdropClass)
            .replace("{{INITIAL_LOGO_URL}}", initialLogoUrl)
            .replace("{{INITIAL_LOGO_STYLE}}", initialLogoStyle)
            .replace("{{INITIAL_TITLE}}", initialTitle)
            .replace("{{INITIAL_TITLE_STYLE}}", initialTitleStyle)
            .replace("{{INITIAL_SUBTITLE}}", "")
            .replace("{{INITIAL_SUBTITLE_STYLE}}", "display:none;")
        if (html.isBlank()) throw IllegalStateException("VLC player UI resources are missing")
        target.writeText(html, Charsets.UTF_8)
        NativePlayerBridge.loadUrl(target.toURI().toString())
    }

    fun pushVlcAppState(isLoading: Boolean) {
        val volume = playerState?.volume?.value ?: 100f
        val muted = playerState?.isMuted?.value ?: false
        val payload = AppStateUpdatePayload(
            volume = volume,
            isMuted = muted,
            isAppLoading = isLoading,
            loadingStatusText = if (isLoading) "Connecting to selected source..." else null,
            debugWait = false,
            debugHasEver = true,
            debugPos = 0.0,
            interpolationEnabled = false,
            autoPlayEnabled = true,
            showEndTime = false,
            showClock = false,
            showServerQuality = false,
            pauseInfoMode = "delay_5s",
            showPauseCast = false,
            seekDurationMs = (PlayerConfig.getSeekDurationSeconds() * 1000).toLong(),
        )
        NativePlayerBridge.postMessage(vlcObjectMapper.writeValueAsString(payload))
    }

    // VLC uses the same HTML UI as MPV. Publish the session metadata before
    // the first decoded frame so probing has the correct title and the normal
    // controls do not fall back to "CloudStream Player".
    fun pushVlcMetadata(isProbing: Boolean) {
        val metadataTitle = title?.takeIf { it.isNotBlank() }
            ?: link?.name?.takeIf { it.isNotBlank() }
            ?: "CloudStream"
        val metadataLinks = links.mapIndexed { index, item ->
            LinkPayload(
                index = index,
                name = item.name,
                quality = item.quality,
                isActive = index == currentLinkIndex,
                isM3u8 = item.isM3u8,
                isDash = item.isDash,
                url = item.url,
            )
        }
        val metadataEpisodes = episodes.map { episode ->
            val isCurrent = (currentEpisodeId != null && episode.data == currentEpisodeId) ||
                (currentEpisodeNumber != null && episode.episode == currentEpisodeNumber &&
                    (currentSeasonNumber == null || episode.season == null || episode.season == currentSeasonNumber))
            EpisodePayload(
                id = episode.data,
                title = episode.name ?: ("Episode " + episode.episode),
                season = episode.season,
                episode = episode.episode,
                isActive = isCurrent,
                posterUrl = episode.posterUrl ?: seriesPosterUrl,
                description = episode.description,
                runTime = episode.runTime,
            )
        }
        val metadata = PlayerUiSyncState(
            plot = null,
            year = null,
            tags = null,
            isProbing = isProbing,
            isScraping = isProbing,
            backdropUrl = backdropUrl,
            logoUrl = logoUrl,
            currentLinkIndex = currentLinkIndex,
            failedLinks = emptyList(),
            links = metadataLinks,
            episodes = metadataEpisodes,
            audioTracks = emptyList(),
            subTracks = emptyList(),
            videoTracks = emptyList(),
            lazyAudioTracks = emptyList(),
            lazySubTracks = emptyList(),
            lazyVideoTracks = emptyList(),
            startPositionMs = startPositionMs,
            title = metadataTitle,
            shaders = emptyList(),
            activeShader = null,
            activeSubtitleFont = null,
            availableSubtitleFonts = emptyList(),
            activeSubtitleBackground = null,
            activeSubtitleBorderColor = null,
            activeSubtitleBorderSize = null,
            activeSubtitleShadowColor = null,
            activeSubtitleShadowOffset = null,
            activeSubtitleBlur = null,
            activeSubtitleBold = null,
            activeSubtitleItalic = null,
            activeLazyVideoTrackUrl = null,
            resolution = null,
            activeSubtitleOverrideEnabled = false,
        )
        NativePlayerBridge.postMessage(
            vlcObjectMapper.writeValueAsString(MetadataUpdatePayloadWrapper(value = metadata)),
        )
        pushVlcAppState(isProbing)
    }

    DisposableEffect(Unit) {
        NativePlayerBridge.setEventListener(object : NativePlayerBridge.NativePlayerEventListener {
            override fun onPlayerEvent(type: String, value: String) {
                if (type != "message") return
                val root = runCatching { vlcObjectMapper.readTree(value) }.getOrNull() ?: return
                if (root.get("type")?.asText() == "state_update") {
                    val position = root.get("positionMs")?.asLong() ?: 0L
                    val duration = root.get("durationMs")?.asLong() ?: 0L
                    val buffering = root.get("isLoading")?.asBoolean() ?: false
                    val playing = root.get("isPlaying")?.asBoolean() ?: false
                    val vlcState = root.get("state")?.asInt() ?: 0
                    playerState?._positionMs?.value = position
                    playerState?._durationMs?.value = duration
                    playerState?._bufferMs?.value = root.get("bufferMs")?.asLong() ?: position
                    playerState?._isBuffering?.value = buffering
                    playerState?._isPaused?.value = !playing
                    scope.launch(Dispatchers.Main) {
                        onPositionChange(position, duration)
                        if (playing && duration > 0L && playbackReported.compareAndSet(false, true)) {
                            pushVlcMetadata(isProbing = false)
                            NativePlayerBridge.executeScript("window.__dismissProbingOverlay && window.__dismissProbingOverlay(); window.showControls && window.showControls(null);")
                            onPlaybackReady()
                        }
                        if (vlcState == 6 && finishedReported.compareAndSet(false, true)) {
                            onFinished()
                        } else if (vlcState == 7 && !finishedReported.get()) {
                            onPlaybackError("VLC reported a playback error")
                        }
                    }
                    return
                }

                val event = PlayerInboundEvent.fromJson(root, value)
                scope.launch(Dispatchers.Main) {
                    when (event) {
                        is PlayerInboundEvent.UiReady -> {
                            uiReady = true
                            pushVlcMetadata(isProbing = true)
                        }
                        is PlayerInboundEvent.ExitPlayer -> onCloseRequest()
                        is PlayerInboundEvent.ToggleFullscreen -> onFullscreenToggle?.invoke()
                        is PlayerInboundEvent.TogglePlay -> playerState?.togglePlayPause()
                        is PlayerInboundEvent.Play -> playerState?.play()
                        is PlayerInboundEvent.Pause -> playerState?.pause()
                        is PlayerInboundEvent.ToggleMute -> playerState?.setMute(event.forcedState ?: !(playerState?.isMuted?.value ?: false))
                        is PlayerInboundEvent.SetVolume -> playerState?.setVolume(event.volume.toFloat())
                        is PlayerInboundEvent.SetSpeed -> playerState?.setSpeed(event.speed.toFloat())
                        is PlayerInboundEvent.SeekTo -> playerState?.seekTo(event.positionMs.toLong())
                        is PlayerInboundEvent.SeekBy -> playerState?.seekBy(event.deltaMs.toLong())
                        is PlayerInboundEvent.SeekLive -> playerState?.seekLive()
                        is PlayerInboundEvent.SkipInterval -> playerState?.skipCurrentInterval()
                        is PlayerInboundEvent.ChangeLink -> onLinkChange?.invoke(event.linkUrl)
                        is PlayerInboundEvent.LoadEpisode -> onEpisodeChange?.invoke(event.episodeId)
                        is PlayerInboundEvent.LoadNextEpisode -> onNextEpisode?.invoke()
                        is PlayerInboundEvent.ReplayEpisode -> onReplayEpisode?.invoke()
                        is PlayerInboundEvent.SkipScraping -> onSkipScraping?.invoke()
                        is PlayerInboundEvent.ClientError -> onPlaybackError(event.message)
                        is PlayerInboundEvent.Unknown -> Unit
                        else -> Unit
                    }
                }
            }
        })
        onDispose {
            NativePlayerBridge.stopVlcSync()
            NativePlayerBridge.stopVlc()
            NativePlayerBridge.resizeWebView(0, 0)
            NativePlayerBridge.setEventListener(null)
            NativePlayerBridge.destroyWebView()
            playerState?.detachVlcController(controller)
        }
    }

    val surfaceAttempted = remember { AtomicBoolean(false) }
    val videoCanvas = remember {
        object : Canvas() {
            init { background = java.awt.Color.BLACK; isFocusable = true }
            override fun paint(g: java.awt.Graphics?) {
                g?.color = java.awt.Color.BLACK
                g?.fillRect(0, 0, width.coerceAtLeast(1), height.coerceAtLeast(1))
            }
            override fun update(g: java.awt.Graphics?) = paint(g)

            override fun addNotify() {
                super.addNotify()
                if (!surfaceAttempted.compareAndSet(false, true)) return
                val canvas = this
                Thread {
                    val handle = runCatching {
                        NativePlayerBridge.initWebView(
                            com.sun.jna.Native.getComponentID(canvas),
                            canvas.width.coerceAtLeast(1),
                            canvas.height.coerceAtLeast(1),
                        )
                    }.getOrElse {
                        AppLogger.e("ComposeNativeVlcPlayer: native surface initialization failed", it)
                        0L
                    }
                    if (handle == 0L) {
                        scope.launch(Dispatchers.Main) {
                            onPlaybackError("Linux native player surface could not be created")
                        }
                        return@Thread
                    }
                    runCatching { loadControlsHtml() }.onFailure {
                        AppLogger.e("ComposeNativeVlcPlayer: failed to load player controls", it)
                        scope.launch(Dispatchers.Main) {
                            onPlaybackError("Player controls could not be loaded")
                        }
                        return@Thread
                    }
                    scope.launch(Dispatchers.Main) { surfaceReady = true }
                }.apply {
                    name = "cs3-vlc-native-surface"
                    isDaemon = true
                    start()
                }
            }
        }
    }

    DisposableEffect(Unit) {
        val listener = object : ComponentAdapter() {
            override fun componentResized(e: ComponentEvent) {
                NativePlayerBridge.resizeWebView(e.component.width, e.component.height)
            }
        }
        videoCanvas.addComponentListener(listener)
        onDispose { videoCanvas.removeComponentListener(listener) }
    }

    LaunchedEffect(Unit) {
        repeat(120) {
            if (videoCanvas.width > 1 && videoCanvas.height > 1) {
                NativePlayerBridge.resizeWebView(videoCanvas.width, videoCanvas.height)
                return@LaunchedEffect
            }
            kotlinx.coroutines.delay(16L)
        }
    }

    LaunchedEffect(link?.url, reloadKey) {
        playbackReported.set(false)
        finishedReported.set(false)
    }

    LaunchedEffect(surfaceReady, link?.url, reloadKey, isExiting) {
        if (!surfaceReady || isExiting || link == null) return@LaunchedEffect
        if (uiReady) pushVlcMetadata(isProbing = true)
        val validatedResult = withContext(Dispatchers.IO) {
            runCatching { com.lagradost.player.impl.PlayerLinkHandler.validate(link, title) }
                .getOrElse { Result.failure(it) }
        }
        val validated = validatedResult.getOrElse {
            onPlaybackError(it.message ?: "Invalid VLC stream")
            return@LaunchedEffect
        }
        val userAgent = validated.headers.entries.firstOrNull { it.key.equals("user-agent", true) }?.value
        val referer = validated.headers.entries.firstOrNull { it.key.equals("referer", true) || it.key.equals("referrer", true) }?.value
        val started = NativePlayerBridge.startVlc(
            validated.url,
            validated.displayTitle,
            userAgent,
            referer,
            startPositionMs,
        )
        if (!started) {
            onPlaybackError("Embedded libVLC could not start. Install VLC/libVLC and try again.")
            return@LaunchedEffect
        }
        NativePlayerBridge.startVlcSync()
    }

    LaunchedEffect(isExiting) {
        if (isExiting) {
            NativePlayerBridge.stopVlcSync()
            NativePlayerBridge.stopVlc()
        }
    }

    SwingPanel(
        background = Color.Black,
        factory = { videoCanvas },
        modifier = modifier,
    )
}
