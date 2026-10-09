package com.lagradost.cloudstream3.desktop.ui.components

import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.lagradost.cloudstream3.desktop.ui.theme.AppearanceConfig
import com.lagradost.cloudstream3.desktop.ui.theme.accentColorFromName
import com.lagradost.cloudstream3.desktop.ui.theme.buildDesktopColors

@Composable
fun AppStartupSplashScreen() {
    // Snapshot theme values once so background startup refreshes do not invalidate the animation.
    val desktopTheme = remember {
        buildDesktopColors(
            primaryColor = accentColorFromName(
                AppearanceConfig.themeAccent.value,
                AppearanceConfig.customThemeAccent.value,
            ),
            isLightMode = AppearanceConfig.isLightMode.value,
            isAmoled = AppearanceConfig.amoledMode.value,
            appThemeBackground = AppearanceConfig.appThemeBackground.value,
            customBgHex = AppearanceConfig.customAppThemeBackground.value,
        )
    }
    val transition = rememberInfiniteTransition(label = "splash")
    val logoScale by transition.animateFloat(
        initialValue = 0.96f,
        targetValue = 1.04f,
        animationSpec = infiniteRepeatable(
            animation = tween(durationMillis = 1800, easing = FastOutSlowInEasing),
            repeatMode = RepeatMode.Reverse,
        ),
        label = "logo-pulse",
    )
    val glowAlpha by transition.animateFloat(
        initialValue = 0.22f,
        targetValue = 0.5f,
        animationSpec = infiniteRepeatable(
            animation = tween(durationMillis = 1800, easing = FastOutSlowInEasing),
            repeatMode = RepeatMode.Reverse,
        ),
        label = "glow-pulse",
    )
    val spinnerRotation by transition.animateFloat(
        initialValue = 0f,
        targetValue = 360f,
        animationSpec = infiniteRepeatable(
            animation = tween(durationMillis = 900, easing = LinearEasing),
        ),
        label = "spinner-rotation",
    )
    val backgroundBrush = if (desktopTheme.isLightMode) {
        Brush.linearGradient(
            colors = listOf(desktopTheme.Background, desktopTheme.SurfaceCard.copy(alpha = 0.94f)),
        )
    } else {
        Brush.linearGradient(
            colors = listOf(
                desktopTheme.Background,
                desktopTheme.SurfaceCard.copy(alpha = 0.88f),
                desktopTheme.Background,
            ),
        )
    }
    val logoGlow = desktopTheme.Accent.copy(alpha = 0.55f)
    val spinnerTrack = desktopTheme.TextPrimary.copy(alpha = if (desktopTheme.isLightMode) 0.18f else 0.13f)

    Box(
        modifier = Modifier.fillMaxSize().background(backgroundBrush),
        contentAlignment = Alignment.Center,
    ) {
        Box(
            modifier = Modifier
                .size(420.dp)
                .graphicsLayer { alpha = glowAlpha }
                .background(
                    Brush.radialGradient(colors = listOf(logoGlow, Color.Transparent)),
                    CircleShape,
                ),
        )

        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Image(
                painter = painterResource("splash_logo_transparent.png"),
                contentDescription = "CloudStream",
                contentScale = ContentScale.Fit,
                modifier = Modifier
                    .size(176.dp)
                    .graphicsLayer {
                        scaleX = logoScale
                        scaleY = logoScale
                    },
            )
            Spacer(Modifier.height(24.dp))
            Text(
                text = "CLOUDSTREAM",
                color = desktopTheme.TextPrimary,
                fontSize = 23.sp,
                fontWeight = FontWeight.SemiBold,
                letterSpacing = 4.sp,
            )
            Spacer(Modifier.height(8.dp))
            Text(
                text = "DESKTOP",
                color = desktopTheme.Accent.copy(alpha = 0.9f),
                fontSize = 11.sp,
                fontWeight = FontWeight.Medium,
                letterSpacing = 5.sp,
            )
            Spacer(Modifier.height(34.dp))
            Canvas(
                modifier = Modifier.size(32.dp).graphicsLayer { rotationZ = spinnerRotation },
            ) {
                val strokeWidth = 2.dp.toPx()
                val radius = (size.minDimension - strokeWidth) / 2f
                drawCircle(
                    color = spinnerTrack,
                    radius = radius,
                    style = Stroke(width = strokeWidth),
                )
                drawArc(
                    color = desktopTheme.Accent.copy(alpha = 0.9f),
                    startAngle = -90f,
                    sweepAngle = 112f,
                    useCenter = false,
                    style = Stroke(width = strokeWidth, cap = StrokeCap.Round),
                )
            }
            Spacer(Modifier.height(12.dp))
            Text(
                text = "Preparing your experience…",
                color = desktopTheme.TextMuted,
                fontSize = 12.sp,
            )
            Spacer(Modifier.height(16.dp))
            // Edition credit pill: breathes with the same glow pulse cycle so
            // it feels part of the splash animation, in theme colors.
            Box(
                modifier = Modifier
                    .graphicsLayer { alpha = (0.7f + glowAlpha * 0.6f).coerceAtMost(1f) }
                    .background(
                        color = desktopTheme.Accent.copy(alpha = 0.10f),
                        shape = CircleShape,
                    )
                    .border(
                        width = 1.dp,
                        color = desktopTheme.Accent.copy(alpha = 0.35f),
                        shape = CircleShape,
                    )
                    .padding(horizontal = 20.dp, vertical = 9.dp),
                contentAlignment = Alignment.Center,
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        text = "Linux Edition",
                        color = desktopTheme.TextPrimary,
                        fontSize = 12.sp,
                        fontWeight = FontWeight.SemiBold,
                        letterSpacing = 1.5.sp,
                    )
                    Text(
                        text = "  •  by AMR GAMAL",
                        color = desktopTheme.Accent,
                        fontSize = 12.sp,
                        fontWeight = FontWeight.Medium,
                        letterSpacing = 1.5.sp,
                    )
                }
            }
        }
    }
}
