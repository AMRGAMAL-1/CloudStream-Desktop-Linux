package com.lagradost.cloudstream3.desktop.network

import okhttp3.Call
import okhttp3.EventListener
import okhttp3.Response
import java.io.IOException
import java.lang.ref.WeakReference
import java.util.concurrent.ConcurrentHashMap

/**
 * EventListener that automatically closes unconsumed [Response] bodies when a [Call] completes.
 * Prevents connection pool starvation and socket buffer leaks when coroutines cancel or exit early.
 */
class LeakSafeEventListener : EventListener() {

    private val liveResponses = ConcurrentHashMap<Call, WeakReference<Response>>()

    override fun responseHeadersEnd(call: Call, response: Response) {
        liveResponses[call] = WeakReference(response)
    }

    override fun callEnd(call: Call) {
        closeIfLeaked(call)
    }

    override fun callFailed(call: Call, ioe: IOException) {
        closeIfLeaked(call)
    }

    private fun closeIfLeaked(call: Call) {
        val ref = liveResponses.remove(call) ?: return
        val response = ref.get() ?: return
        try {
            response.body.close()
        } catch (_: Throwable) {
            // Best-effort: ignore errors on already-closed bodies
        }
    }

    companion object {
        val FACTORY: Factory = Factory { LeakSafeEventListener() }
    }
}
