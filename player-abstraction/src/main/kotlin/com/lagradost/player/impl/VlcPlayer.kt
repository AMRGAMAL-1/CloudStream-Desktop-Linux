package com.lagradost.player.impl

import com.lagradost.cloudstream3.utils.ExtractorLink
import com.lagradost.common.logging.AppLogger
import com.lagradost.player.api.MediaPlayer
import com.lagradost.player.api.PlayerState
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.io.BufferedReader
import java.io.PrintWriter
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketTimeoutException
import java.util.concurrent.atomic.AtomicReference
import java.util.concurrent.atomic.AtomicBoolean

class VlcPlayer : MediaPlayer {

    private val _state = MutableStateFlow(PlayerState())
    override val state: StateFlow<PlayerState> = _state.asStateFlow()

    private val playerScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var stdoutJob: Job? = null
    private var rcAcceptJob: Job? = null
    private var statePollJob: Job? = null
    private val currentProcess = AtomicReference<Process?>(null)
    private val paused = AtomicBoolean(false)
    private val rcLock = Any()
    private var rcServer: ServerSocket? = null
    private var rcSocket: Socket? = null
    private var rcReader: BufferedReader? = null
    private var rcWriter: PrintWriter? = null

    private fun closeRemoteControl() {
        rcAcceptJob?.cancel()
        statePollJob?.cancel()
        rcAcceptJob = null
        statePollJob = null
        synchronized(rcLock) {
            runCatching { rcReader?.close() }
            runCatching { rcWriter?.close() }
            runCatching { rcSocket?.close() }
            rcReader = null
            rcWriter = null
            rcSocket = null
        }
        runCatching { rcServer?.close() }
        rcServer = null
    }

    private fun killCurrent() {
        closeRemoteControl()
        currentProcess.getAndSet(null)?.let { proc ->
            try {
                proc.destroyForcibly()
                AppLogger.i("VlcPlayer: Killed previous VLC process.")
            } catch (_: Exception) {}
        }
        stdoutJob?.cancel()
        stdoutJob = null
        paused.set(false)
        _state.update {
            it.copy(
                isPlaying = false,
                isPaused = false,
                currentUrl = null,
                position = 0,
                duration = 0,
                isFinished = false,
            )
        }
    }

    override suspend fun play(link: ExtractorLink, title: String?, subtitles: List<String>, startPositionMs: Long): Result<Unit> =
        withContext(Dispatchers.IO) {
            try {
                _state.update { it.copy(isLoading = true, error = null) }

                val validated = PlayerLinkHandler.validate(link, title).getOrElse {
                    _state.update { state -> state.copy(isLoading = false, error = it.message) }
                    return@withContext Result.failure(it)
                }

                val vlcExecutable = findVlcExecutable()
                    ?: run {
                        _state.update { state -> state.copy(isLoading = false, error = "VLC not found") }
                        return@withContext Result.failure(IllegalStateException("VLC not found. Install VLC or use MPV."))
                    }

                val startSec = startPositionMs / 1000L

                val args = mutableListOf(vlcExecutable)

                // Keep VLC's normal desktop UI, but expose its local RC
                // interface so the shared MediaPlayer controls can pause,
                // seek, and report position/duration instead of treating VLC
                // as a fire-and-forget external process.
                killCurrent()
                val remoteControlServer = ServerSocket(
                    0,
                    1,
                    InetAddress.getLoopbackAddress(),
                )
                val rcAddress = remoteControlServer.inetAddress.hostAddress
                    .removePrefix("/")
                    .let { address -> if (address.contains(':')) "[$address]" else address }
                args.add("--extraintf=rc")
                args.add("--rc-host=$rcAddress:${remoteControlServer.localPort}")
                args.add("--rc-quiet")

                val userAgent = validated.headers.entries.firstOrNull { it.key.equals("user-agent", true) }?.value
                if (!userAgent.isNullOrBlank()) {
                    args.add("--http-user-agent=$userAgent")
                }

                val referer = validated.headers.entries.firstOrNull {
                    it.key.equals("referer", true) || it.key.equals("referrer", true)
                }?.value
                if (!referer.isNullOrBlank()) {
                    args.add("--http-referrer=$referer")
                }

                subtitles.filter { it.isNotBlank() }.forEach { sub ->
                    args.add("--sub-file=$sub")
                }

                if (startSec > 0) {
                    args.add("--start-time=$startSec")
                }

                // Force VLC to display the correct title instead of raw URL strings
                args.add("--meta-title=${validated.displayTitle}")
                args.add("--video-title=${validated.displayTitle}")

                if (validated.useUrlFile) {
                    val listFile = PlayerLinkHandler.writeUrlListFile(
                        "cloudstream_vlc_url_",
                        validated.displayTitle,
                        validated.url,
                    )
                    args.add(listFile.absolutePath)
                } else {
                    args.add(validated.url)
                }

                AppLogger.i("Launching VLC (${validated.streamKind}): ${validated.displayTitle}")

                val process = try {
                    ProcessBuilder(args)
                        .redirectErrorStream(true)
                        .start()
                } catch (error: Throwable) {
                    runCatching { remoteControlServer.close() }
                    throw error
                }

                currentProcess.set(process)
                rcServer = remoteControlServer
                paused.set(false)
                _state.update {
                    it.copy(
                        isPlaying = true,
                        isPaused = false,
                        isLoading = false,
                        isFinished = false,
                        position = startPositionMs,
                        currentUrl = validated.url,
                    )
                }

                rcAcceptJob = playerScope.launch(Dispatchers.IO) {
                    try {
                        val socket = remoteControlServer.accept()
                        if (currentProcess.get() !== process || !process.isAlive) {
                            socket.close()
                            return@launch
                        }
                        socket.soTimeout = 300
                        synchronized(rcLock) {
                            rcSocket = socket
                            rcReader = socket.getInputStream().bufferedReader()
                            rcWriter = PrintWriter(socket.getOutputStream(), true)
                        }
                        statePollJob = startStatePoller(process)
                    } catch (_: Exception) {
                        // VLC may exit before opening the optional RC socket.
                    } finally {
                        runCatching { remoteControlServer.close() }
                    }
                }

                stdoutJob = playerScope.launch(Dispatchers.IO) {
                    try {
                        process.inputStream.bufferedReader().use { reader ->
                            reader.lineSequence().forEach { AppLogger.i("VLC: $it") }
                        }
                        process.waitFor()
                    } catch (e: Exception) {
                        AppLogger.i("VLC process ended: ${e.message}")
                    } finally {
                        if (currentProcess.compareAndSet(process, null)) {
                            closeRemoteControl()
                            _state.update { it.copy(isPlaying = false, isFinished = true) }
                        }
                    }
                }

                Result.success(Unit)
            } catch (e: Exception) {
                AppLogger.i("VLC launch failed: ${e.message}")
                _state.update { it.copy(isPlaying = false, error = e.message) }
                Result.failure(e)
            }
        }

