package com.lagradost.cloudstream3.desktop.ui.components

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CloudDownload
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.lagradost.cloudstream3.desktop.download.AppDownloadManager
import com.lagradost.cloudstream3.desktop.download.TaskStatus
import com.lagradost.cloudstream3.desktop.torrent.DesktopTorrentEngine

@Composable
fun TorrServerInstallRequiredDialog(
    show: Boolean,
    onDismiss: () -> Unit,
    onInstalled: () -> Unit,
) {
    if (!show) return

    val tasks by AppDownloadManager.tasks.collectAsState()
    val torrTask = tasks.firstOrNull { it.id == "torrserver" }
    val isDownloading = torrTask?.status == TaskStatus.RUNNING

    LaunchedEffect(torrTask?.status) {
        if (torrTask?.status == TaskStatus.COMPLETED && DesktopTorrentEngine.binary.isInstalled()) {
            onInstalled()
        }
    }

    val warningColor = Color(0xFFF59E0B)

    CloudstreamAlertDialog(
        show = show,
        onDismissRequest = {
            if (!isDownloading) onDismiss()
        },
        title = {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                Icon(
                    imageVector = Icons.Default.CloudDownload,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.size(26.dp),
                )
                Text(
                    text = "TorrServer Engine Required",
                    style = MaterialTheme.typography.titleLarge,
                    fontWeight = FontWeight.Bold,
                    fontSize = 19.sp,
                )
            }
        },
        text = {
            Column(
                modifier = Modifier.fillMaxWidth().padding(top = 4.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Surface(
                    shape = RoundedCornerShape(10.dp),
                    color = warningColor.copy(alpha = 0.12f),
                    border = BorderStroke(1.dp, warningColor.copy(alpha = 0.35f)),
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Row(
                        modifier = Modifier.padding(12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(10.dp),
                    ) {
                        Icon(
                            imageVector = Icons.Default.Warning,
                            contentDescription = null,
                            tint = warningColor,
                            modifier = Modifier.size(22.dp),
                        )
                        Text(
                            text = "This stream is peer-to-peer (torrent) based and requires the local TorrServer cache engine.",
                            style = MaterialTheme.typography.bodyMedium,
                            fontWeight = FontWeight.SemiBold,
                            color = warningColor,
                            fontSize = 13.5.sp,
                            lineHeight = 18.sp,
                        )
                    }
                }

                Text(
                    text = "TorrServer is an open-source background streaming daemon that downloads and caches torrent pieces into RAM so they can be smoothly decoded by the player.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurface,
                    lineHeight = 20.sp,
                )

                Surface(
                    shape = RoundedCornerShape(8.dp),
                    color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.4f),
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Column(
                        modifier = Modifier.padding(10.dp),
                        verticalArrangement = Arrangement.spacedBy(4.dp),
                    ) {
                        Text(
                            text = "Source: github.com/YouROK/TorrServer (Official Releases)",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        Text(
                            text = "Path: %AppData%/CloudStream/torrserver/bin/TorrServer.exe",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }

                if (isDownloading && torrTask != null) {
                    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                        Text(
                            text = "Downloading TorrServer: ${torrTask.downloadedMB} / ${torrTask.totalMB} (${torrTask.speedFormatted})",
                            style = MaterialTheme.typography.labelMedium,
                            color = MaterialTheme.colorScheme.primary,
                            fontWeight = FontWeight.SemiBold,
                        )
                        if (torrTask.progress >= 0f) {
                            LinearProgressIndicator(
                                progress = { torrTask.progress },
                                modifier = Modifier.fillMaxWidth().height(6.dp),
                            )
                        } else {
                            LinearProgressIndicator(
                                modifier = Modifier.fillMaxWidth().height(6.dp),
                            )
                        }
                    }
                }
            }
        },
        confirmButton = {
            if (isDownloading) {
                OutlinedButton(
                    onClick = { AppDownloadManager.cancelDownload("torrserver") },
                    shape = RoundedCornerShape(10.dp),
                ) {
                    Text("Cancel Download")
                }
            } else {
                Button(
                    onClick = {
                        DesktopTorrentEngine.binary.downloadWithManager {
                            onInstalled()
                        }
                    },
                    shape = RoundedCornerShape(10.dp),
                ) {
                    Text("Install TorrServer (~28 MB)", fontWeight = FontWeight.Bold)
                }
            }
        },
        dismissButton = {
            if (!isDownloading) {
                OutlinedButton(
                    onClick = onDismiss,
                    shape = RoundedCornerShape(10.dp),
                ) {
                    Text("Cancel")
                }
            }
        },
    )
}
