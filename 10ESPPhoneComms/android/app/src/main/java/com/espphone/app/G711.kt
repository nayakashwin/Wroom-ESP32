package com.espphone.app

// G.711 µ-law: 8 kHz mono 16-bit PCM <-> 64 kbps (canonical Sun variant).
object G711 {

    private const val BIAS = 0x84
    private const val CLIP = 32635

    // exponent of the highest set bit, clamped to 7: exp[0..1]=0, [2..3]=1,
    // [4..7]=2 ... [128..255]=7
    private val EXP = IntArray(256).also {
        var e = 0
        for (i in 0 until 256) {
            while (e < 7 && i >= (1 shl (e + 1))) e++
            it[i] = e
        }
    }

    fun linearToUlaw(sample: Int): Byte {
        var s = sample
        val sign = if (s < 0) 0x80 else 0
        if (sign != 0) s = -s
        if (s > CLIP) s = CLIP
        s += BIAS
        val exponent = EXP[(s shr 7) and 0xFF]
        val mantissa = (s shr (exponent + 3)) and 0x0F
        return ((sign or (exponent shl 4) or mantissa).inv() and 0xFF).toByte()
    }

    fun ulawToLinear(u: Byte): Short {
        val v = (u.toInt() and 0xFF) xor 0xFF
        var t = ((v and 0x0F) shl 3) + BIAS
        t = t shl ((v and 0x70) shr 4)
        val r = if (v and 0x80 != 0) BIAS - t else t - BIAS
        return r.toShort()
    }

    fun encode(pcm: ShortArray, count: Int): ByteArray {
        val out = ByteArray(count)
        for (i in 0 until count) out[i] = linearToUlaw(pcm[i].toInt())
        return out
    }

    fun decode(u: ByteArray): ShortArray {
        val out = ShortArray(u.size)
        for (i in u.indices) out[i] = ulawToLinear(u[i])
        return out
    }
}
