package com.lagradost.cloudstream3.desktop.updates

import org.junit.jupiter.api.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

class AppUpdateChannelTest {
    @Test
    fun `linux channel accepts only linux release tags`() {
        if (!System.getProperty("os.name").orEmpty().contains("linux", ignoreCase = true)) return

        assertEquals("0.1.10", AppUpdateChannel.releaseVersion("linux-v0.1.10"))
        assertNull(AppUpdateChannel.releaseVersion("v0.1.10"))
        assertTrue(AppUpdateChannel.isSupportedRelease("linux-v0.1.10", draft = false, prerelease = false))
    }
}
