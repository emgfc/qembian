package org.qembian.phone

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.graphics.Shader
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View

/**
 * The phone's front key strip under the screen, drawn after the real phone (the Nokia 808 PureView: one
 * glossy black strip with the green Call key, the Menu key and the red End key, and the notification light).
 * The keys come from the bundle (bundle.json "keys", group "front"), so another phone shows its own.
 */
class KeyStrip(context: Context, private val keys: List<BundleKey>) : View(context) {
    private val strip = Paint(Paint.ANTI_ALIAS_FLAG)
    private val edge = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; color = 0xFF3A3A3A.toInt() }
    private val icon = Paint(Paint.ANTI_ALIAS_FLAG)
    private val glow = Paint(Paint.ANTI_ALIAS_FLAG)
    private val sep = Paint().apply { color = 0xFF262626.toInt() }
    private var pressed = -1

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        val pad = h * 0.14f
        val r = RectF(w * 0.03f, pad, w * 0.97f, h - pad)
        strip.shader = LinearGradient(0f, r.top, 0f, r.bottom, 0xFF2A2A2A.toInt(), 0xFF050505.toInt(), Shader.TileMode.CLAMP)
        c.drawRoundRect(r, r.height() * 0.5f, r.height() * 0.5f, strip)
        edge.strokeWidth = h * 0.012f
        c.drawRoundRect(r, r.height() * 0.5f, r.height() * 0.5f, edge)
        if (keys.isEmpty()) return
        val kw = r.width() / keys.size
        for ((i, k) in keys.withIndex()) {
            val cx = r.left + kw * (i + 0.5f)
            val cy = r.centerY()
            if (i > 0) c.drawRect(r.left + kw * i - 1f, r.top + r.height() * 0.25f, r.left + kw * i + 1f, r.bottom - r.height() * 0.25f, sep)
            if (i == pressed) {
                glow.color = 0x30FFFFFF
                c.drawRoundRect(RectF(cx - kw * 0.42f, r.top + 4, cx + kw * 0.42f, r.bottom - 4), r.height() * 0.4f, r.height() * 0.4f, glow)
            }
            val s = r.height() * 0.30f
            when (k.style) {
                "call" -> handset(c, cx, cy, s, 0xFF35C759.toInt(), false)
                "end" -> handset(c, cx, cy, s, 0xFFE53935.toInt(), true)
                else -> menuGlyph(c, cx, cy, s)
            }
        }
        // the notification light, left of the strip as on the 808
        glow.color = 0xFF101010.toInt()
        c.drawCircle(r.left - w * 0.012f + r.height() * 0.5f, r.top - pad * 0.45f, h * 0.025f, glow)
    }

    private fun handset(c: Canvas, cx: Float, cy: Float, s: Float, color: Int, down: Boolean) {
        icon.color = color
        icon.style = Paint.Style.FILL
        val p = Path()
        // a telephone receiver: two ear cups joined by a curved handle (the end key's points down)
        val dir = if (down) 1f else -1f
        p.moveTo(cx - s * 1.25f, cy + dir * s * 0.10f)
        p.cubicTo(cx - s * 0.9f, cy - dir * s * 0.75f, cx + s * 0.9f, cy - dir * s * 0.75f, cx + s * 1.25f, cy + dir * s * 0.10f)
        p.lineTo(cx + s * 1.25f, cy + dir * s * 0.42f)
        p.lineTo(cx + s * 0.62f, cy + dir * s * 0.42f)
        p.lineTo(cx + s * 0.55f, cy + dir * s * 0.12f)
        p.cubicTo(cx + s * 0.25f, cy - dir * s * 0.12f, cx - s * 0.25f, cy - dir * s * 0.12f, cx - s * 0.55f, cy + dir * s * 0.12f)
        p.lineTo(cx - s * 0.62f, cy + dir * s * 0.42f)
        p.lineTo(cx - s * 1.25f, cy + dir * s * 0.42f)
        p.close()
        c.drawPath(p, icon)
    }

    private fun menuGlyph(c: Canvas, cx: Float, cy: Float, s: Float) {
        icon.color = 0xFFBDBDBD.toInt()
        icon.style = Paint.Style.STROKE
        icon.strokeWidth = s * 0.16f
        c.drawRoundRect(RectF(cx - s * 0.9f, cy - s * 0.55f, cx + s * 0.9f, cy + s * 0.55f), s * 0.25f, s * 0.25f, icon)
        icon.style = Paint.Style.FILL
        c.drawCircle(cx, cy, s * 0.16f, icon)
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        if (keys.isEmpty()) return true
        val i = (e.x / width * keys.size).toInt().coerceIn(0, keys.size - 1)
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                pressed = i
                performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
                Native.key(keys[i].qcode, true)
                invalidate()
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                if (pressed >= 0) Native.key(keys[pressed].qcode, false)
                pressed = -1
                invalidate()
            }
        }
        return true
    }
}
