package com.espphone.app

// Port of the ESP32 side frame.h: A5 5A <type> <len16le> <payload>.
object Frame {
    const val M1: Int = 0xA5
    const val M2: Int = 0x5A
    const val HDR: Int = 5
    const val MAX_PAYLOAD: Int = 1024

    const val TEXT: Int = 0x01
    const val VOICE: Int = 0x02
    const val PTT_BEGIN: Int = 0x03
    const val PTT_END: Int = 0x04
    const val PING: Int = 0x05
    const val PONG: Int = 0x06
    const val INFO: Int = 0x07

    fun encode(type: Int, payload: ByteArray): ByteArray {
        val out = ByteArray(HDR + payload.size)
        out[0] = M1.toByte()
        out[1] = M2.toByte()
        out[2] = type.toByte()
        out[3] = (payload.size and 0xFF).toByte()
        out[4] = ((payload.size shr 8) and 0xFF).toByte()
        payload.copyInto(out, HDR)
        return out
    }

    // Streaming resynchronizing parser; feed() returns (type, payload) when a
    // complete frame lands, else null. Non-frame noise is skipped.
    class Parser {
        private var buf = ByteArray(HDR + MAX_PAYLOAD)
        private var pos = 0
        var noise = 0L; private set
        var frames = 0L; private set

        fun feed(b: Byte): Pair<Int, ByteArray>? {
            val v = b.toInt() and 0xFF
            if (pos == 0) {
                if (v == M1) buf[pos++] = b else noise++
                return null
            }
            if (pos == 1) {
                when {
                    v == M2 -> buf[pos++] = b
                    v == M1 -> { /* still a candidate start */ }
                    else -> { pos = 0; noise++ }
                }
                return null
            }
            buf[pos++] = b
            if (pos < HDR) return null
            val len = (buf[3].toInt() and 0xFF) or ((buf[4].toInt() and 0xFF) shl 8)
            if (len > MAX_PAYLOAD) { pos = 0; noise += HDR; return null }
            if (pos == HDR + len) {
                val out = if (len == 0) ByteArray(0) else buf.copyOfRange(HDR, HDR + len)
                frames++
                pos = 0
                return Pair(buf[2].toInt() and 0xFF, out)
            }
            return null
        }
    }
}
