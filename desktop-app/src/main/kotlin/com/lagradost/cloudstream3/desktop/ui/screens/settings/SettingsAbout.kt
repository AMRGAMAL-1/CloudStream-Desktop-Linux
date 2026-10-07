package com.lagradost.cloudstream3.desktop.ui.screens.settings

import com.lagradost.cloudstream3.desktop.ui.components.ArabicAwareText

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.OpenInNew
import androidx.compose.material.icons.filled.Gavel
import androidx.compose.material.icons.filled.Info
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.lagradost.cloudstream3.desktop.AppConfig
import com.lagradost.cloudstream3.desktop.utils.appScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

@Composable
fun SettingsAbout() {
    Column(
        modifier = Modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        SettingsGroupCard(title = "CloudStream Desktop") {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(vertical = 12.dp, horizontal = 4.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Surface(
                    shape = RoundedCornerShape(8.dp),
                    color = MaterialTheme.colorScheme.errorContainer.copy(alpha = 0.85f),
                ) {
                    ArabicAwareText(
                        "UNOFFICIAL DESKTOP CLIENT",
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onErrorContainer,
                        fontWeight = FontWeight.Bold,
                        letterSpacing = 0.5.sp,
                        modifier = Modifier.padding(horizontal = 10.dp, vertical = 4.dp),
                    )
                }
                Spacer(modifier = Modifier.height(12.dp))
                ArabicAwareText(
                    "An independent Linux edition of CloudStream Desktop, adapted and enhanced by AMR GAMAL for a smooth desktop experience.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    textAlign = TextAlign.Center,
                )
                Spacer(modifier = Modifier.height(8.dp))
                ArabicAwareText(
                    "Built with care for Linux users who love CloudStream.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    textAlign = TextAlign.Center,
                )
            }
        }

        SettingsGroupCard(title = "Developer & Linux Edition Maintainer") {
            SettingsNavigationItem(
                label = "AMR GAMAL",
                subtitle = "Developer & Linux Edition Maintainer",
                onClick = {},
            )
            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.35f))
            SettingsNavigationItem(
                label = "Telegram Channel",
                subtitle = "@AMRGAMAL_STORE",
                onClick = { openUrl("https://t.me/AMRGAMAL_STORE") },
            )
            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.35f))
            SettingsNavigationItem(
                label = "Telegram Community",
                subtitle = "@AMRGAMAL_CHAT",
                onClick = { openUrl("https://t.me/AMRGAMAL_CHAT") },
            )
            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.35f))
            SettingsNavigationItem(
                label = "Personal Telegram",
                subtitle = "@AMRGAMAL1",
                onClick = { openUrl("https://t.me/AMRGAMAL1") },
            )
        }

        SettingsGroupCard(title = "Desktop Community & Support") {
            SettingsNavigationItem(
                label = "Community Discord",
                subtitle = "Join Discord to report bugs or any issue.",
                onClick = { openUrl("https://discord.gg/cU6Fr7Wx4") },
            )
            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.35f))
            SettingsNavigationItem(
                label = "Desktop Source Code",
                subtitle = "View repository, report desktop issues, and inspect release builds.",
                onClick = { openUrl("https://github.com/errorcode26/CS3-desktop-client-unofficial") },
            )
        }

        SettingsGroupCard(title = "Upstream Core & Documentation") {
            SettingsNavigationItem(
                label = "Upstream Android Project",
                subtitle = "The official Android app whose core library powers this desktop client.",
                onClick = { openUrl("https://github.com/recloudstream/cloudstream") },
            )
            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.35f))
            SettingsNavigationItem(
                label = "Wiki & Extension Docs",
                subtitle = "Read plugin development guides and extension APIs documentation.",
                onClick = { openUrl("https://recloudstream.github.io/csdocs/") },
            )
        }

        SettingsGroupCard(title = "Legal Notice & Disclaimer") {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(vertical = 8.dp, horizontal = 4.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Surface(
                    shape = RoundedCornerShape(8.dp),
                    color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f),
                    border = BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.6f)),
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Column(
                        modifier = Modifier.padding(16.dp),
                        verticalArrangement = Arrangement.spacedBy(10.dp),
                    ) {
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(8.dp),
                        ) {
                            Icon(
                                imageVector = Icons.Default.Gavel,
                                contentDescription = "Legal Notice",
                                tint = MaterialTheme.colorScheme.primary,
                                modifier = Modifier.size(18.dp),
                            )
                            ArabicAwareText(
                                text = "IMPORTANT LEGAL NOTICE — PLEASE READ CAREFULLY",
                                style = MaterialTheme.typography.labelMedium,
                                fontWeight = FontWeight.Bold,
                                color = MaterialTheme.colorScheme.primary,
                                letterSpacing = 0.5.sp,
                            )
                        }

                        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.35f))

                        ArabicAwareText(
                            text = "1. Pure Browser Shell & Zero Bundled Media\n" +
                                "This software is strictly an open-source, media-neutral desktop browser shell and video player. It does NOT host, own, create, index, scrape, transmit, or distribute any media files, video streams, torrents, or content of any kind. Out of the box, this application contains no media catalog, no streaming repositories, and no content.",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            lineHeight = 18.sp,
                        )

                        ArabicAwareText(
                            text = "2. User-Provided Extensions & Third-Party Repositories\n" +
                                "All plugins, extensions, addon manifests, external links, and media sources loaded into this application are configured and added entirely by the user at their own sole discretion. The developers and contributors do not author, maintain, verify, control, or endorse any external repositories, plugins, or third-party streaming sources.",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            lineHeight = 18.sp,
                        )

                        ArabicAwareText(
                            text = "3. Sole User Responsibility & Legal Compliance\n" +
                                "Users are exclusively and solely responsible for their use of this software. You must ensure that your access, playback, or usage of any media or third-party services complies fully with all applicable local, state, national, and international laws, copyright regulations, and intellectual property rights in your jurisdiction.",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            lineHeight = 18.sp,
                        )

                        ArabicAwareText(
                            text = "4. \"As-Is\" Software & Limitation of Liability\n" +
                                "This software is provided \"AS IS\", without warranty of any kind, express or implied. Under no circumstances shall the authors, maintainers, or copyright holders be held liable for any claim, damages, copyright infringement, or legal consequences arising from the use, installation, or misuse of this software.",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            lineHeight = 18.sp,
                        )
                    }
                }
            }
        }
    }
}

private fun openUrl(url: String) {
    appScope.launch(Dispatchers.IO) {
        try {
            val uri = java.net.URI(url)
            val desktop = java.awt.Desktop.getDesktop()
            desktop.browse(uri)
        } catch (e: Exception) {
            com.lagradost.common.logging.AppLogger.e("Error opening link $url", e)
        }
    }
}

@Composable
fun SettingsAboutScreen() {
    val scrollState = rememberScrollState()
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .verticalScroll(scrollState)
            .padding(top = 16.dp, bottom = 40.dp),
        verticalArrangement = Arrangement.spacedBy(20.dp),
    ) {
        SettingsAbout()
    }
}

@Composable
fun SettingsAboutAndUpdates(viewModel: SettingsViewModel) {
    val scrollState = rememberScrollState()
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .verticalScroll(scrollState)
            .padding(top = 16.dp, bottom = 40.dp),
        verticalArrangement = Arrangement.spacedBy(20.dp),
    ) {
        SettingsUpdates(viewModel = viewModel)
        SettingsAbout()
    }
}
