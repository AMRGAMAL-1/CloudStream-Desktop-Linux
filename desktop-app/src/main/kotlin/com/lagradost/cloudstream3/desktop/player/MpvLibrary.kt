@file:Suppress("ktlint:standard:property-naming")

package com.lagradost.cloudstream3.desktop.player

import com.lagradost.common.logging.AppLogger
import com.lagradost.cloudstream3.desktop.player.webview.NativePlayerBridge
import com.sun.jna.Library
import com.sun.jna.Pointer
import com.sun.jna.Structure

interface MpvLibrary : Library {
    fun mpv_create(): Pointer?
    fun mpv_initialize(handle: Pointer): Int
    fun mpv_set_option_string(ctx: Pointer, name: String, data: String): Int
    fun mpv_get_property_string(ctx: Pointer, name: String): Pointer?
    fun mpv_set_property_string(ctx: Pointer, name: String, data: String): Int
    fun mpv_get_property(ctx: Pointer, name: String, format: Int, data: Pointer): Int
    fun mpv_command_string(ctx: Pointer, args: String): Int
    fun mpv_command(ctx: Pointer, args: Array<String?>): Int
    fun mpv_observe_property(ctx: Pointer, reply_userdata: Long, name: String, format: Int): Int
    fun mpv_wait_event(ctx: Pointer, timeout: Double): Pointer?
    fun mpv_request_log_messages(ctx: Pointer, min_level: String): Int
    fun mpv_free(data: Pointer)
    fun mpv_terminate_destroy(handle: Pointer)

    @Suppress("ktlint:standard:property-naming")
    @Structure.FieldOrder("event_id", "error", "reply_userdata", "data")
    open class MpvEvent(p: Pointer? = null) : Structure(p) {
        @JvmField var event_id: Int = 0

        @JvmField var error: Int = 0

        @JvmField var reply_userdata: Long = 0

        @JvmField var data: Pointer? = null
        init {
            p?.let { read() }
        }
    }

    @Structure.FieldOrder("name", "format", "data")
    open class MpvEventProperty(p: Pointer? = null) : Structure(p) {
        @JvmField var name: String? = null

        @JvmField var format: Int = 0

        @JvmField var data: Pointer? = null
        init {
            p?.let { read() }
        }
    }

    @Structure.FieldOrder("reason", "error", "playlist_entry_id", "playlist_insert_id", "playlist_insert_num_entries")
    open class MpvEventEndFile(p: Pointer? = null) : Structure(p) {
        @JvmField var reason: Int = 0

        @JvmField var error: Int = 0

        @JvmField var playlist_entry_id: Long = 0

        @JvmField var playlist_insert_id: Long = 0

        @JvmField var playlist_insert_num_entries: Int = 0
        init {
            p?.let { read() }
        }
    }

    @Structure.FieldOrder("prefix", "level", "text", "log_level")
    open class MpvEventLogMessage(p: Pointer? = null) : Structure(p) {
        @JvmField var prefix: String? = null

        @JvmField var level: String? = null

        @JvmField var text: String? = null

        @JvmField var log_level: Int = 0
        init {
            p?.let { read() }
        }
    }

    companion object {
        private val isWindows = System.getProperty("os.name", "").contains("win", ignoreCase = true)

        fun isAvailable(): Boolean = runCatching {
            INSTANCE
            true
        }.getOrDefault(false)

        fun getPropertyString(ctx: Pointer, name: String): String? {
            return try {
                val ptr = INSTANCE.mpv_get_property_string(ctx, name) ?: return null
                val str = ptr.getString(0)
                INSTANCE.mpv_free(ptr)
                str
            } catch (t: Throwable) {
                null
            }
        }

        fun getPropertyDouble(ctx: Pointer, name: String, fallback: Double = -1.0): Double {
            return try {
                val str = getPropertyString(ctx, name) ?: return fallback
                str.replace(",", ".").toDoubleOrNull() ?: fallback
            } catch (t: Throwable) {
                AppLogger.e("DEBUG_MPV: getPropertyDouble failed for $name: ${t.stackTraceToString()}")
                fallback
            }
        }

        val INSTANCE: MpvLibrary by lazy {
            if (!isWindows) {
                // MPV rejects a non-C native numeric locale during mpv_create().
                // The Linux bridge is already loaded by the desktop app and sets
                // LC_NUMERIC before JNA resolves the MPV symbols.
                runCatching { NativePlayerBridge.configureMpvLocale() }
            }

            val targets = if (isWindows) {
                listOf("libmpv-2", "mpv-2", "mpv-1", "mpv-3.dll")
            } else {
                // JNA prefixes a bare library name with `lib` on Linux. Starting
                // with `mpv` therefore resolves the system libmpv.so correctly;
                // `libmpv-2` would incorrectly become liblibmpv-2.so.
                listOf("mpv", "libmpv", "mpv-2", "libmpv.so.2", "libmpv.so.1")
            }
            var loaded: MpvLibrary? = null
            for (target in targets) {
                try {
                    loaded = com.sun.jna.Native.load(target, MpvLibrary::class.java) as MpvLibrary
                    AppLogger.i("Player:MPV", "Successfully loaded native mpv library: $target")
                    break
                } catch (e: UnsatisfiedLinkError) {
                    AppLogger.e("Player:MPV", "Failed to load native mpv library $target: ${e.message}")
                } catch (e: IllegalArgumentException) {
                    AppLogger.e("Player:MPV", "Illegal arg for native mpv library $target: ${e.message}")
                }
            }
            loaded ?: throw RuntimeException("Failed to load native MPV library. Please ensure mpv is installed and in your system PATH.")
        }
    }
}
