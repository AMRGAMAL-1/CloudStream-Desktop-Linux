package com.lagradost.cloudstream3.desktop.ui.components

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.lagradost.common.storage.DesktopDataStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import java.awt.Desktop
import java.net.URI

const val DISCORD_INVITE_URL = "https://discord.gg/cU6Fr7Wx4"
private const val PREF_KEY_DISMISSED_PRE_ALPHA = "has_dismissed_pre_alpha_notice"

fun openDiscordInvite() {
    try {
        if (Desktop.isDesktopSupported() && Desktop.getDesktop().isSupported(Desktop.Action.BROWSE)) {
            Desktop.getDesktop().browse(URI(DISCORD_INVITE_URL))
        }
    } catch (_: Throwable) {}
}

@Composable
fun PreAlphaNoticeDialog() {
    val scope = rememberCoroutineScope()
    var showDialog by remember {
        mutableStateOf(!(DesktopDataStore.getKey<Boolean>(PREF_KEY_DISMISSED_PRE_ALPHA) ?: false))
    }
    var dontShowAgain by remember { mutableStateOf(false) }

    if (!showDialog) return

    val handleDismiss = {
        if (dontShowAgain) {
            scope.launch(Dispatchers.IO) {
                DesktopDataStore.setKey(PREF_KEY_DISMISSED_PRE_ALPHA, true)
            }
        }
        showDialog = false
    }

    CloudstreamAlertDialog(
        show = showDialog,
        onDismissRequest = handleDismiss,
        title = {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                Icon(
                    imageVector = Icons.Default.Warning,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.size(24.dp),
                )
                Text(
                    text = "Early Pre-Alpha Preview",
                    fontWeight = FontWeight.Bold,
                    fontSize = 18.sp,
                )
            }
        },
        text = {
            Column(
                modifier = Modifier.fillMaxWidth().padding(top = 4.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Text(
                    text = "Welcome to CloudStream Desktop! Please note that this client is currently in active pre-alpha development and is not a final stable release. You may encounter visual glitches, missing features, and occasional crashes.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Text(
                    text = "We actively rely on tester feedback and bug reports to keep the app updated and fix issues quickly. Please join our Discord community (benene) to report bugs, submit logs, or get help!",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )

                // Checkbox: Don't show again
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(top = 4.dp)
                        .clickable { dontShowAgain = !dontShowAgain },
                ) {
                    Checkbox(
                        checked = dontShowAgain,
                        onCheckedChange = { dontShowAgain = it },
                    )
                    Spacer(Modifier.width(8.dp))
                    Text(
                        text = "Don't show this message on startup again",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurface,
                    )
                }
            }
        },
        confirmButton = {
            Button(
                onClick = {
                    openDiscordInvite()
                    handleDismiss()
                },
            ) {
                Text("Join Discord (benene)")
            }
        },
        dismissButton = {
            TextButton(
                onClick = handleDismiss,
            ) {
                Text("Continue")
            }
        },
    )
}
