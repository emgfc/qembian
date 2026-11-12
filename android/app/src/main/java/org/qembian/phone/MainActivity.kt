package org.qembian.phone

import android.annotation.SuppressLint
import android.app.Activity
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.system.Os
import android.util.Log
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.PopupMenu
import android.widget.TextView
import android.window.OnBackInvokedDispatcher
import org.json.JSONObject
import java.io.File
import java.io.RandomAccessFile

/** A key from the bundle (bundle.json "keys"): its name, label, QEMU key code name, group and style. */
data class BundleKey(val key: String, val label: String, val qcode: String, val group: String, val style: String, val col: Int)

/**
 * The phone, full screen (android/README.md). The bundle (made on the PC by android/tools/make_bundle.py,
 * pushed to <external files>/bundles/<id>/) says everything about the device; this activity only:
 * shows the screen (QEMU draws into the SurfaceView's window: present=window), passes touches (the picture
 * keeps its aspect ratio, black bars around it), the front key strip under the picture, the volume keys,
 * and a floating button for the side keys and Exit. Android's own back gesture is eaten.
 */
class MainActivity : Activity(), SurfaceHolder.Callback {
    private var bundleDir: File? = null
    private var scrW = 360
    private var scrH = 640
    private var keys = listOf<BundleKey>()
    private lateinit var surface: SurfaceView

