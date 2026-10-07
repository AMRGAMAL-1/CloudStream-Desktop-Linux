package com.lagradost.cloudstream3.desktop.init

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.layout.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import java.awt.GraphicsEnvironment
import java.awt.Point
import java.awt.Rectangle
import java.awt.Window
import javax.swing.JFrame
import kotlin.math.roundToInt

private const val DOUBLE_CLICK_TIMEOUT_MS = 400L
private const val TITLE_BAR_HEIGHT_DP = 36

@Composable
internal fun TreelandWindowTitleBar(
    window: Window,
    onClose: () -> Unit,
) {
    var isMaximized by remember(window) { mutableStateOf(true) }
    var normalBounds by remember(window) { mutableStateOf<Rectangle?>(null) }

    fun workArea(): Rectangle = GraphicsEnvironment
        .getLocalGraphicsEnvironment()
        .maximumWindowBounds

    fun savedNormalBounds(): Rectangle {
        val frame = window as? JFrame
        val saved = frame?.rootPane?.getClientProperty(TREELAND_RESTORE_BOUNDS_KEY) as? Rectangle
        if (saved != null) return Rectangle(saved)

        val screen = window.graphicsConfiguration?.bounds
            ?: GraphicsEnvironment.getLocalGraphicsEnvironment().defaultScreenDevice.defaultConfiguration.bounds
        val width = (screen.width * 0.7f).roundToInt().coerceAtLeast(1000)
        val height = (screen.height * 0.7f).roundToInt().coerceAtLeast(700)
        return Rectangle(
            screen.x + (screen.width - width) / 2,
            screen.y + (screen.height - height) / 2,
            width,
            height,
        )
    }

    fun restoreWindow() {
        val bounds = normalBounds ?: savedNormalBounds()
        normalBounds = Rectangle(bounds)
        isMaximized = false
        window.setBounds(Rectangle(bounds))
        window.validate()
    }

    fun maximizeWindow() {
        normalBounds = Rectangle(window.bounds)
        isMaximized = true
        window.setBounds(workArea())
        window.validate()
    }

    fun toggleMaximize() {
        if (isMaximized) restoreWindow() else maximizeWindow()
    }

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .height(TITLE_BAR_HEIGHT_DP.dp)
            .background(Color(0xFF292929)),
    ) {
        Row(
            modifier = Modifier.fillMaxSize(),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(
                modifier = Modifier
                    .weight(1f)
                    .fillMaxHeight()
                    .treelandDragArea(
                        window = window,
                        onDoubleClick = ::toggleMaximize,
                        onDragStart = {
                            if (isMaximized) restoreWindow()
                        },
                        onDrag = { dx, dy ->
                            val current = window.location
                            window.location = Point(current.x + dx, current.y + dy)
                        },
                    ),
                contentAlignment = Alignment.Center,
            ) {
                androidx.compose.material3.Text(
                    text = "CloudStream Desktop",
                    color = Color(0xFFD8D8D8),
                    fontSize = 13.sp,
                    fontWeight = FontWeight.Medium,
                )
            }

            TreelandWindowButton(
                label = "−",
                contentDescription = "Minimize",
                onClick = {
                    val frame = window as? JFrame
                    if (frame != null) frame.extendedState = frame.extendedState or JFrame.ICONIFIED
                },
            )
            TreelandWindowButton(
                label = if (isMaximized) "❐" else "□",
                contentDescription = if (isMaximized) "Restore" else "Maximize",
                onClick = ::toggleMaximize,
            )
            TreelandWindowButton(
                label = "×",
                contentDescription = "Close",
                onClick = onClose,
                closeButton = true,
            )
        }
    }
}

@Composable
private fun TreelandWindowButton(
    label: String,
    contentDescription: String,
    onClick: () -> Unit,
    closeButton: Boolean = false,
) {
    Box(
        modifier = Modifier
            .width(46.dp)
            .fillMaxHeight()
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        androidx.compose.material3.Text(
            text = label,
            color = if (closeButton) Color(0xFFE8E8E8) else Color(0xFFD8D8D8),
            fontSize = if (closeButton) 22.sp else 17.sp,
            fontWeight = FontWeight.Light,
        )
    }
}

private fun Modifier.treelandDragArea(
    window: Window,
    onDoubleClick: () -> Unit,
    onDragStart: (Offset) -> Unit,
    onDrag: (Int, Int) -> Unit,
): Modifier = pointerInput(window) {
    var lastClickTime = 0L
    var lastClickPosition = Offset.Unspecified

    awaitPointerEventScope {
        while (true) {
            val downEvent = awaitPointerEvent(pass = PointerEventPass.Initial)
            val down = downEvent.changes.firstOrNull { it.pressed } ?: continue
            var dragging = false
            var totalDx = 0f
            var totalDy = 0f

            while (true) {
                val event = awaitPointerEvent(pass = PointerEventPass.Initial)
                val change = event.changes.firstOrNull { it.id == down.id } ?: break
                if (!change.pressed) {
                    if (!dragging) {
                        val now = System.currentTimeMillis()
                        val closeToPrevious = lastClickPosition != Offset.Unspecified &&
                            (change.position - lastClickPosition).getDistance() < 32f
                        if (closeToPrevious && now - lastClickTime <= DOUBLE_CLICK_TIMEOUT_MS) {
                            onDoubleClick()
                            lastClickTime = 0L
                        } else {
                            lastClickTime = now
                            lastClickPosition = change.position
                        }
                    }
                    break
                }

                val delta = change.position - change.previousPosition
                totalDx += delta.x
                totalDy += delta.y
                if (!dragging && (totalDx * totalDx + totalDy * totalDy) > viewConfiguration.touchSlop * viewConfiguration.touchSlop) {
                    dragging = true
                    onDragStart(down.position)
                }
                if (dragging) {
                    onDrag(delta.x.roundToInt(), delta.y.roundToInt())
                    change.consume()
                }
            }
        }
    }
}
