package com.lightink.companion.ui

import android.content.Intent
import android.provider.Settings
import android.text.format.DateUtils
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.material3.ScrollableTabRow
import androidx.compose.material3.Tab
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.unit.Dp
import kotlinx.coroutines.launch
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.lightink.companion.Protocol
import com.lightink.companion.WatchState
import com.lightink.companion.WatchStore
import kotlinx.coroutines.delay
import java.util.TimeZone

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HomeScreen(
    permissionsGranted: Boolean,
    onGrantPermissions: () -> Unit,
    onEnableBluetooth: () -> Unit,
) {
    val state by WatchStore.state.collectAsState()
    Scaffold(topBar = { TopAppBar(title = { Text("LightInk") }) }) { padding ->
        Column(Modifier.padding(top = padding.calculateTopPadding())) {
            val notice = when {
                !permissionsGranted -> "The app needs the Bluetooth permission to let the watch find the phone."
                state.needsService -> state.bluetoothProblem
                else -> null
            }
            if (notice != null) {
                Box(Modifier.padding(horizontal = 16.dp, vertical = 8.dp)) {
                    when {
                        !permissionsGranted -> Notice(notice, "Allow", onGrantPermissions)
                        notice == "Bluetooth is off" ->
                            Notice("Bluetooth is off, the watch can't sync.", "Turn on", onEnableBluetooth)
                        else -> Notice(notice)
                    }
                }
            }
            val bottom = padding.calculateBottomPadding() + 24.dp
            if (state.paired) {
                Tabs(state, permissionsGranted, bottom)
            } else {
                Page(bottom) { item { WatchCard(state, enabled = permissionsGranted) } }
            }
        }
    }
}

/** "Watch" (status and app options), then one tab per settings group the watch sent. */
@Composable
private fun Tabs(state: WatchState, permissionsGranted: Boolean, bottom: Dp) {
    val groups = state.settings.groupBy { it.group }
    val titles = listOf("Watch") + groups.keys
    val pager = rememberPagerState { titles.size }
    val scope = rememberCoroutineScope()
    ScrollableTabRow(selectedTabIndex = pager.currentPage.coerceAtMost(titles.lastIndex), edgePadding = 8.dp) {
        titles.forEachIndexed { i, title ->
            // A dot on the groups with changes still waiting for the watch
            val waiting = groups[title]?.any { it.id in state.pending } == true
            Tab(
                selected = pager.currentPage == i,
                onClick = { scope.launch { pager.animateScrollToPage(i) } },
                text = { Text(if (waiting) "$title •" else title) },
            )
        }
    }
    HorizontalPager(pager, Modifier.fillMaxSize(), verticalAlignment = Alignment.Top) { page ->
        val group = titles.getOrNull(page)
        Page(bottom) {
            if (page == 0) {
                item { WatchCard(state, enabled = permissionsGranted) }
                if (state.settings.isEmpty()) {
                    item {
                        Text(
                            "The watch sends its settings at its next sync.",
                            style = MaterialTheme.typography.bodyMedium,
                            modifier = Modifier.padding(horizontal = 4.dp),
                        )
                    }
                }
                item { SectionTitle("App") }
                item { AppCard(state) }
            } else {
                groups[group]?.let { settings -> item { SettingsCard(state, settings) } }
            }
        }
    }
}

@Composable
private fun Page(bottom: Dp, content: androidx.compose.foundation.lazy.LazyListScope.() -> Unit) {
    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 12.dp, bottom = bottom),
        verticalArrangement = Arrangement.spacedBy(12.dp),
        content = content,
    )
}

@Composable
private fun SectionTitle(text: String) {
    Text(
        text,
        style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(start = 4.dp, top = 8.dp),
    )
}

@Composable
private fun Notice(text: String, action: String? = null, onAction: () -> Unit = {}) {
    Card(
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Row(Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(text, Modifier.weight(1f), color = MaterialTheme.colorScheme.onErrorContainer)
            if (action != null) {
                Spacer(Modifier.width(8.dp))
                Button(onClick = onAction) { Text(action) }
            }
        }
    }
}

// --- Watch status and pairing ------------------------------------------------------------

