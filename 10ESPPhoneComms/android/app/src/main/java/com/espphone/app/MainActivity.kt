package com.espphone.app

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.app.AlertDialog
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.media.AudioManager
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.Gravity
import android.view.View
import android.view.inputmethod.EditorInfo
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.TextView
import android.widget.Toast

class MainActivity : Activity(), SensorEventListener {

    private val REQ_PERMS = 1
    private val REQ_ENABLE_BT = 2

    private val TAG = "espphone"

    private lateinit var status: TextView
    private lateinit var chatAdapter: ArrayAdapter<String>
    private lateinit var chatList: ListView
    private lateinit var muteBtn: Button
    private lateinit var speakerBtn: Button
    private lateinit var screen: FrameLayout

    private var link: EspLink? = null
    private var remoteTalking = false
    private var voiceTx: Voice.Tx? = null
    private var voiceRx = Voice.Rx()

    // conversation state: mic streams while connected & unmuted (hands free)
    private var muted = false
    private var speakerOn = true    // hands-free by default; toggle for earpiece

    // diagnostics for the long-press stats dialog
    private var sentTexts = 0; private var recvTexts = 0
    private var sentVoice = 0L; private var recvVoice = 0L
    private var connects = 0; private var lastWhy = "-"
    private var connectedAtMs = 0L
    private var lastInfo = "-"

    private var sensorManager: SensorManager? = null
    private var proxSensor: Sensor? = null
    private var earCover: View? = null   // black overlay while phone at ear

    private val audioManager: AudioManager
        get() = getSystemService(Context.AUDIO_SERVICE) as AudioManager

    private val adapter: BluetoothAdapter?
        get() = (getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        buildUi()
        sensorManager = getSystemService(Context.SENSOR_SERVICE) as SensorManager
        proxSensor = sensorManager?.getDefaultSensor(Sensor.TYPE_PROXIMITY)
        if (!hasPerms()) requestPerms()
    }

    // ------------------------------------------------------------------- ui
    private fun buildUi() {
        val pad = (resources.displayMetrics.density * 8).toInt()
        screen = FrameLayout(this)
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
        }

        status = TextView(this).apply { text = "not connected"; textSize = 14f }
        status.setOnLongClickListener { showStats(); true }
        root.addView(status)

        val connect = Button(this).apply { text = "Connect ESP32" }
        connect.setOnClickListener { pickDevice() }
        root.addView(connect)

