package com.lightink.companion

import android.content.Context
import android.content.SharedPreferences
import android.util.Base64
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/** A change made in the app that the watch hasn't picked up yet. */
data class Pending(val value: Int, val rev: Int)

data class WatchState(
    val token: Long = 0, // Picked by the watch when pairing, 0 = not paired
    val watchAddress: String? = null,
    val settings: List<Protocol.Setting> = emptyList(),
    val schemaHash: Long = 0,
    val wantSchema: Boolean = false,
    val values: Map<Int, Int> = emptyMap(), // As the watch last reported them
    val pending: Map<Int, Pending> = emptyMap(),
    val rev: Int = 0,
    val lastSync: Long = 0, // ms since epoch
    val lastSeen: Long = 0, // The watch connected, whether the sync then worked or not
    val batteryMv: Int = 0,
    val batteryPermille: Int = 0,
    val fastAdvertising: Boolean = true,
    val alwaysReachable: Boolean = false, // Keep the notification and the link up all the time
    val pairingAddress: String? = null, // Watch seen while pairing, its bond may be left over
    // Not saved
    val pairing: Boolean = false,
    val codeRequested: Boolean = false, // Android asked for the pairing code, the app shows a pad
    val connected: Boolean = false,
    val bluetoothProblem: String? = null,
) {
    val paired get() = token != 0L

    /** What the app shows: the pending value if there is one. */
    fun value(id: Int): Int? = pending[id]?.value ?: values[id]

    fun advertisementFlags(): Int {
        if (pairing) return Protocol.FLAG_PAIRING
        var flags = 0
        if (pending.isNotEmpty()) flags = flags or Protocol.FLAG_PENDING
        if (values.isEmpty()) flags = flags or Protocol.FLAG_WANT_STATE
        if (settings.isEmpty() || wantSchema) flags = flags or Protocol.FLAG_WANT_SCHEMA
        return flags
    }

    /** The service runs (and the phone advertises) only while there is a watch to talk to. */
    val needsService get() = paired || pairing
}

/**
 * Everything the app knows about the watch, saved across restarts. The UI and the
 * Bluetooth service share it through [state].
 */
object WatchStore {
    private lateinit var prefs: SharedPreferences
    private val _state = MutableStateFlow(WatchState())
    val state: StateFlow<WatchState> = _state.asStateFlow()

    // A schema written in the current connection: the STATE after it carries its hash
    private var schemaFresh = false

    @Synchronized
    fun init(context: Context) {
        if (::prefs.isInitialized) return
        prefs = context.applicationContext.getSharedPreferences("watch", Context.MODE_PRIVATE)
        _state.value = load()
    }

    @Synchronized
    private fun update(save: Boolean = true, transform: (WatchState) -> WatchState) {
        val next = transform(_state.value)
        _state.value = next
        if (save) save(next)
    }

    // --- From the UI ---------------------------------------------------------------

    fun setValue(setting: Protocol.Setting, value: Int) = update { s ->
        val v = setting.clamp(value)
        var rev = s.rev
        if (rev >= 0xFFF0 && s.pending.isEmpty()) rev = 0 // The wire counter is 16 bits
        val pending = if (s.values[setting.id] == v) {
            s.pending - setting.id // Back to what the watch has
        } else {
            rev += 1
            s.pending + (setting.id to Pending(v, rev))
        }
        s.copy(pending = pending, rev = rev)
    }

    fun discardPending() = update { it.copy(pending = emptyMap()) }

    fun startPairing() = update { it.copy(pairing = true, pairingAddress = null) }

    fun cancelPairing() = update(save = false) { it.copy(pairing = false, codeRequested = false) }

    fun forget() = update { s ->
        WatchState(fastAdvertising = s.fastAdvertising, alwaysReachable = s.alwaysReachable)
    }

    fun setFastAdvertising(fast: Boolean) = update { it.copy(fastAdvertising = fast) }

    fun setAlwaysReachable(on: Boolean) = update { it.copy(alwaysReachable = on) }

    // --- From the Bluetooth service ------------------------------------------------

    fun setConnected(connected: Boolean) = update(save = false) { it.copy(connected = connected) }

    /** The paired watch connected. A sync usually follows, but it can also fail. */
    fun seen() = update { it.copy(lastSeen = System.currentTimeMillis()) }

    fun setCodeRequested(requested: Boolean) =
        update(save = false) { it.copy(codeRequested = requested) }

    fun setPairingAddress(address: String) = update { it.copy(pairingAddress = address) }

    fun setBluetoothProblem(problem: String?) =
        update(save = false) { it.copy(bluetoothProblem = problem) }