@Composable
private fun WatchCard(state: WatchState, enabled: Boolean) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            when {
                state.pairing -> Pairing()
                state.paired -> Paired(state)
                else -> NotPaired(enabled)
            }
        }
    }
}

@Composable
private fun NotPaired(enabled: Boolean) {
    Text("No watch paired", style = MaterialTheme.typography.titleLarge)
    Text(
        "Pair once and the watch keeps its settings in sync with this phone. It checks in " +
            "for a moment while its screen refreshes, so the app only has to stay installed.",
        style = MaterialTheme.typography.bodyMedium,
    )
    Button(onClick = WatchStore::startPairing, enabled = enabled) { Text("Pair watch") }
}

@Composable
private fun Pairing() {
    Text("Pairing", style = MaterialTheme.typography.titleLarge)
    LinearProgressIndicator(Modifier.fillMaxWidth())
    Step(1, "On the watch, open Bluetooth › Pair phone.")
    Step(2, "The watch shows a 6 digit code and Android asks for it. " +
        "If no dialog appears, look for a pairing notification.")
    Step(3, "Type the code here. The watch says \"Paired!\" when done.")
    Text(
        "Pairing again after a failed try? Android refuses a watch it still remembers: " +
            "remove it from Bluetooth settings first (it may show as its address).",
        style = MaterialTheme.typography.bodySmall,
    )
    val context = LocalContext.current
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        OutlinedButton(onClick = WatchStore::cancelPairing) { Text("Cancel") }
        TextButton(onClick = { context.startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS)) }) {
            Text("Bluetooth settings")
        }
    }
}

@Composable
private fun Step(n: Int, text: String) {
    Row {
        Text("$n.", Modifier.width(24.dp), fontWeight = FontWeight.Bold)
        Text(text, style = MaterialTheme.typography.bodyMedium)
    }
}

@Composable
private fun Paired(state: WatchState) {
    val context = LocalContext.current
    var confirmForget by remember { mutableStateOf(false) }
    // Keep "5 min ago" moving
    var now by remember { mutableLongStateOf(System.currentTimeMillis()) }
    LaunchedEffect(Unit) {
        while (true) {
            delay(30_000)
            now = System.currentTimeMillis()
        }
    }

    Row(verticalAlignment = Alignment.CenterVertically) {
        Text("Watch paired", style = MaterialTheme.typography.titleLarge, modifier = Modifier.weight(1f))
        if (state.connected) Text("Syncing…", color = MaterialTheme.colorScheme.primary)
    }
    val lastSync = if (state.lastSync == 0L) "not yet"
    else DateUtils.getRelativeTimeSpanString(state.lastSync, now, DateUtils.MINUTE_IN_MILLIS).toString()
    Text("Last sync: $lastSync", style = MaterialTheme.typography.bodyMedium)
    if (state.batteryMv > 0) {
        Text(
            "Battery: %d %% (%.2f V)".format(state.batteryPermille / 10, state.batteryMv / 1000f),
            style = MaterialTheme.typography.bodyMedium,
        )
    }
    if (state.pending.isNotEmpty()) {
        HorizontalDivider()
        Text(
            "${state.pending.size} change(s) waiting for the watch. They apply at its next " +
                "sync, or right away with Bluetooth › Sync now on the watch.",
            style = MaterialTheme.typography.bodyMedium,
        )
        TextButton(onClick = WatchStore::discardPending) { Text("Discard changes") }
    }
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        FilledTonalButton(onClick = WatchStore::startPairing) { Text("Pair again") }
        OutlinedButton(onClick = { confirmForget = true }) { Text("Forget watch") }
    }

    if (confirmForget) {
        AlertDialog(
            onDismissRequest = { confirmForget = false },
            title = { Text("Forget the watch?") },
            text = {
                Text(
                    "The phone stops answering the watch. Also use Bluetooth › Forget phone on " +
                        "the watch, and remove the watch from Android's Bluetooth settings " +
                        "(it may show as its address)."
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    confirmForget = false
                    WatchStore.forget()
                    context.startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS))
                }) { Text("Forget") }
            },
            dismissButton = { TextButton(onClick = { confirmForget = false }) { Text("Cancel") } },
        )
    }
}