        chatAdapter = ArrayAdapter(this, android.R.layout.simple_list_item_1,
                                    ArrayList<String>())
        chatList = ListView(this).apply {
            adapter = chatAdapter
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f)
            divider = null
        }
        root.addView(chatList)

        val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val input = EditText(this).apply {
            hint = "message"
            layoutParams = LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
            imeOptions = EditorInfo.IME_ACTION_SEND
        }
        val sendBtn = Button(this).apply { text = "Send"; gravity = Gravity.CENTER_VERTICAL }
        input.setOnEditorActionListener { _, actionId, _ ->
            if (actionId == EditorInfo.IME_ACTION_SEND) { send(input); true } else false
        }
        sendBtn.setOnClickListener { send(input) }
        row.addView(input); row.addView(sendBtn)
        root.addView(row)

        val voiceRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        muteBtn = Button(this).apply { text = "MUTE" }
        muteBtn.setOnClickListener { toggleMute() }
        speakerBtn = Button(this).apply { text = "SPEAKER: ON" }
        speakerBtn.setOnClickListener { toggleSpeaker() }
        val lp = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
        voiceRow.addView(muteBtn, lp)
        voiceRow.addView(speakerBtn, LinearLayout.LayoutParams(0,
            LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        root.addView(voiceRow)

        screen.addView(root, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        setContentView(screen)
        applyAudioMode()
    }

    private fun send(input: EditText) {
        val t = input.text.toString().trim()
        if (t.isEmpty()) return
        if (link?.sendText(t) != true) {
            toast("not connected"); return
        }
        sentTexts++
        input.text.clear()
        addLine("Me: $t")
    }

    private fun showStats() {
        val up = if (connectedAtMs > 0)
            (System.currentTimeMillis() - connectedAtMs) / 1000 else 0
        val msg = """
            link: ${link?.isConnected ?: false}   mute=$muted   speaker=$speakerOn
            session: ${up / 60}m ${up % 60}s   connects=$connects
            last disconnect: $lastWhy
            last info: $lastInfo
            texts: sent $sentTexts / recv $recvTexts
            voice chunks: sent $sentVoice / recv $recvVoice
        """.trimIndent()
        AlertDialog.Builder(this).setTitle("Stats").setMessage(msg)
            .setPositiveButton("Close", null).show()
    }

    // ------------------------------------------------------------- voice ctl
    private fun toggleMute() {
        muted = !muted
        if (link?.isConnected == true) {
            if (muted) {
                stopVoiceTx()
                link?.sendPtt(false)
            } else {
                if (startVoiceTx()) link?.sendPtt(true)
            }
        }
        updateVoiceButtons()
    }

    private fun toggleSpeaker() {
        speakerOn = !speakerOn
        voiceRx.route(speakerOn, audioManager)
        applyAudioMode()
        updateVoiceButtons()
    }

    private fun applyAudioMode() {
        try {
            val am = audioManager
            if (link?.isConnected == true) {
                am.mode = AudioManager.MODE_IN_COMMUNICATION
                am.setSpeakerphoneOn(speakerOn)
                Log.i(TAG, "audio mode COM, speaker=$speakerOn")
            } else {
                am.setSpeakerphoneOn(false)
                am.mode = AudioManager.MODE_NORMAL
                Log.i(TAG, "audio mode NORMAL")
            }
        } catch (e: Exception) {
            Log.w(TAG, "applyAudioMode failed", e)   // routing falls back to preferredDevice
        }
    }

    private fun updateVoiceButtons() {
        muteBtn.text = if (muted) "UNMUTE" else "MUTE"
        speakerBtn.text = if (speakerOn) "SPEAKER: ON" else "SPEAKER: OFF"
    }

    private fun startVoiceTx(): Boolean {
        if (Build.VERSION.SDK_INT >= 23 &&
            checkSelfPermission(Manifest.permission.RECORD_AUDIO) !=
            PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), REQ_PERMS)
            return false
        }
        voiceTx?.stop()
        val tx = Voice.Tx { chunk -> if (link?.sendVoice(chunk) == true) sentVoice++ }
        if (!tx.start()) { toast("mic unavailable"); return false }
        voiceTx = tx
        return true
    }

    private fun stopVoiceTx() {
        voiceTx?.stop()
        voiceTx = null
    }

    override fun onRequestPermissionsResult(requestCode: Int,
        permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQ_PERMS && grantResults.isNotEmpty() &&
            grantResults[0] == PackageManager.PERMISSION_GRANTED &&
            !muted && link?.isConnected == true) {
            startVoiceTx()
        }
    }

    private fun addLine(s: String) {
        runOnUiThread {
            chatAdapter.add(s)
            chatAdapter.notifyDataSetChanged()
            chatList.setSelection(chatAdapter.count - 1)   // always show newest
        }
    }

    private fun setStatus(s: String) = runOnUiThread { status.text = s }

    private fun toast(s: String) = runOnUiThread { Toast.makeText(this, s, Toast.LENGTH_SHORT).show() }

    // ---------------------------------------------------------- proximity
    override fun onResume() {
        super.onResume()
        proxSensor?.let { sensorManager?.registerListener(this, it, SensorManager.SENSOR_DELAY_NORMAL) }
    }

    override fun onPause() {
        super.onPause()
        sensorManager?.unregisterListener(this)
    }

    override fun onSensorChanged(event: SensorEvent) {
        if (event.sensor.type != Sensor.TYPE_PROXIMITY) return
        val near = event.values[0] < event.sensor.maximumRange
        runOnUiThread { if (near) enterEarMode() else leaveEarMode() }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}

    // phone against ear: black out the screen and swallow all touches so
    // nothing can be pressed by a cheek (like an in-call phone)
    private fun enterEarMode() {
        if (earCover != null) return
        val v = View(this).apply {
            setBackgroundColor(Color.BLACK)
            setOnTouchListener { _, _ -> true }
        }
        screen.addView(v, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        earCover = v
        window.attributes = window.attributes.apply { screenBrightness = 0f }
    }

    private fun leaveEarMode() {
        earCover?.let { screen.removeView(it) }
        earCover = null
        window.attributes = window.attributes.apply { screenBrightness = -1f }
    }

    // ------------------------------------------------------------ connection
    private fun hasPerms(): Boolean {
        if (Build.VERSION.SDK_INT < 31) return true
        return checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) ==
               PackageManager.PERMISSION_GRANTED
    }

    private fun requestPerms() {
        if (Build.VERSION.SDK_INT >= 31)
            requestPermissions(arrayOf(Manifest.permission.BLUETOOTH_CONNECT), REQ_PERMS)
    }

    private fun pickDevice() {
        val a = adapter
        if (a == null) { toast("no bluetooth on this device"); return }
        if (!hasPerms()) { requestPerms(); return }
        if (!a.isEnabled) {
            startActivityForResult(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE), REQ_ENABLE_BT)
            return
        }
        @SuppressLint("MissingPermission")
        val bonded: List<BluetoothDevice> = a.bondedDevices.sortedBy { it.name ?: it.address }
        if (bonded.isEmpty()) {
            toast("no paired devices - pair the ESP32 in system settings first")
            return
        }
        val names = bonded.map { "${it.name ?: "?"}  (${it.address})" }.toTypedArray()
        AlertDialog.Builder(this)
            .setTitle("Pick ESP32")
            .setItems(names) { _, i -> connectTo(bonded[i]) }
            .setNegativeButton("Cancel", null)
            .show()
    }

    @SuppressLint("MissingPermission")
    private fun connectTo(d: BluetoothDevice) {
        val a = adapter ?: return
        setStatus("connecting ${d.name ?: d.address} ...")
        link = EspLink { e -> onLinkEvent(e) }
        link!!.connect(a, d)
    }

    private fun onLinkEvent(e: EspLink.Event) {
        when (e) {
            is EspLink.Event.Connected -> {
                setStatus("connected: ${e.name}")
                addLine("-- connected to ${e.name} --")
                connects++
                connectedAtMs = System.currentTimeMillis()
                voiceRx.route(speakerOn, audioManager)
                applyAudioMode()
                if (!muted && startVoiceTx()) link?.sendPtt(true)
            }
            is EspLink.Event.Disconnected -> {
                setStatus("disconnected (${e.why})")
                remoteTalking = false
                lastWhy = e.why
                stopVoiceTx()
                voiceRx.stop()
                applyAudioMode()
                addLine("-- disconnected: ${e.why} --")
            }
            is EspLink.Event.Text -> { recvTexts++; addLine("Them: ${e.text}") }
            is EspLink.Event.Ptt -> {
                remoteTalking = e.begin
                setStatus(when {
                    e.begin -> "connected - REMOTE TALKING"
                    link?.isConnected == true -> "connected"
                    else -> "disconnected"
                })
                if (!e.begin) voiceRx.stop()   // far side muted: release the player
            }
            is EspLink.Event.Info -> { lastInfo = e.text; setStatus(e.text) }
            is EspLink.Event.Voice -> { recvVoice++; voiceRx.feed(e.chunk) }
            is EspLink.Event.Stats -> { /* diagnostic hook */ }
        }
    }

    override fun onDestroy() {
        stopVoiceTx()
        voiceRx.stop()
        link?.disconnect("activity destroyed")
        applyAudioMode()
        leaveEarMode()
        super.onDestroy()
    }
}
