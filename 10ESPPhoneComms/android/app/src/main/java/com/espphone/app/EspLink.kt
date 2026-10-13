package com.espphone.app

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothSocket
import android.os.Handler
import android.os.Looper
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.util.UUID

// One SPP connection to an ESP32 ("ESPPhone-xxxx"). Speaks frames.
class EspLink(private val onEvent: (Event) -> Unit) {

    sealed class Event {
        data class Connected(val name: String) : Event()
        data class Disconnected(val why: String) : Event()
        data class Text(val text: String) : Event()
        data class Voice(val chunk: ByteArray) : Event()
        data class Ptt(val begin: Boolean) : Event()
        data class Info(val text: String) : Event()
        data class Stats(val frames: Long, val noise: Long) : Event()
    }

    companion object {
        val SPP_UUID: UUID = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB")
    }

    private val main = Handler(Looper.getMainLooper())
    private var socket: BluetoothSocket? = null
    private var output: OutputStream? = null
    private var reader: Thread? = null
    private val writeLock = Any()

    // generation counter: bumped by connect()/disconnect(); a reader loop only
    // keeps running (and only posts events) while its generation is current.
    @Volatile private var gen = 0

    val isConnected: Boolean get() = socket?.isConnected == true

    @SuppressLint("MissingPermission")
    fun connect(adapter: BluetoothAdapter, device: BluetoothDevice) {
        // stop any previous session silently
        val myGen: Int
        synchronized(this) {
            myGen = ++gen
            reader?.interrupt()
            val old = socket
            socket = null
            synchronized(writeLock) { output = null }
            try { old?.close() } catch (_: IOException) {}
        }
        val t = Thread({
            var attempt = 0
            var posted = false          // has a Disconnected been posted for this generation
            while (gen == myGen && !Thread.currentThread().isInterrupted) {
                var own: BluetoothSocket? = null
                var ownOut: OutputStream? = null
                try {
                    val s = device.createRfcommSocketToServiceRecord(SPP_UUID)
                    // no cancelDiscovery(): bonded-only picker, would need
                    // BLUETOOTH_SCAN on Android 12+
                    s.connect()
                    val o = s.outputStream
                    own = s; ownOut = o
                    synchronized(this) { socket = s }
                    synchronized(writeLock) { output = o }
                    attempt = 0
                    post(Event.Connected(s.remoteDevice.name ?: device.address))
                    posted = false
                    readLoop(s.inputStream)
                    // readLoop only returns on interrupt -> treat as closed
                    throw IOException("stream end")
                } catch (e: SecurityException) {
                    if (gen == myGen && !posted) {
                        posted = true
                        post(Event.Disconnected("permission denied"))
                    }
                    break                        // retrying won't fix permissions
                } catch (e: Exception) {
                    if (gen == myGen && (!posted || attempt == 0)) {
                        posted = true
                        val why = when (e) {
                            is IOException -> "io: ${e.message}"
                            else -> "${e.javaClass.simpleName}: ${e.message}"
                        }
                        post(Event.Disconnected("$why — retrying every 2 s"))
                    }
                } finally {
                    synchronized(writeLock) { if (output === ownOut) output = null }
                    synchronized(this) { if (socket === own) socket = null }
                    try { own?.close() } catch (_: IOException) {}
                }
                if (gen == myGen && !Thread.currentThread().isInterrupted) {
                    attempt++
                    try {
                        Thread.sleep(2000)
                    } catch (_: InterruptedException) {
                        break
                    }
                }
            }
        }, "esp-spp-reader")
        reader = t
        t.start()
    }

    private fun readLoop(input: InputStream) {
        val parser = Frame.Parser()
        val buf = ByteArray(512)
        while (!Thread.currentThread().isInterrupted) {
            val n = input.read(buf)
            if (n < 0) return
            for (i in 0 until n) {
                val f = parser.feed(buf[i]) ?: continue
                when (f.first) {
                    Frame.TEXT -> post(Event.Text(String(f.second, Charsets.UTF_8)))
                    Frame.VOICE -> post(Event.Voice(f.second))
                    Frame.PTT_BEGIN -> post(Event.Ptt(true))
                    Frame.PTT_END -> post(Event.Ptt(false))
                    Frame.INFO -> post(Event.Info(String(f.second, Charsets.UTF_8)))
                    else -> { /* PING/PONG handled where sent */ }
                }
                post(Event.Stats(parser.frames, parser.noise))
            }
        }
    }

    fun disconnect(why: String) {
        synchronized(this) {
            gen++
            reader?.interrupt()
            val old = socket
            socket = null
            synchronized(writeLock) { output = null }
            try { old?.close() } catch (_: IOException) {}
        }
        post(Event.Disconnected(why))
    }

    private fun post(e: Event) = main.post { onEvent(e) }

    fun sendFrame(type: Int, payload: ByteArray): Boolean {
        val out = synchronized(writeLock) { output } ?: return false
        return try {
            synchronized(writeLock) { out.write(Frame.encode(type, payload)); out.flush() }
            true
        } catch (_: IOException) {
            false
        }
    }

    fun sendText(text: String) = sendFrame(Frame.TEXT, text.toByteArray(Charsets.UTF_8))
    fun sendPtt(begin: Boolean) = sendFrame(if (begin) Frame.PTT_BEGIN else Frame.PTT_END, ByteArray(0))
    fun sendVoice(chunk: ByteArray) = sendFrame(Frame.VOICE, chunk)
}