// --- Settings, built from the description the watch sent ---------------------------------

@Composable
private fun SettingsCard(state: WatchState, settings: List<Protocol.Setting>) {
    Card(Modifier.fillMaxWidth()) {
        settings.forEachIndexed { i, setting ->
            if (i > 0) HorizontalDivider(Modifier.padding(horizontal = 16.dp))
            SettingRow(setting, state.value(setting.id), pending = setting.id in state.pending)
        }
    }
}

@Composable
private fun SettingRow(setting: Protocol.Setting, value: Int?, pending: Boolean) {
    val v = value ?: setting.min
    val set = { n: Int -> WatchStore.setValue(setting, n) }
    val supporting: (@Composable () -> Unit)? = when {
        setting.kind == Protocol.Kind.TZ_MINUTES -> {
            { TimeZoneHint(v, pending, onUsePhone = set) }
        }
        pending -> {
            { Text("Waiting for the watch", color = MaterialTheme.colorScheme.primary) }
        }
        else -> null
    }
    ListItem(
        headlineContent = { Text(setting.name) },
        supportingContent = supporting,
        colors = ListItemDefaults.colors(containerColor = CardDefaults.cardColors().containerColor),
        trailingContent = {
            when (setting.kind) {
                Protocol.Kind.BOOL -> Switch(checked = v != 0, onCheckedChange = { set(if (it) 1 else 0) })
                Protocol.Kind.ENUM -> EnumPicker(setting, v, set)
                Protocol.Kind.INT -> Stepper(v.toString(), { set(v - 1) }, { set(v + 1) })
                Protocol.Kind.TZ_MINUTES -> Stepper(utcOffset(v), { set(v - 15) }, { set(v + 15) })
            }
        },
    )
}

@Composable
private fun TimeZoneHint(value: Int, pending: Boolean, onUsePhone: (Int) -> Unit) {
    val phone = TimeZone.getDefault().getOffset(System.currentTimeMillis()) / 60_000
    Column {
        if (pending) Text("Waiting for the watch", color = MaterialTheme.colorScheme.primary)
        if (phone != value) {
            TextButton(onClick = { onUsePhone(phone) }, contentPadding = PaddingValues(0.dp)) {
                Text("Use the phone's (${utcOffset(phone)})")
            }
        }
    }
}

private fun utcOffset(minutes: Int): String {
    val sign = if (minutes < 0) "−" else "+"
    val m = kotlin.math.abs(minutes)
    return "UTC$sign%d:%02d".format(m / 60, m % 60)
}

@Composable
private fun Stepper(text: String, onDown: () -> Unit, onUp: () -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        OutlinedButton(onClick = onDown, contentPadding = PaddingValues(0.dp), modifier = Modifier.width(40.dp)) {
            Text("−")
        }
        Text(text, Modifier.widthIn(min = 72.dp), textAlign = TextAlign.Center)
        OutlinedButton(onClick = onUp, contentPadding = PaddingValues(0.dp), modifier = Modifier.width(40.dp)) {
            Text("+")
        }
    }
}

@Composable
private fun EnumPicker(setting: Protocol.Setting, value: Int, onPick: (Int) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box {
        TextButton(onClick = { open = true }) {
            Text(setting.options.getOrElse(value) { value.toString() } + "  ▾")
        }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            setting.options.forEachIndexed { i, label ->
                DropdownMenuItem(text = { Text(label) }, onClick = {
                    open = false
                    onPick(i)
                })
            }
        }
    }
}

// --- App options -----------------------------------------------------------------------

@Composable
private fun AppCard(state: WatchState) {
    Card(Modifier.fillMaxWidth()) {
        ListItem(
            headlineContent = { Text("Fast advertising") },
            supportingContent = {
                Text(
                    "Every 100 ms instead of 250 ms. The watch only listens for about 200 ms, " +
                        "so turning this off saves phone battery but syncs get missed."
                )
            },
            colors = ListItemDefaults.colors(containerColor = CardDefaults.cardColors().containerColor),
            trailingContent = {
                Switch(checked = state.fastAdvertising, onCheckedChange = WatchStore::setFastAdvertising)
            },
        )
        Spacer(Modifier.height(4.dp))
    }
}
