package com.lightink.companion

import java.util.UUID

/**
 * Wire format shared with the watch firmware (firmware/main/ble.cpp and
 * remote_settings.cpp). Described in docs/ble_protocol.md: change both sides together.
 *
 * The watch is the BLE central. The phone advertises [advertisement] all the time and
 * hosts a GATT service the watch connects to for a few hundred ms when there is
 * something to exchange. All multi-byte numbers are little endian.
 */
object Protocol {
    const val VERSION = 1
    const val COMPANY_ID = 0xFFFF // Bluetooth SIG "no company", fine for a private protocol

    // 4c494748-5449-4e4b-0000-0000000000xx: "LIGHTINK"
    private fun uuid(n: Int) = UUID.fromString("4c494748-5449-4e4b-0000-%012x".format(n))
    val SERVICE: UUID = uuid(0)
    val STATE: UUID = uuid(1)   // Watch writes: its values, battery, time
    val PENDING: UUID = uuid(2) // Watch reads: the changes made in the app
    val SCHEMA: UUID = uuid(3)  // Watch writes: what its settings are

    // Advertisement flags
    const val FLAG_PAIRING = 0x01
    const val FLAG_PENDING = 0x02
    const val FLAG_WANT_STATE = 0x04
    const val FLAG_WANT_SCHEMA = 0x08

    // State flags
    const val STATE_JUST_PAIRED = 0x01

    /** Manufacturer data payload, after the company id: "LK", version, flags, token. */
    fun advertisement(flags: Int, token: Long): ByteArray =
        byteArrayOf('L'.code.toByte(), 'K'.code.toByte(), VERSION.toByte(), flags.toByte()) +
            le32(token)

    enum class Kind { BOOL, INT, ENUM, TZ_MINUTES }

    data class Setting(
        val id: Int,
        val kind: Kind,
        val group: String,
        val name: String,
        val min: Int,
        val max: Int,
        val options: List<String>,
    ) {
        fun clamp(v: Int) = v.coerceIn(min, max)
    }

    /** schema: version u8, count u8, then per entry: id u8, kind u8, min i16, max i16,
     *  group\0, name\0, options joined by '|'\0 */
    fun parseSchema(data: ByteArray): List<Setting> {
        val r = Reader(data)
        require(r.u8() == VERSION) { "schema version" }
        val count = r.u8()
        return List(count) {
            val id = r.u8()
            val kind = Kind.entries.getOrElse(r.u8()) { Kind.INT }
            val min = r.i16()
            val max = r.i16()
            val group = r.string()
            val name = r.string()
            val options = r.string().let { if (it.isEmpty()) emptyList() else it.split('|') }
            Setting(id, kind, group, name, min, max, options)
        }
    }

    data class State(
        val flags: Int,
        val token: Long,
        val schemaHash: Long,
        val ackRev: Int,
        val batteryMv: Int,
        val batteryPermille: Int,
        val watchTime: Long, // UTC seconds
        val values: Map<Int, Int>,
    )

    /** state: version u8, flags u8, token u32, schema hash u32, ack rev u16,
     *  battery mV u16, battery 0.1% u16, time u32, then (id u8, value i16) triples */
    fun parseState(data: ByteArray): State {
        val r = Reader(data)
        require(r.u8() == VERSION) { "state version" }
        return State(
            flags = r.u8(),
            token = r.u32(),
            schemaHash = r.u32(),
            ackRev = r.u16(),
            batteryMv = r.u16(),
            batteryPermille = r.u16(),
            watchTime = r.u32(),
            values = parseValues(r),
        )
    }

    /** pending: version u8, rev u16, then (id u8, value i16) triples */
    fun pending(rev: Int, changes: Map<Int, Int>): ByteArray {
        val out = ArrayList<Byte>()
        out += VERSION.toByte()
        out += le16(rev).toList()
        for ((id, v) in changes) {
            out += id.toByte()
            out += le16(v).toList()
        }
        return out.toByteArray()
    }

    /**
     * The watch writes STATE and SCHEMA as plain writes of pieces: offset u16, total u16,
     * then the data (no ATT long writes, their echo check failed on a Galaxy).
     */
    class Assembler {
        private var buffer = ByteArray(0)

        /** The whole value once its last piece is in, null before. Throws on a bad piece. */
        fun add(piece: ByteArray): ByteArray? {
            require(piece.size >= 4) { "short piece" }
            val offset = (piece[0].toInt() and 0xFF) or ((piece[1].toInt() and 0xFF) shl 8)
            val total = (piece[2].toInt() and 0xFF) or ((piece[3].toInt() and 0xFF) shl 8)
            if (offset == 0) buffer = ByteArray(0)
            require(offset == buffer.size) { "piece out of order" }
            buffer += piece.copyOfRange(4, piece.size)
            require(buffer.size <= total) { "longer than announced" }
            if (buffer.size < total) return null
            return buffer.also { buffer = ByteArray(0) }
        }
    }

    private fun parseValues(r: Reader): Map<Int, Int> {
        val values = LinkedHashMap<Int, Int>()
        while (r.remaining() >= 3) values[r.u8()] = r.i16()
        return values
    }

    private fun le16(v: Int) = byteArrayOf(v.toByte(), (v shr 8).toByte())
    private fun le32(v: Long) =
        byteArrayOf(v.toByte(), (v shr 8).toByte(), (v shr 16).toByte(), (v shr 24).toByte())

    private class Reader(private val data: ByteArray) {
        private var pos = 0
        fun remaining() = data.size - pos
        fun u8(): Int {
            require(pos < data.size) { "truncated" }
            return data[pos++].toInt() and 0xFF
        }
        fun u16() = u8() or (u8() shl 8)
        fun i16() = u16().toShort().toInt()
        fun u32() = u16().toLong() or (u16().toLong() shl 16)
        fun string(): String {
            val end = (pos until data.size).firstOrNull { data[it] == 0.toByte() }
                ?: throw IllegalArgumentException("unterminated string")
            val s = String(data, pos, end - pos, Charsets.UTF_8)
            pos = end + 1
            return s
        }
    }
}