    override fun playLocal(file: File) {
        playerScope.launch {
            play(
                link = com.lagradost.cloudstream3.utils.newExtractorLink(
                    source = "Local",
                    name = "Local File",
                    url = file.absolutePath,
                ),
                title = file.name,
                subtitles = emptyList(),
                startPositionMs = 0,
            )
        }
    }

    override fun pause() {
        if (currentProcess.get() == null || paused.get()) return
        sendRemoteCommand("pause")
        paused.set(true)
        _state.update { it.copy(isPaused = true) }
    }

    override fun resume() {
        if (currentProcess.get() == null || !paused.get()) return
        sendRemoteCommand("play")
        paused.set(false)
        _state.update { it.copy(isPaused = false, isPlaying = true) }
    }

    override fun seek(positionMs: Long) {
        if (currentProcess.get() == null) return
        sendRemoteCommand("seek ${positionMs.coerceAtLeast(0) / 1000L}")
        _state.update { it.copy(position = positionMs.coerceAtLeast(0)) }
    }

    override fun stop() {
        killCurrent()
    }

    override fun destroy() {
        killCurrent()
    }

    private fun findVlcExecutable(): String? {
        try {
            fun probeCommand(vararg cmd: String): String? {
                return try {
                    val process = ProcessBuilder(cmd.toList())
                        .redirectErrorStream(true)
                        .start()
                    process.inputStream.bufferedReader().use { it.readLine()?.trim()?.takeIf { it.isNotBlank() } }
                } catch (_: Exception) {
                    null
                }
            }

            val os = System.getProperty("os.name").lowercase()
            if (os.contains("win")) {
                probeCommand("where", "vlc")?.let {
                    if (File(it).exists()) return it
                }
                val bases = listOfNotNull(
                    System.getenv("ProgramFiles"),
                    System.getenv("ProgramFiles(x86)"),
                    System.getenv("ProgramW6432"),
                )
                for (base in bases) {
                    val p = File(base, "VideoLAN\\VLC\\vlc.exe")
                    if (p.exists()) return p.absolutePath
                }
            } else {
                probeCommand("which", "vlc")?.let {
                    if (File(it).exists()) return it
                }
                listOf(
                    "/Applications/VLC.app/Contents/MacOS/VLC",
                    "/usr/local/bin/vlc",
                    "/opt/homebrew/bin/vlc",
                ).forEach { if (File(it).exists()) return it }
            }
        } catch (_: Exception) {
        }
        return null
    }

    private fun startStatePoller(process: Process): Job = playerScope.launch(Dispatchers.IO) {
        while (isActive && currentProcess.get() === process && process.isAlive) {
            val positionSeconds = sendRemoteCommand("get_time").firstInteger()
            val durationSeconds = sendRemoteCommand("get_length").firstInteger()
            if (positionSeconds != null || durationSeconds != null) {
                _state.update {
                    it.copy(
                        position = positionSeconds?.times(1000L) ?: it.position,
                        duration = durationSeconds?.times(1000L) ?: it.duration,
                        isPaused = paused.get(),
                    )
                }
            }
            kotlinx.coroutines.delay(500L)
        }
    }

    private fun sendRemoteCommand(command: String): String {
        synchronized(rcLock) {
            val writer = rcWriter ?: return ""
            val reader = rcReader ?: return ""
            return try {
                writer.println(command)
                writer.flush()

                val response = StringBuilder()
                while (true) {
                    val line = try {
                        reader.readLine()
                    } catch (_: SocketTimeoutException) {
                        null
                    }
                    if (line == null) break
                    response.append(line).append('\n')
                    if (line.trimStart().startsWith(">")) break
                }
                response.toString()
            } catch (_: Exception) {
                ""
            }
        }
    }

    private fun String.firstInteger(): Long? {
        return Regex("(?<![0-9])-?[0-9]+(?:\\.[0-9]+)?")
            .find(this)
            ?.value
            ?.toDoubleOrNull()
            ?.toLong()
    }
}