    /** Value of the PENDING characteristic. */
    fun pendingPayload(): ByteArray {
        val s = _state.value
        return Protocol.pending(s.rev and 0xFFFF, s.pending.mapValues { it.value.value })
    }

    fun onSchema(data: ByteArray): Boolean {
        val settings = runCatching { Protocol.parseSchema(data) }.getOrNull() ?: return false
        schemaFresh = true
        update { it.copy(settings = settings, wantSchema = false) }
        prefs.edit().putString(KEY_SCHEMA, Base64.encodeToString(data, Base64.NO_WRAP)).apply()
        return true
    }

    /** The watch's values. Returns false when it isn't the paired watch. */
    fun onState(data: ByteArray, address: String): Boolean {
        val st = runCatching { Protocol.parseState(data) }.getOrNull() ?: return false
        var accepted = false
        val now = System.currentTimeMillis()
        update { s ->
            val justPaired = s.pairing && st.flags and Protocol.STATE_JUST_PAIRED != 0
            if (!justPaired && (!s.paired || st.token != s.token)) return@update s
            accepted = true
            // Served changes the watch confirms. Later edits keep waiting
            val pending = if (justPaired) emptyMap() else s.pending.filterValues { it.rev > st.ackRev }
            val (hash, want) = when {
                schemaFresh -> st.schemaHash to false
                st.schemaHash != s.schemaHash -> s.schemaHash to true // Firmware changed
                else -> s.schemaHash to false
            }
            schemaFresh = false
            s.copy(
                token = st.token,
                watchAddress = if (justPaired) address else s.watchAddress,
                values = st.values,
                pending = pending,
                schemaHash = hash,
                wantSchema = want,
                lastSync = now,
                lastSeen = now,
                batteryMv = st.batteryMv,
                batteryPermille = st.batteryPermille,
                pairing = false,
                codeRequested = false,
            )
        }
        return accepted
    }

    // --- Persistence -------------------------------------------------------------------

    private const val KEY_SCHEMA = "schema"

    private fun load(): WatchState {
        val schema = prefs.getString(KEY_SCHEMA, null)?.let {
            runCatching { Protocol.parseSchema(Base64.decode(it, Base64.NO_WRAP)) }.getOrNull()
        }
        return WatchState(
            token = prefs.getLong("token", 0),
            watchAddress = prefs.getString("address", null),
            settings = schema ?: emptyList(),
            schemaHash = prefs.getLong("schemaHash", 0),
            wantSchema = prefs.getBoolean("wantSchema", false),
            values = decodeMap(prefs.getString("values", "")!!).mapValues { it.value.first },
            pending = decodeMap(prefs.getString("pending", "")!!)
                .mapValues { Pending(it.value.first, it.value.second) },
            rev = prefs.getInt("rev", 0),
            lastSync = prefs.getLong("lastSync", 0),
            // Saved by an app from before it was kept: the last sync is the best guess
            lastSeen = maxOf(prefs.getLong("lastSeen", 0), prefs.getLong("lastSync", 0)),
            batteryMv = prefs.getInt("batteryMv", 0),
            batteryPermille = prefs.getInt("batteryPermille", 0),
            fastAdvertising = prefs.getBoolean("fast", true),
            alwaysReachable = prefs.getBoolean("alwaysReachable", false),
            pairingAddress = prefs.getString("pairingAddress", null),
        )
    }

    private fun save(s: WatchState) {
        val edit = prefs.edit()
            .putLong("token", s.token)
            .putString("address", s.watchAddress)
            .putLong("schemaHash", s.schemaHash)
            .putBoolean("wantSchema", s.wantSchema)
            .putString("values", s.values.entries.joinToString(",") { "${it.key}:${it.value}" })
            .putString("pending", s.pending.entries.joinToString(",") {
                "${it.key}:${it.value.value}:${it.value.rev}"
            })
            .putInt("rev", s.rev)
            .putLong("lastSync", s.lastSync)
            .putLong("lastSeen", s.lastSeen)
            .putInt("batteryMv", s.batteryMv)
            .putInt("batteryPermille", s.batteryPermille)
            .putBoolean("fast", s.fastAdvertising)
            .putBoolean("alwaysReachable", s.alwaysReachable)
            .putString("pairingAddress", s.pairingAddress)
        if (s.settings.isEmpty()) edit.remove(KEY_SCHEMA)
        edit.apply()
    }

    /** "id:a[:b],..." */
    private fun decodeMap(text: String): Map<Int, Pair<Int, Int>> =
        text.split(',').filter { it.isNotBlank() }.mapNotNull { item ->
            val p = item.split(':').mapNotNull { it.toIntOrNull() }
            if (p.size >= 2) p[0] to (p[1] to p.getOrElse(2) { 0 }) else null
        }.toMap()
}
