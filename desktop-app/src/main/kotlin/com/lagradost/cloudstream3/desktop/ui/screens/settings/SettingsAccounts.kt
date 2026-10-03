package com.lagradost.cloudstream3.desktop.ui.screens.settings

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.background
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.Info
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.lagradost.cloudstream3.desktop.ui.components.CloudstreamAlertDialog
import com.lagradost.cloudstream3.desktop.ui.components.CloudstreamCustomDialog
import com.lagradost.cloudstream3.desktop.ui.screens.settings.contract.SettingsUiEvent
import com.lagradost.cloudstream3.syncproviders.AccountManager
import com.lagradost.cloudstream3.syncproviders.AuthAPI
import com.lagradost.cloudstream3.syncproviders.AuthData
import com.lagradost.common.storage.DesktopDataStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

@Composable
fun SettingsAccounts(viewModel: SettingsViewModel) {
    val uiState by viewModel.uiState.collectAsState()
    val scope = rememberCoroutineScope()
    var selectedApiForLogin by remember { mutableStateOf<AuthAPI?>(null) }
    val cachedAccounts by AccountManager.accountsFlow.collectAsState()

    val scrollState = rememberScrollState()
    var containerCoordinates by remember { mutableStateOf<androidx.compose.ui.layout.LayoutCoordinates?>(null) }

    CompositionLocalProvider(
        LocalSettingsScrollState provides scrollState,
        LocalScrollContainerCoordinates provides containerCoordinates,
    ) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .onGloballyPositioned { containerCoordinates = it }
                .verticalScroll(scrollState)
                .padding(bottom = 32.dp),
            verticalArrangement = Arrangement.spacedBy(24.dp),
        ) {
            val profiles by com.lagradost.cloudstream3.desktop.profile.ProfileManager.profiles.collectAsState()
            val activeProfile by com.lagradost.cloudstream3.desktop.profile.ProfileManager.activeProfile.collectAsState()
            val isPickerOnStartup by com.lagradost.cloudstream3.desktop.profile.ProfileManager.isPickerOnStartup.collectAsState()
            var editingProfile by remember { mutableStateOf<com.lagradost.cloudstream3.desktop.profile.Profile?>(null) }
            var isCreatingNew by remember { mutableStateOf(false) }

            if (editingProfile != null || isCreatingNew) {
                com.lagradost.cloudstream3.desktop.ui.screens.profile.ProfileEditDialog(
                    profile = editingProfile,
                    canDelete = profiles.size > 1,
                    onDismiss = {
                        editingProfile = null
                        isCreatingNew = false
                    },
                    onSave = { name, colorIndex, customAvatar, pin, isKids ->
                        scope.launch(Dispatchers.IO) {
                            if (isCreatingNew) {
                                com.lagradost.cloudstream3.desktop.profile.ProfileManager.createProfile(name, colorIndex, customAvatar, pin, isKids)
                            } else if (editingProfile != null) {
                                com.lagradost.cloudstream3.desktop.profile.ProfileManager.updateProfile(
                                    editingProfile!!.copy(
                                        name = name,
                                        avatarColorIndex = colorIndex,
                                        customAvatarPath = customAvatar,
                                        pinCode = pin,
                                        isKids = isKids,
                                    ),
                                )
                            }
                        }
                    },
                    onDelete = {
                        scope.launch(Dispatchers.IO) {
                            editingProfile?.let { com.lagradost.cloudstream3.desktop.profile.ProfileManager.deleteProfile(it.id) }
                        }
                    },
                )
            }

            SettingsGroupCard(title = "User Profiles") {
                Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    // Profile List Items
                    profiles.forEach { profile ->
                        Row(
                            modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.SpaceBetween,
                        ) {
                            Row(
                                verticalAlignment = Alignment.CenterVertically,
                                horizontalArrangement = Arrangement.spacedBy(12.dp),
                                modifier = Modifier.weight(1f),
                            ) {
                                com.lagradost.cloudstream3.desktop.profile.ProfileAvatar(
                                    profile = profile,
                                    size = 36.dp,
                                    shape = RoundedCornerShape(8.dp),
                                    fontSize = 16.sp,
                                )

                                Column {
                                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                                        Text(
                                            text = profile.name,
                                            style = MaterialTheme.typography.titleMedium,
                                            fontWeight = if (profile.id == activeProfile.id) FontWeight.Bold else FontWeight.Medium,
                                            color = Color.White,
                                        )
                                        if (profile.id == activeProfile.id) {
                                            Surface(
                                                shape = androidx.compose.foundation.shape.RoundedCornerShape(4.dp),
                                                color = MaterialTheme.colorScheme.primary.copy(alpha = 0.2f),
                                            ) {
                                                Text(
                                                    "Active",
                                                    style = MaterialTheme.typography.labelSmall,
                                                    color = MaterialTheme.colorScheme.primary,
                                                    modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
                                                )
                                            }
                                        }
                                    }
                                    val details = buildList {
                                        if (profile.isKids) add("Kids Profile")
                                        if (profile.hasPin) add("PIN Protected")
                                    }.joinToString(" • ")
                                    if (details.isNotEmpty()) {
                                        Text(details, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                                    }
                                }
                            }

                            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                if (profile.id != activeProfile.id) {
                                    OutlinedButton(
                                        onClick = {
                                            scope.launch(Dispatchers.IO) {
                                                com.lagradost.cloudstream3.desktop.profile.ProfileManager.switchProfile(profile.id)
                                            }
                                        },
                                        contentPadding = PaddingValues(horizontal = 12.dp, vertical = 0.dp),
                                        modifier = Modifier.height(32.dp),
                                    ) {
                                        Text("Switch", style = MaterialTheme.typography.labelMedium)
                                    }
                                }
                                IconButton(
                                    onClick = { editingProfile = profile },
                                    modifier = Modifier.size(32.dp),
                                ) {
                                    Icon(androidx.compose.material.icons.Icons.Default.Edit, contentDescription = "Edit Profile", modifier = Modifier.size(16.dp))
                                }
                            }
                        }
                    }

                    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.4f))

                    // Actions & Preferences
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.SpaceBetween,
                    ) {
                        Button(onClick = { isCreatingNew = true }) {
                            Text("+ Create New Profile")
                        }

                        val autoSignIn by com.lagradost.cloudstream3.desktop.profile.ProfileManager.autoSignIn.collectAsState()
                        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            Text("Auto sign-in on launch", style = MaterialTheme.typography.bodyMedium)
                            Switch(
                                checked = autoSignIn,
                                onCheckedChange = { checked ->
                                    scope.launch(Dispatchers.IO) {
                                        com.lagradost.cloudstream3.desktop.profile.ProfileManager.setAutoSignIn(checked)
                                    }
                                },
                            )
                        }
                    }
                }
            }

            SettingsGroupCard(title = "Subtitle Providers") {
                Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    AccountManager.subtitleProviders.forEachIndexed { index, provider ->
                        val accounts = cachedAccounts[provider.idPrefix]
                        val isLoggedIn = !accounts.isNullOrEmpty() && accounts.firstOrNull()?.token?.accessToken?.isNotBlank() == true

                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(vertical = 4.dp),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.SpaceBetween,
                        ) {
                            Column(modifier = Modifier.weight(1f)) {
                                Text(
                                    text = provider.name,
                                    style = MaterialTheme.typography.titleMedium,
                                    fontWeight = FontWeight.SemiBold,
                                    color = MaterialTheme.colorScheme.onSurface,
                                )
                                Text(
                                    text = when {
                                        !provider.hasInApp -> "Active (Built-in integration)"
                                        isLoggedIn -> "API Key configured"
                                        else -> "API Key required for external subtitle search"
                                    },
                                    style = MaterialTheme.typography.bodySmall,
                                    color = if (isLoggedIn) MaterialTheme.colorScheme.primary else Color.Gray,
                                )
                            }

                            if (provider.hasInApp) {
                                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                    if (isLoggedIn) {
                                        OutlinedButton(
                                            onClick = {
                                                scope.launch(Dispatchers.IO) {
                                                    AccountManager.updateAccounts(provider.idPrefix, emptyArray())
                                                    if (provider.idPrefix == "subsource") {
                                                        DesktopDataStore.removeKey(com.lagradost.cloudstream3.desktop.player.PlayerConfig.PREF_SUBSOURCE_API_KEY)
                                                    }
                                                }
                                            },
                                            colors = ButtonDefaults.outlinedButtonColors(
                                                contentColor = MaterialTheme.colorScheme.error,
                                            ),
                                        ) {
                                            Text("Remove")
                                        }
                                        Button(
                                            onClick = { selectedApiForLogin = provider },
                                        ) {
                                            Text("Edit Key")
                                        }
                                    } else {
                                        Button(
                                            onClick = { selectedApiForLogin = provider },
                                        ) {
                                            Text("Add API Key")
                                        }
                                    }
                                }
                            }
                        }

                        if (index < AccountManager.subtitleProviders.size - 1) {
                            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.3f))
                        }
                    }
                }
            }

            SettingsGroupCard(title = "Trackers & Integrations") {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Column(
                        modifier = Modifier.fillMaxWidth().padding(vertical = 24.dp),
                        horizontalAlignment = Alignment.CenterHorizontally,
                        verticalArrangement = Arrangement.spacedBy(16.dp),
                    ) {
                        Icon(
                            imageVector = Icons.Default.Info,
                            contentDescription = "Info",
                            modifier = Modifier.size(48.dp),
                            tint = MaterialTheme.colorScheme.primary.copy(alpha = 0.7f),
                        )
                        Text(
                            text = "Trackers Are Not Supported Yet",
                            style = MaterialTheme.typography.titleLarge,
                            fontWeight = FontWeight.Bold,
                            color = Color.White,
                        )
                        Text(
                            text = "External tracker and sync logins (MAL, AniList, Simkl) are currently disabled for the Desktop Client.",
                            style = MaterialTheme.typography.bodyMedium,
                            color = Color.Gray,
                            textAlign = TextAlign.Center,
                            modifier = Modifier.padding(horizontal = 32.dp),
                        )
                    }
                }
            }

            SettingsGroupCard(title = "Discord Rich Presence") {
                MviSettingsToggle(
                    key = DesktopDataStore.PREF_DISCORD_RPC_ENABLED,
                    label = "Enable Discord Rich Presence",
                    subtitle = "Display your current playback and browsing status on your Discord profile.",
                    uiState = uiState,
                    onEvent = viewModel::onEvent,
                    defaultValue = false,
                )

                val discordRpcEnabled = uiState.booleanSettings[DesktopDataStore.PREF_DISCORD_RPC_ENABLED] ?: (DesktopDataStore.getKey<Boolean>(DesktopDataStore.PREF_DISCORD_RPC_ENABLED) ?: false)

                if (discordRpcEnabled) {
                    MviSettingsToggle(
                        key = DesktopDataStore.PREF_DISCORD_RPC_SHOW_TITLE,
                        label = "Show Media & Episode Titles",
                        subtitle = "Display the specific movie, series name, and episode number.",
                        uiState = uiState,
                        onEvent = viewModel::onEvent,
                        defaultValue = true,
                    )

                    MviSettingsToggle(
                        key = DesktopDataStore.PREF_DISCORD_RPC_SHOW_PROGRESS,
                        label = "Show Playback Progress Bar",
                        subtitle = "Display a live countdown progress bar on Discord while playing video.",
                        uiState = uiState,
                        onEvent = viewModel::onEvent,
                        defaultValue = true,
                    )

                    MviSettingsToggle(
                        key = DesktopDataStore.PREF_DISCORD_RPC_SHOW_BROWSING,
                        label = "Show Browsing Activity",
                        subtitle = "Display when browsing menus and catalogs when video is not playing.",
                        uiState = uiState,
                        onEvent = viewModel::onEvent,
                        defaultValue = true,
                    )
                }
            }
        }
    }

    selectedApiForLogin?.let { targetApi ->
        InAppLoginDialog(
            api = targetApi,
            onDismiss = { selectedApiForLogin = null },
            onSuccess = { authData ->
                val apiPrefix = targetApi.idPrefix
                scope.launch(Dispatchers.IO) {
                    AccountManager.updateAccounts(apiPrefix, arrayOf(authData))
                    if (apiPrefix == "subsource") {
                        authData.token.accessToken?.let {
                            DesktopDataStore.setKey(com.lagradost.cloudstream3.desktop.player.PlayerConfig.PREF_SUBSOURCE_API_KEY, it)
                        }
                    }
                }
                selectedApiForLogin = null
            },
        )
    }
}