    override fun onCreate(saved: Bundle?) {
        super.onCreate(saved)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        window.setDecorFitsSystemWindows(false)
        immersive()
        val b = findBundle()
        Log.i("qembian", "bundle: ${b?.absolutePath} (looked in ${getExternalFilesDir("bundles")})")
        if (b == null) {
            setContentView(TextView(this).apply {
                setTextColor(Color.WHITE)
                setPadding(48, 160, 48, 48)
                text = "No phone bundle.\n\nOn the PC:\n  python3 android/tools/make_bundle.py nokia-808-rm807\n" +
                    "  android/tools/push_bundle.sh nokia-808-rm807\n\nthen start the app again."
            })
            return
        }
        bundleDir = b
        val info = JSONObject(File(b, "bundle.json").readText())
        scrW = info.optInt("width", 360)
        scrH = info.optInt("height", 640)
        val ka = info.optJSONArray("keys")
        val all = mutableListOf<BundleKey>()
        for (i in 0 until (ka?.length() ?: 0)) {
            val k = ka!!.getJSONObject(i)
            all += BundleKey(k.getString("key"), k.optString("label"), k.getString("qcode"), k.optString("group"),
                k.optString("style"), k.optInt("col"))
        }
        keys = all
        buildUi()
        // the first start copies the C: drive (hundreds of MB): off the UI thread, with a note on the screen
        val note = TextView(this).apply { setTextColor(0xFFBDBDBD.toInt()); textSize = 15f; gravity = Gravity.CENTER }
        (window.decorView as android.view.ViewGroup).addView(note, android.view.ViewGroup.LayoutParams(-1, -1))
        Thread {
            startPhone(b, info) { msg -> runOnUiThread { note.text = msg } }
            runOnUiThread { (note.parent as? android.view.ViewGroup)?.removeView(note) }
        }.start()
        if (Build.VERSION.SDK_INT >= 33) {
            onBackInvokedDispatcher.registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_OVERLAY) { }
        }
    }

    /** sticky immersive: the bars only show on an edge swipe, and the swipe still reaches the phone */
    private fun immersive() {
        window.decorView                // the insets controller lives on the decor view: make it first
        window.insetsController?.let {
            it.hide(WindowInsets.Type.systemBars())
            it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) immersive()
    }

    /** the app's own storage (android/tools/push_bundle.sh copies there), else its external files dir */
    private fun findBundle(): File? {
        for (dir in listOfNotNull(File(filesDir, "bundles"), getExternalFilesDir("bundles"))) {
            val b = dir.listFiles()?.filter { File(it, "qemu-args.txt").exists() }?.minByOrNull { it.name }
            if (b != null) return b
        }
        return null
    }

    @SuppressLint("ClickableViewAccessibility")
    private fun buildUi() {
        val root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK) }
        val column = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        surface = SurfaceView(this)
        surface.holder.addCallback(this)
        surface.setOnTouchListener { v, e -> onScreenTouch(v, e) }
        column.addView(surface, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
        val front = keys.filter { it.group == "front" }.sortedBy { it.col }
        val strip = KeyStrip(this, front)
        val stripH = (resources.displayMetrics.widthPixels * 0.24f).toInt()
        column.addView(strip, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, stripH))
        root.addView(column, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        root.addView(floatingButton(), FrameLayout.LayoutParams(dp(48), dp(48), Gravity.TOP or Gravity.END).apply {
            topMargin = dp(96); rightMargin = dp(12)
        })
        setContentView(root)
    }

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    /** the floating button: semi-transparent, draggable; tap: the side keys and Exit */
    @SuppressLint("ClickableViewAccessibility")
    private fun floatingButton(): View {
        val b = TextView(this).apply {
            text = "≡"
            textSize = 22f
            gravity = Gravity.CENTER
            setTextColor(0xCCFFFFFF.toInt())
            background = GradientDrawable().apply { shape = GradientDrawable.OVAL; setColor(0x55303030) }
            alpha = 0.7f
        }
        var downX = 0f; var downY = 0f; var moved = false
        b.setOnTouchListener { v, e ->
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> { downX = e.rawX - v.x; downY = e.rawY - v.y; moved = false }
                MotionEvent.ACTION_MOVE -> {
                    val nx = e.rawX - downX; val ny = e.rawY - downY
                    if (Math.abs(nx - v.x) + Math.abs(ny - v.y) > 6) moved = true
                    v.x = nx; v.y = ny
                }
                MotionEvent.ACTION_UP -> if (!moved) showMenu(v)
            }
            true
        }
        return b
    }

    private fun showMenu(anchor: View) {
        val m = PopupMenu(this, anchor)
        val side = keys.filter { it.group != "front" }
        side.forEachIndexed { i, k -> m.menu.add(0, i, i, k.label) }
        m.menu.add(0, 1000, 1000, "Exit")
        m.setOnMenuItemClickListener { item ->
            if (item.itemId == 1000) exitPhone() else press(side[item.itemId].qcode)
            true
        }
        m.show()
    }

    private fun press(qcode: String) {
        Native.key(qcode, true)
        Handler(Looper.getMainLooper()).postDelayed({ Native.key(qcode, false) }, 150)
    }

    private fun exitPhone() {
        Native.quit()                   // QEMU shuts down (drives flushed) and ends the process
        Handler(Looper.getMainLooper()).postDelayed({
            finishAndRemoveTask()
            android.os.Process.killProcess(android.os.Process.myPid())
        }, 3000)
    }

    /** the finger the phone follows (single touch until multitouch is in: android/README.md): Android's pointer
        id of the gesture's first finger, -1 when none. Other fingers are ignored; when the followed finger lifts
        the touch ends, and the rest of the gesture is ignored until every finger is up (otherwise Android's
        pointer index 0 becomes another finger and the phone saw its finger jump across the screen) */
    private var followed = -1
    private var gestureDone = false

    /** the phone's screen inside the surface: scaled to fit, centred (as syborg-gl draws it) */
    private fun onScreenTouch(v: View, e: MotionEvent): Boolean {
        val sc = Math.min(v.width.toFloat() / scrW, v.height.toFloat() / scrH)
        val dw = scrW * sc; val dh = scrH * sc
        val dx = (v.width - dw) / 2; val dy = (v.height - dh) / 2
        fun sendAt(px: Float, py: Float, down: Boolean) {
            val x = (((px - dx) / dw).coerceIn(0f, 1f) * 0x7fff).toInt()
            val y = (((py - dy) / dh).coerceIn(0f, 1f) * 0x7fff).toInt()
            Native.touch(0, x, y, down)
        }
        fun send(i: Int, down: Boolean) = sendAt(e.getX(i), e.getY(i), down)
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                followed = e.getPointerId(0); gestureDone = false
                send(0, true)
            }
            MotionEvent.ACTION_MOVE -> {
                val i = if (followed >= 0) e.findPointerIndex(followed) else -1
                if (i >= 0 && !gestureDone) {
                    // every sample Android batched into this event, in time order (the phone sees the
                    // finger's whole path, as a real digitiser reports it)
                    for (h in 0 until e.historySize) sendAt(e.getHistoricalX(i, h), e.getHistoricalY(i, h), true)
                    send(i, true)
                }
            }
            MotionEvent.ACTION_POINTER_UP -> {
                if (e.getPointerId(e.actionIndex) == followed && !gestureDone) {
                    send(e.actionIndex, false); gestureDone = true
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                if (!gestureDone && followed >= 0) {
                    val i = e.findPointerIndex(followed)
                    send(if (i >= 0) i else 0, false)
                }
                followed = -1; gestureDone = false
            }
        }
        return true
    }

    private fun keyFor(code: Int): String? = when (code) {
        KeyEvent.KEYCODE_VOLUME_UP -> keys.firstOrNull { it.key == "volup" }?.qcode
        KeyEvent.KEYCODE_VOLUME_DOWN -> keys.firstOrNull { it.key == "voldown" }?.qcode
        else -> null
    }

    override fun dispatchKeyEvent(e: KeyEvent): Boolean {
        if (e.keyCode == KeyEvent.KEYCODE_BACK) return true      // eaten: a stray swipe never leaves the phone
        val q = keyFor(e.keyCode) ?: return super.dispatchKeyEvent(e)
        if (e.repeatCount == 0) {
            when (e.action) {
                KeyEvent.ACTION_DOWN -> Native.key(q, true)
                KeyEvent.ACTION_UP -> Native.key(q, false)
            }
        }
        return true
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() { /* eaten */ }

    /** drives in the app's own storage (kept across bundle updates), then QEMU with the bundle's arguments */
    private fun startPhone(b: File, info: JSONObject, progress: (String) -> Unit) {
        val id = info.optString("id", b.name)
        val data = File(filesDir, "devices/$id").apply { mkdirs() }
        val drives = info.optJSONArray("drives")
        for (i in 0 until (drives?.length() ?: 0)) {
            val d = drives!!.getJSONObject(i)
            val f = File(data, d.getString("file"))
            if (f.exists()) continue
            val part = File(data, d.getString("file") + ".part")
            if (d.has("from")) {        // copied whole, then renamed: an interrupted copy starts again
                val src = File(b, d.getString("from"))
                src.inputStream().use { inp -> part.outputStream().use { out ->
                    val buf = ByteArray(4 shl 20); var done = 0L; var n: Int
                    while (inp.read(buf).also { n = it } > 0) {
                        out.write(buf, 0, n); done += n
                        progress("Preparing the phone's memory: ${done * 100 / src.length()} %")
                    }
                } }
            } else {
                RandomAccessFile(part, "rw").use { it.setLength(d.getLong("size_mb") * 1024 * 1024) }   // sparse
            }
            part.renameTo(f)
        }
        progress("")
        Os.setenv("TMPDIR", cacheDir.absolutePath, true)
        Os.setenv("HOME", filesDir.absolutePath, true)
        val args = File(b, "qemu-args.txt").readLines().filter { it.isNotEmpty() }
            .map { it.replace("@BUNDLE@", b.absolutePath).replace("@DATA@", data.absolutePath) }
        Log.i("qembian", "starting QEMU: ${args.size} arguments, data in $data")
        Native.start(args.toTypedArray())
    }

    override fun surfaceCreated(h: SurfaceHolder) {}
    override fun surfaceChanged(h: SurfaceHolder, format: Int, w: Int, hh: Int) { Native.setWindow(h.surface) }
    override fun surfaceDestroyed(h: SurfaceHolder) { Native.setWindow(null) }
}
