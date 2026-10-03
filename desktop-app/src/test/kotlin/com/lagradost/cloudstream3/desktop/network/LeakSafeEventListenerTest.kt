package com.lagradost.cloudstream3.desktop.network

import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Request
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.junit.jupiter.api.Test
import java.io.IOException
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class LeakSafeEventListenerTest {

    private class TestResponseBody(val closed: AtomicBoolean) : okhttp3.ResponseBody() {
        override fun contentType(): okhttp3.MediaType? = null
        override fun contentLength(): Long = 0L
        override fun source(): okio.BufferedSource = okio.Buffer()
        override fun close() {
            closed.set(true)
            super.close()
        }
    }

    @Test
    fun testUnclosedBodyIsClosedOnCallEnd() {
        val listener = LeakSafeEventListener()
        val closed = AtomicBoolean(false)
        val testBody = TestResponseBody(closed)

        val client = OkHttpClient()
        val request = Request.Builder().url("https://example.com").build()
        val call = client.newCall(request)

        val response = Response.Builder()
            .request(request)
            .protocol(Protocol.HTTP_1_1)
            .code(200)
            .message("OK")
            .body(testBody)
            .build()

        listener.responseHeadersEnd(call, response)
        assertFalse(closed.get(), "Body should not be closed before callEnd")

        listener.callEnd(call)
        assertTrue(closed.get(), "Body must be closed when callEnd is triggered")
    }

    @Test
    fun testUnclosedBodyIsClosedOnCallFailed() {
        val listener = LeakSafeEventListener()
        val closed = AtomicBoolean(false)
        val testBody = TestResponseBody(closed)

        val client = OkHttpClient()
        val request = Request.Builder().url("https://example.com").build()
        val call = client.newCall(request)

        val response = Response.Builder()
            .request(request)
            .protocol(Protocol.HTTP_1_1)
            .code(200)
            .message("OK")
            .body(testBody)
            .build()

        listener.responseHeadersEnd(call, response)
        assertFalse(closed.get(), "Body should not be closed before callFailed")

        listener.callFailed(call, IOException("Simulated network timeout"))
        assertTrue(closed.get(), "Body must be closed when callFailed is triggered")
    }

    @Test
    fun testAlreadyClosedBodyDoesNotThrow() {
        val listener = LeakSafeEventListener()
        val client = OkHttpClient()
        val request = Request.Builder().url("https://example.com").build()
        val call = client.newCall(request)

        val realBody = "Test data".toResponseBody()
        val response = Response.Builder()
            .request(request)
            .protocol(Protocol.HTTP_1_1)
            .code(200)
            .message("OK")
            .body(realBody)
            .build()

        listener.responseHeadersEnd(call, response)
        realBody.close()

        // Verify that closing an already-closed body does not throw
        listener.callEnd(call)
    }
}