@Composable
fun InAppLoginDialog(api: AuthAPI, onDismiss: () -> Unit, onSuccess: (AuthData) -> Unit) {
    val scope = rememberCoroutineScope()
    var username by remember { mutableStateOf("") }
    var password by remember { mutableStateOf("") }
    var email by remember { mutableStateOf("") }
    var server by remember { mutableStateOf("") }
    val initialKey = AccountManager.cachedAccounts[api.idPrefix]?.firstOrNull()?.token?.accessToken ?: ""
    var apiKeyStr by remember { mutableStateOf(initialKey) }
    var errorMsg by remember { mutableStateOf<String?>(null) }
    var isTestingApi by remember { mutableStateOf(false) }
    var testApiStatus by remember { mutableStateOf<Pair<Boolean, String>?>(null) }

    val req = api.inAppLoginRequirement
    val isApiKeyOnly = req != null && req.apiKey && !req.username && !req.password && !req.email && !req.server

    CloudstreamCustomDialog(
        show = true,
        onDismissRequest = onDismiss,
    ) {
        Column(modifier = Modifier.padding(24.dp).width(440.dp)) {
            Text(if (isApiKeyOnly) "Enter API Key for ${api.name}" else "Login to ${api.name}", style = MaterialTheme.typography.titleLarge, color = MaterialTheme.colorScheme.onSurface, fontWeight = FontWeight.Bold)
            Spacer(modifier = Modifier.height(24.dp))

            if (req?.username == true) {
                OutlinedTextField(
                    value = username,
                    onValueChange = { username = it },
                    label = { Text("Username") },
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true,
                )
                Spacer(modifier = Modifier.height(16.dp))
            }

            if (req?.email == true) {
                OutlinedTextField(
                    value = email,
                    onValueChange = { email = it },
                    label = { Text("Email") },
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true,
                )
                Spacer(modifier = Modifier.height(16.dp))
            }

            if (req?.password == true) {
                OutlinedTextField(
                    value = password,
                    onValueChange = { password = it },
                    label = { Text("Password") },
                    visualTransformation = PasswordVisualTransformation(),
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true,
                )
                Spacer(modifier = Modifier.height(16.dp))
            }

            if (req?.server == true) {
                OutlinedTextField(
                    value = server,
                    onValueChange = { server = it },
                    label = { Text("Server") },
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true,
                )
                Spacer(modifier = Modifier.height(16.dp))
            }

            if (req?.apiKey == true) {
                OutlinedTextField(
                    value = apiKeyStr,
                    onValueChange = {
                        apiKeyStr = it
                        if (errorMsg != null) errorMsg = null
                        testApiStatus = null
                    },
                    label = { Text("API Key") },
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true,
                    visualTransformation = PasswordVisualTransformation(),
                )
                Spacer(modifier = Modifier.height(16.dp))
            }

            if (testApiStatus != null) {
                val (isSuccess, msg) = testApiStatus!!
                Text(
                    text = if (isSuccess) "✓ $msg" else "✕ $msg",
                    color = if (isSuccess) Color(0xFF4CAF50) else MaterialTheme.colorScheme.error,
                    style = MaterialTheme.typography.bodySmall,
                    fontWeight = FontWeight.Medium,
                )
                Spacer(modifier = Modifier.height(14.dp))
            } else if (errorMsg != null) {
                Text(errorMsg!!, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
                Spacer(modifier = Modifier.height(16.dp))
            }

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = if (isApiKeyOnly) Arrangement.SpaceBetween else Arrangement.End,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                if (isApiKeyOnly) {
                    OutlinedButton(
                        onClick = {
                            val cleanKey = apiKeyStr.trim()
                            if (cleanKey.isBlank()) {
                                testApiStatus = false to "API Key cannot be empty"
                                return@OutlinedButton
                            }
                            isTestingApi = true
                            testApiStatus = null
                            scope.launch(Dispatchers.IO) {
                                val res = runCatching {
                                    if (api.idPrefix == "subdl") {
                                        val r = com.lagradost.cloudstream3.app.get("https://api.subdl.com/api/v1/subtitles?api_key=$cleanKey&film_name=matrix", timeout = 8000L).okhttpResponse
                                        when (r.code) {
                                            200 -> true to "Valid API Key (Connected successfully)"
                                            400, 401, 403 -> false to "Invalid API Key (HTTP ${r.code})"
                                            else -> false to "HTTP ${r.code}: Unexpected response"
                                        }
                                    } else if (api.idPrefix == "subsource") {
                                        com.lagradost.cloudstream3.desktop.subtitles.SubsourceSubtitleProvider.testApiKey(cleanKey)
                                    } else {
                                        true to "API Key format verified"
                                    }
                                }.getOrElse { false to "Connection error: ${it.message}" }
                                testApiStatus = res
                                isTestingApi = false
                            }
                        },
                        enabled = !isTestingApi && apiKeyStr.isNotBlank(),
                    ) {
                        if (isTestingApi) {
                            CircularProgressIndicator(modifier = Modifier.size(16.dp), strokeWidth = 2.dp)
                            Spacer(modifier = Modifier.width(6.dp))
                            Text("Testing...")
                        } else {
                            Text("Test API Key")
                        }
                    }
                }

                Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                    TextButton(onClick = onDismiss) { Text("Cancel") }
                    Button(onClick = {
                        val cleanKey = apiKeyStr.trim()
                        if (isApiKeyOnly && cleanKey.isBlank()) {
                            errorMsg = "API Key cannot be empty"
                            return@Button
                        }
                        val authData = AuthData(
                            user = com.lagradost.cloudstream3.syncproviders.AuthUser(name = if (username.isNotBlank()) username.trim() else "User", id = 0, profilePicture = ""),
                            token = com.lagradost.cloudstream3.syncproviders.AuthToken(accessToken = cleanKey),
                        )
                        onSuccess(authData)
                    }) { Text(if (isApiKeyOnly) "Save Key" else "Login") }
                }
            }
        }
    }
}
