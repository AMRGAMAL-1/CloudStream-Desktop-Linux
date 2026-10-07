package com.lagradost.cloudstream3.desktop.ui.components

import androidx.compose.foundation.text.InlineTextContent
import androidx.compose.material3.LocalTextStyle
import androidx.compose.material3.Text as MaterialText
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextLayoutResult
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.TextUnit
import com.lagradost.cloudstream3.desktop.ui.theme.AppearanceConfig
import com.lagradost.cloudstream3.desktop.ui.theme.getArabicFontFamily

/** Text helper for callers that need Cocon applied only to Arabic glyph runs. */
@Composable
fun ArabicAwareText(
    text: String,
    modifier: androidx.compose.ui.Modifier = androidx.compose.ui.Modifier,
    color: Color = Color.Unspecified,
    fontSize: TextUnit = TextUnit.Unspecified,
    fontStyle: FontStyle? = null,
    fontWeight: FontWeight? = null,
    fontFamily: FontFamily? = null,
    textAlign: TextAlign? = null,
    letterSpacing: TextUnit = TextUnit.Unspecified,
    lineHeight: TextUnit = TextUnit.Unspecified,
    overflow: TextOverflow = TextOverflow.Clip,
    maxLines: Int = Int.MAX_VALUE,
    style: TextStyle = LocalTextStyle.current,
    textDecoration: TextDecoration? = null,
    softWrap: Boolean = true,
    minLines: Int = 1,
    inlineContent: Map<String, InlineTextContent> = emptyMap(),
    onTextLayout: (TextLayoutResult) -> Unit = {},
) {
    val enabled by AppearanceConfig.arabicFontEnabled.collectAsState()
    MaterialText(
        text = withArabicFont(AnnotatedString(text), enabled),
        modifier = modifier,
        color = color,
        fontSize = fontSize,
        fontStyle = fontStyle,
        fontWeight = fontWeight,
        fontFamily = fontFamily,
        textAlign = textAlign,
        letterSpacing = letterSpacing,
        lineHeight = lineHeight,
        overflow = overflow,
        maxLines = maxLines,
        style = style,
        textDecoration = textDecoration,
        softWrap = softWrap,
        minLines = minLines,
        inlineContent = inlineContent,
        onTextLayout = onTextLayout,
    )
}

@Composable
fun ArabicAwareText(
    text: AnnotatedString,
    modifier: androidx.compose.ui.Modifier = androidx.compose.ui.Modifier,
    color: Color = Color.Unspecified,
    fontSize: TextUnit = TextUnit.Unspecified,
    fontStyle: FontStyle? = null,
    fontWeight: FontWeight? = null,
    fontFamily: FontFamily? = null,
    textAlign: TextAlign? = null,
    letterSpacing: TextUnit = TextUnit.Unspecified,
    lineHeight: TextUnit = TextUnit.Unspecified,
    overflow: TextOverflow = TextOverflow.Clip,
    maxLines: Int = Int.MAX_VALUE,
    style: TextStyle = LocalTextStyle.current,
    textDecoration: TextDecoration? = null,
    softWrap: Boolean = true,
    minLines: Int = 1,
    inlineContent: Map<String, InlineTextContent> = emptyMap(),
    onTextLayout: (TextLayoutResult) -> Unit = {},
) {
    val enabled by AppearanceConfig.arabicFontEnabled.collectAsState()
    MaterialText(
        text = withArabicFont(text, enabled),
        modifier = modifier,
        color = color,
        fontSize = fontSize,
        fontStyle = fontStyle,
        fontWeight = fontWeight,
        fontFamily = fontFamily,
        textAlign = textAlign,
        letterSpacing = letterSpacing,
        lineHeight = lineHeight,
        overflow = overflow,
        maxLines = maxLines,
        style = style,
        textDecoration = textDecoration,
        softWrap = softWrap,
        minLines = minLines,
        inlineContent = inlineContent,
        onTextLayout = onTextLayout,
    )
}

fun withArabicFont(
    text: AnnotatedString,
    enabled: Boolean = AppearanceConfig.arabicFontEnabled.value,
): AnnotatedString {
    if (System.getProperty("cloudstream.disableArabicFont") == "true" || !enabled) return text
    val arabicFont = getArabicFontFamily() ?: return text

    return buildAnnotatedString {
        append(text)
        var runStart = -1
        var index = 0
        while (index < text.length) {
            val codePoint = text.text.codePointAt(index)
            val isArabic = Character.UnicodeScript.of(codePoint) == Character.UnicodeScript.ARABIC ||
                codePoint in 0x0600..0x060F || codePoint in 0x06DD..0x06ED
            val next = index + Character.charCount(codePoint)
            if (isArabic && runStart < 0) runStart = index
            if (!isArabic && runStart >= 0) {
                addStyle(SpanStyle(fontFamily = arabicFont), runStart, index)
                runStart = -1
            }
            index = next
        }
        if (runStart >= 0) addStyle(SpanStyle(fontFamily = arabicFont), runStart, text.length)
    }
}
