package com.lagradost.cloudstream3.desktop.subtitles

import org.junit.jupiter.api.Test
import java.io.ByteArrayOutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

class SubsourceSubtitleProviderTest {

    @Test
    fun testExtractSubtitleFromZip() {
        val srtContent = "1\n00:00:01,000 --> 00:00:04,000\nHello Subtitle Test\n"
        val bos = ByteArrayOutputStream()
        ZipOutputStream(bos).use { zos ->
            val entry = ZipEntry("movie.1080p.web.srt")
            zos.putNextEntry(entry)
            zos.write(srtContent.toByteArray(Charsets.UTF_8))
            zos.closeEntry()
        }
        val zipBytes = bos.toByteArray()

        val extracted = SubsourceSubtitleProvider.extractSubtitleBytes(zipBytes)
        assertNotNull(extracted)
        assertEquals(srtContent, String(extracted, Charsets.UTF_8))
    }

    @Test
    fun testExtractRawSubtitleBytes() {
        val rawSrt = "1\n00:00:01,000 --> 00:00:02,000\nPlain SRT Content\n"
        val rawBytes = rawSrt.toByteArray(Charsets.UTF_8)

        val result = SubsourceSubtitleProvider.extractSubtitleBytes(rawBytes)
        assertNotNull(result)
        assertEquals(rawSrt, String(result, Charsets.UTF_8))
    }

    @Test
    fun testLanguageMatchingLogic() {
        val preferred = listOf("eng,en", "spa,es")

        assertTrue(SubsourceSubtitleProvider.matchesAnyPreferred("English", preferred))
        assertTrue(SubsourceSubtitleProvider.matchesAnyPreferred("Spanish", preferred))
        assertTrue(SubsourceSubtitleProvider.matchesAnyPreferred("eng", preferred))

        assertFalse(SubsourceSubtitleProvider.matchesAnyPreferred("Japanese", preferred))
        assertFalse(SubsourceSubtitleProvider.matchesAnyPreferred("German", preferred))
    }

    @Test
    fun testLoadSubtitleBlank() {
        kotlinx.coroutines.runBlocking {
            val result = SubsourceSubtitleProvider.loadSubtitle("")
            kotlin.test.assertNull(result)
        }
    }
}
