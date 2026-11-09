package com.lightink.companion

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test

/** Byte layouts as firmware/main/settings_table.cpp and ble.cpp write them. */
class ProtocolTest {
    private fun bytes(vararg v: Int) = ByteArray(v.size) { v[it].toByte() }
    private fun str(s: String) = s.toByteArray() + 0
    private fun le32(v: Int) = bytes(v, v shr 8, v shr 16, v shr 24)

    @Test
    fun uuidsSpellLightInk() {
        assertEquals("4c494748-5449-4e4b-0000-000000000000", Protocol.SERVICE.toString())
        assertEquals("4c494748-5449-4e4b-0000-000000000002", Protocol.PENDING.toString())
    }

    @Test
    fun advertisementLayout() {
        // The watch checks "LK", the version, then reads flags and the token (LE)
        assertArrayEquals(
            bytes('L'.code, 'K'.code, 2, 0x06, 0x78, 0x56, 0x34, 0x12),
            Protocol.advertisement(0x06, 0x12345678),
        )
        assertArrayEquals(
            bytes('L'.code, 'K'.code, 2, 0, 0xEF, 0xBE, 0xAD, 0xDE),
            Protocol.advertisement(0, 0xDEADBEEFL),
        )
    }

    @Test
    fun parsesSchema() {
        val data = bytes(2, 3) +
            // NUM(1, TzMinutes, kApp, "Clock", "Time zone", -720..840)
            bytes(1, 3) + le32(-720) + le32(840) + str("Clock") + str("Time zone") + str("") +
            // BOOL(2, kApp, "Clock", "Hour beep")
            bytes(2, 0) + le32(0) + le32(1) + str("Clock") + str("Hour beep") + str("") +
            // ENUM(23, kApp, "Display", "Menu refresh"), magic_enum names without '_'
            bytes(23, 2) + le32(0) + le32(3) + str("Display") + str("Menu refresh") +
            str("FULL|FAST|GOOD|QUICK")
        val s = Protocol.parseSchema(data)
        assertEquals(3, s.size)
        assertEquals(Protocol.Setting(1, Protocol.Kind.TZ_MINUTES, "Clock", "Time zone", -720, 840, emptyList()), s[0])
        assertEquals(Protocol.Kind.BOOL, s[1].kind)
        assertEquals(listOf("FULL", "FAST", "GOOD", "QUICK"), s[2].options)
        assertEquals(3, s[2].max)
    }

    @Test
    fun parsesState() {
        val data = bytes(2, 1) + // version, just paired
            bytes(0x78, 0x56, 0x34, 0x12) + // token
            bytes(0xEF, 0xBE, 0xAD, 0xDE) + // schema hash
            bytes(5, 0) + // ack rev
            bytes(0x6E, 0x0F) + // 3950 mV
            bytes(0x20, 0x03) + // 800 = 80.0 %
            bytes(0x00, 0x5E, 0xD0, 0xB2) + // time
            // Records: int16 time zone, bool, int8 sensitivity, a 3 byte record that
            // can't be a number (skipped), int32
            bytes(1, 2, 0x3C, 0x00) + bytes(2, 1, 1) + bytes(41, 1, 0xC4) +
            bytes(70, 3, 0, 1, 2) + bytes(60, 4) + le32(-2)
        val s = Protocol.parseState(data)
        assertEquals(Protocol.STATE_JUST_PAIRED, s.flags)
        assertEquals(0x12345678L, s.token)
        assertEquals(0xDEADBEEFL, s.schemaHash)
        assertEquals(5, s.ackRev)
        assertEquals(3950, s.batteryMv)
        assertEquals(800, s.batteryPermille)
        assertEquals(0xB2D05E00L, s.watchTime)
        assertEquals(mapOf(1 to 60, 2 to 1, 41 to -60, 60 to -2), s.values)
    }

    @Test
    fun stopsAtTruncatedRecord() {
        val head = bytes(2, 0) + ByteArray(18)
        val s = Protocol.parseState(head + bytes(4, 1, 8) + bytes(5, 2, 22))
        assertEquals(mapOf(4 to 8), s.values)
    }

    @Test
    fun encodesPending() {
        assertArrayEquals(
            bytes(2, 0x07, 0x00) + bytes(1, 4) + le32(-120) + bytes(20, 4) + le32(1),
            Protocol.pending(7, linkedMapOf(1 to -120, 20 to 1)),
        )
        assertArrayEquals(bytes(2, 0, 0), Protocol.pending(0, emptyMap()))
    }

    @Test
    fun joinsPieces() {
        val a = Protocol.Assembler()
        // 5 bytes in pieces of 3 and 2, as the watch's write() cuts them
        assertEquals(null, a.add(bytes(0, 0, 5, 0, 1, 2, 3)))
        assertArrayEquals(bytes(1, 2, 3, 4, 5), a.add(bytes(3, 0, 5, 0, 4, 5)))
        // A value that fits in one piece
        assertArrayEquals(bytes(9), a.add(bytes(0, 0, 1, 0, 9)))
        // Offset 0 starts over after an aborted value
        assertEquals(null, a.add(bytes(0, 0, 4, 0, 1, 2)))
        assertArrayEquals(bytes(7, 8), a.add(bytes(0, 0, 2, 0, 7, 8)))
    }

    @Test(expected = IllegalArgumentException::class)
    fun rejectsPieceOutOfOrder() {
        val a = Protocol.Assembler()
        a.add(bytes(0, 0, 6, 0, 1, 2))
        a.add(bytes(4, 0, 6, 0, 5, 6))
    }

    @Test(expected = IllegalArgumentException::class)
    fun rejectsOtherVersions() {
        Protocol.parseState(bytes(1, 0))
    }
}
