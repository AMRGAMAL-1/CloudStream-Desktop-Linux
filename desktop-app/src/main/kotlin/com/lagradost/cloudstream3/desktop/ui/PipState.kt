package com.lagradost.cloudstream3.desktop.ui

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

object PipState {
    /** PiP is implemented by the native top-level window path on Windows and Linux/X11. */
    val isSupported: Boolean = System.getProperty("os.name", "").let {
        it.contains("win", ignoreCase = true) || it.contains("linux", ignoreCase = true)
    }

    /** Keep the controls available on supported desktop platforms. */
    val unsupportedControlsCss: String = if (isSupported) {
        ""
    } else {
        """
        #pipBtn,
        #pipContextBtn {
            display: none !important;
        }
        """.trimIndent()
    }

    private val _isPipMode = MutableStateFlow(false)
    val isPipMode: StateFlow<Boolean> = _isPipMode.asStateFlow()

    fun setPipMode(enabled: Boolean) {
        if (!isSupported && enabled) return
        _isPipMode.value = enabled
    }
}
