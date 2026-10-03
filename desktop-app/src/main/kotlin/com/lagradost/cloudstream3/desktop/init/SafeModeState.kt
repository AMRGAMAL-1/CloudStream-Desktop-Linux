package com.lagradost.cloudstream3.desktop.init

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Global lifecycle state for CloudStream Desktop Safe Mode.
 * When active, third-party plugins, custom shaders, and non-essential preloads
 * are completely bypassed to guarantee reliable startup and crash recovery.
 */
object SafeModeState {
    private val _isSafeMode = MutableStateFlow(false)
    val isSafeMode: StateFlow<Boolean> = _isSafeMode.asStateFlow()

    fun enable() {
        _isSafeMode.value = true
    }

    fun disable() {
        _isSafeMode.value = false
    }
}
