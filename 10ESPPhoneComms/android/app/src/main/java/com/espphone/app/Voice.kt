package com.espphone.app

import android.annotation.SuppressLint
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.MediaRecorder
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.atomic.AtomicBoolean

// Voice pump: mic -> µ-law 20 ms chunks -> EspLink.sendVoice while unmuted
// (continuous, hands-free); arriving chunks -> jitter buffer (4 chunks
// prebuffer) -> paced playback with silence concealment on gaps.
object Voice {

    const val RATE = 8000
    const val FRAME_MS = 20
    const val SAMPLES = RATE / 1000 * FRAME_MS       // 160
    const val BYTES = SAMPLES                        // µ-law: 1 B/sample
    const val JITTER_CHUNKS = 4                      // 80 ms prebuffer
    const val QUEUE_CHUNKS = 12                      // 240 ms ceiling

    // ------------------------------------------------------------------- tx
    class Tx(private val send: (ByteArray) -> Unit) {
        private var record: AudioRecord? = null
        private var thread: Thread? = null
        val running = AtomicBoolean(false)

        @SuppressLint("MissingPermission")   // caller checks RECORD_AUDIO
        fun start(): Boolean {
            if (running.get()) return true
            val min = AudioRecord.getMinBufferSize(RATE, AudioFormat.CHANNEL_IN_MONO,
                                                   AudioFormat.ENCODING_PCM_16BIT)
            if (min <= 0) return false
            val r = AudioRecord(MediaRecorder.AudioSource.VOICE_COMMUNICATION, RATE,
                               AudioFormat.CHANNEL_IN_MONO,
                               AudioFormat.ENCODING_PCM_16BIT,
                               maxOf(min, SAMPLES * 4))
            if (r.state != AudioRecord.STATE_INITIALIZED) { r.release(); return false }
            record = r
            r.startRecording()
            running.set(true)
            thread = Thread {
                val pcm = ShortArray(SAMPLES)
                while (running.get()) {
                    var got = 0
                    while (got < SAMPLES && running.get()) {
                        val n = r.read(pcm, got, SAMPLES - got)
                        if (n <= 0) break
                        got += n
                    }
                    if (got < SAMPLES) continue
                    send(G711.encode(pcm, SAMPLES))
                }
            }.apply { name = "voice-tx"; start() }
            return true
        }

        fun stop() {
            running.set(false)
            thread?.interrupt()
            thread = null
            try { record?.stop() } catch (_: Exception) {}
            record?.release()
            record = null
        }
    }

    // ------------------------------------------------------------------- rx
    class Rx {
        private val q = ArrayBlockingQueue<ByteArray>(QUEUE_CHUNKS)
        private var track: AudioTrack? = null
        private var thread: Thread? = null
        private val playing = AtomicBoolean(false)
        var droppedChunks = 0L; private set
        var lostChunks = 0L; private set   // concealed with silence

        @Volatile private var useSpeaker = true
        private var spkDev: AudioDeviceInfo? = null
        private var earDev: AudioDeviceInfo? = null

        /** Output routing: force the built-in speaker or the earpiece. */
        fun route(speaker: Boolean, am: AudioManager) {
            useSpeaker = speaker
            spkDev = am.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
                .firstOrNull { it.type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER }
            earDev = am.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
                .firstOrNull { it.type == AudioDeviceInfo.TYPE_BUILTIN_EARPIECE }
            applyRoute()
        }

        private fun applyRoute() {
            val t = track ?: return
            t.preferredDevice = if (useSpeaker) spkDev else earDev
        }

        fun feed(chunk: ByteArray) {
            while (!q.offer(chunk)) {          // full: drop oldest, keep live
                q.poll(); droppedChunks++
            }
            if (!playing.get()) start()
        }

        fun stop() {
            playing.set(false)
            thread?.interrupt()
            thread = null
            q.clear()
            try { track?.stop() } catch (_: Exception) {}
            track?.release()
            track = null
        }

        private fun start() {
            playing.set(true)
            val min = AudioTrack.getMinBufferSize(RATE, AudioFormat.CHANNEL_OUT_MONO,
                                                  AudioFormat.ENCODING_PCM_16BIT)
            val t = AudioTrack(AudioManager.STREAM_VOICE_CALL, RATE,
                               AudioFormat.CHANNEL_OUT_MONO,
                               AudioFormat.ENCODING_PCM_16BIT,
                               maxOf(min, SAMPLES * 8), AudioTrack.MODE_STREAM)
            track = t
            applyRoute()
            t.play()
            thread = Thread {
                // prebuffer: wait for JITTER_CHUNKS before the first pop so
                // radio jitter (measured max ~56 ms, budget 80 ms) is absorbed
                var nextAt = System.nanoTime()
                val period = FRAME_MS * 1_000_000L
                while (playing.get()) {
                    val chunk = pollWithDeadline(nextAt)
                    val pcm = if (chunk != null) G711.decode(chunk)
                              else { lostChunks++; ShortArray(SAMPLES) }  // conceal
                    t.write(pcm, 0, pcm.size)
                    nextAt += period
                    val ahead = (nextAt - System.nanoTime()) / 1_000_000
                    if (ahead > 1) try {
                        Thread.sleep(ahead.coerceAtMost(FRAME_MS.toLong()))
                    } catch (_: InterruptedException) { break }
                }
            }.apply { name = "voice-rx"; start() }
        }

        private fun pollWithDeadline(deadlineNs: Long): ByteArray? {
            val remain = (deadlineNs - System.nanoTime()).coerceAtLeast(0)
            return try {
                q.poll(remain, java.util.concurrent.TimeUnit.NANOSECONDS)
            } catch (_: InterruptedException) {   // stop() raced our poll:
                Thread.currentThread().interrupt() // exit via the loop check,
                null                              // never kill the process
            }
        }
    }
}
