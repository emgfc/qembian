/*
 * syborg-snd (syborg-audio v2): one sound device for every Symbian phone of the board.
 *
 * The guest forwards streams; this device does the rest on the host: 16 independent stream slots, each bound to a
 * guest ring (PCM or encoded bytes), resampled/decoded, mixed into one 48 kHz stereo voice of QEMU's audio backend
 * (-audiodev ...; the board passes it as the "audiodev" property). The backend's consumption is the clock: a slot's
 * READ_POS advances only as the host plays, so a guest pacing on READ_POS runs at real time
 * (docs/research/sound-device-architecture.md section 3).
 *
 * Global registers (word index, at 0xc000e000, irq 13):
 *   0 ID            0xc51d0012 (v1 was ...10; ...11 is syborg-gl's)
 *   1 CAPS          bit0 FFmpeg, bit1 MP3, bit2 AAC, bits 8..15 slot count
 *   2 CLOCK_LO      frames mixed since reset (48 kHz); 3 CLOCK_HI
 *   4 MASTER_VOLUME 0..256 (Q8)
 *   5 ROUTE         metadata (0 none, 1 earpiece, 2 speaker, 3 headset, 4 BT, 5 TV-out)
 *   6 INT_STATUS    bit k = slot k raised (write 1 to clear)
 *   7 INT_ENABLE    same bits
 * Slot k at 0x100 + k * 0x40 (word index within the bank):
 *   0 CTRL      write: bit0 START (READ_POS = WRITE_POS = 0, decoder (re)opened; refused with STATUS bit3 if the
 *               geometry below is invalid), bit1 STOP, bit2 PAUSE, bit3 RESUME, bit4 EOS (no data after WRITE_POS),
 *               bit5 FLUSH (decoder state reset)
 *   1 FORMAT    0 S16LE, 1 S8, 2 A-law, 3 MP3, 4 AAC ADTS (eAAC+ too), 5 S24 in 32-bit LE words (RAPIDO engine rings),
 *               6 S32LE; | 0x100 = encoded bytes come in 16-bit words byte-swapped (TI C55x DSP word order: the
 *               N95's MP3/AAC nodes)
 *   2 RATE      Hz (PCM, 1..192000)   3 CHANNELS  1 / 2 interleaved
 *   4 RING_ADDR guest physical    5 RING_SIZE bytes (16 .. 16 MB, inside RAM)
 *   6 WRITE_POS bytes written, monotonic modulo 2^32 (an advance beyond the free space, or backwards, is refused)
 *   7 READ_POS  bytes consumed (read only)
 *   8 WATERMARK interrupt when free bytes >= WATERMARK (once per crossing; re-armed by a WRITE_POS write)
 *   9 GAIN      0..256 (Q8)       10 CLASS   metadata (1 key tone, 2 system tone, 3 ringtone, 4 media)
 *  11 STATUS    bit0 running, bit1 EOS reached, bit2 underrun since last read (cleared by the read),
 *               bit3 error (invalid geometry, no decoder, decode or memory errors), bit4 watermark reached, bit5 paused
 *  12 FRAMES_LO frames of this stream played (stream rate); 13 FRAMES_HI
 *  14 PLANAR    0 = interleaved; N = stereo PCM comes in chunks of N left samples then N right samples (the RAPIDO
 *               engine's rings: N = 240); N * frame bytes <= 8192
 * A slot's interrupt: watermark crossed, EOS reached (ring drained after EOS), error.
 *
 * Host-side users (no guest involvement): syborg_snd_ring_written() / syborg_snd_pending() let the fake modem pace
 * RAPIDO's F3 0x34/0x35 on this clock (stream type t = slot t); syborg_snd_beep() plays the modem's key tones.
 *
 * The voice runs only while some slot has something to play (or a tone sounds): an idle phone costs no mixing.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "migration/vmstate.h"
#include "qemu/audio.h"
#include "qom/object.h"
#include "syborg.h"
#include <math.h>
#ifndef SYBORG_NO_FFMPEG             /* builds without FFmpeg (Android): encoded slots drain silently */
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#else
typedef struct AVCodecContext AVCodecContext;
typedef struct AVCodecParserContext AVCodecParserContext;
typedef struct AVPacket AVPacket;
typedef struct AVFrame AVFrame;
typedef struct SwrContext SwrContext;
#endif

OBJECT_DECLARE_SIMPLE_TYPE(SyborgAudioState, SYBORG_AUDIO)

#define NSLOTS 16
#define OUT_RATE 48000
#define DEC_FIFO (OUT_RATE / 2)           /* frames: half a second of decoded 48 kHz stereo */
#define CHUNK 256                         /* frames mixed per step */
#define STAGE 8192                        /* PCM bytes read from the ring at once */
#define RING_MIN 16
#define RING_MAX (16 * 1024 * 1024)
#define DEC_STEPS 16                      /* ring reads per decode call (bounds the work garbage input causes) */

enum { F_S16 = 0, F_S8 = 1, F_ALAW = 2, F_MP3 = 3, F_AAC = 4, F_S24_32 = 5, F_S32 = 6 };
enum { ST_RUN = 1, ST_EOS = 2, ST_UNDER = 4, ST_ERR = 8, ST_WM = 16, ST_PAUSE = 32 };

typedef struct {
    uint32_t swap16;                      /* FORMAT bit 8 */
    uint32_t planar;                      /* PLANAR: frames per channel chunk, 0 = interleaved */
    uint32_t format, rate, channels, ring_addr, ring_size, watermark, gain, klass, status;
    uint32_t eos_set, wm_armed;
    uint64_t wpos, rpos, frames;
    /* PCM resampler: phase in 1/65536 input frames, the last two input frames */
    uint32_t phase;
    int32_t prev[2], cur[2];
    uint32_t primed;
    /* PCM staging (not migrated: refilled from the ring) */
    uint8_t stage[STAGE];
    uint64_t stage_pos;
    uint32_t stage_len;
    /* encoded (not migrated: reopened at the next frame sync) */
    AVCodecContext *dec;
    AVCodecParserContext *parser;
    AVPacket *pkt;
    AVFrame *frame;
    SwrContext *swr;
    int16_t *fifo;                        /* decoded 48 kHz stereo frames */
    uint32_t fifo_head, fifo_frames;      /* circular */
    uint32_t dec_open, drained;           /* drained: parser, decoder and resampler flushed after EOS */
} Slot;

struct SyborgAudioState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    AudioBackend *audio_be;
    SWVoiceOut *voice;
    bool active;
    uint32_t master, route, int_status, int_enable;
    uint64_t clock;
    Slot slot[NSLOTS];
    /* host tone (the BB5 modem's own key tones, started by the fake modem): frames left, phase, Hz, Q8 gain */
    uint32_t tone_left, tone_len, tone_phase, tone_hz, tone_gain;
    /* mixed output the backend did not take yet (written first at the next callback) */
    int16_t pend[CHUNK * 2];
    uint32_t pend_bytes, pend_off;
    uint32_t nlog;
};

static SyborgAudioState *snd_singleton;

static void snd_update_irq(SyborgAudioState *s)
{
    qemu_set_irq(s->irq, (s->int_status & s->int_enable) != 0);
}

static void snd_raise(SyborgAudioState *s, int k, uint32_t bits)
{
    s->slot[k].status |= bits;
    s->int_status |= 1u << k;
    snd_update_irq(s);
}

static void snd_logf(SyborgAudioState *s, const char *fmt, int k, uint64_t a, uint64_t b)
{
    if (s->nlog++ < 64) {
        qemu_log_mask(LOG_GUEST_ERROR, "syborg-snd: slot %d: ", k);
        qemu_log_mask(LOG_GUEST_ERROR, fmt, a, b);
        qemu_log_mask(LOG_GUEST_ERROR, "\n");
    }
}

static bool snd_encoded(const Slot *sl)
{
    return sl->format == F_MP3 || sl->format == F_AAC;
}

/* invariant: rpos <= wpos <= rpos + ring_size */
static uint32_t snd_queued(const Slot *sl)
{
    return sl->wpos > sl->rpos ? (uint32_t)MIN(sl->wpos - sl->rpos, (uint64_t)sl->ring_size) : 0;
}

static uint32_t snd_frame_bytes(const Slot *sl)
{
    uint32_t ch = sl->channels == 2 ? 2 : 1;
    return (sl->format == F_S16 ? 2 : sl->format >= F_S24_32 ? 4 : 1) * ch;
}

/* the smallest amount of PCM that can be played: a frame, or a whole planar chunk */
static uint32_t snd_unit(const Slot *sl)
{
    if (snd_encoded(sl)) {
        return 1;
    }
    return sl->planar && sl->channels == 2 ? sl->planar * snd_frame_bytes(sl) : snd_frame_bytes(sl);
}

/* a slot's geometry before START: everything later arithmetic relies on */
static bool snd_slot_valid(const Slot *sl)
{
    if (sl->ring_size < RING_MIN || sl->ring_size > RING_MAX ||
        (uint64_t)sl->ring_addr + sl->ring_size > 0x100000000ull ||
        !address_space_access_valid(&address_space_memory, sl->ring_addr, sl->ring_size, false,
                                    MEMTXATTRS_UNSPECIFIED)) {
        return false;
    }
    if (snd_encoded(sl)) {
        return true;
    }
    if (sl->format > F_S32 || sl->format == F_MP3 || sl->format == F_AAC || !sl->rate || sl->rate > 192000) {
        return false;
    }
    if (sl->planar) {
        uint64_t unit = (uint64_t)sl->planar * snd_frame_bytes(sl);
        if (sl->channels != 2 || unit > STAGE || unit > sl->ring_size) {
            return false;
        }
    }
    return snd_frame_bytes(sl) <= sl->ring_size;
}

/* read n ring bytes at ring position `at` (wrapping); false on a memory error */
static bool snd_ring_read(Slot *sl, uint64_t at, uint8_t *buf, uint32_t n)
{
    while (n) {
        uint32_t off = at % sl->ring_size;
        uint32_t m = MIN(n, sl->ring_size - off);
        if (address_space_read(&address_space_memory, sl->ring_addr + off, MEMTXATTRS_UNSPECIFIED, buf, m)
            != MEMTX_OK) {
            return false;
        }
        buf += m;
        at += m;
        n -= m;
    }
    return true;
}

static void snd_fifo_put(Slot *sl, const int16_t *src, uint32_t n)
{
    for (uint32_t i = 0; i < n && sl->fifo_frames < DEC_FIFO; i++) {
        uint32_t t = (sl->fifo_head + sl->fifo_frames) % DEC_FIFO;
        sl->fifo[2 * t] = src[2 * i];
        sl->fifo[2 * t + 1] = src[2 * i + 1];
        sl->fifo_frames++;
    }
}

/* ---- encoded slots ---------------------------------------------------------------------------- */
#ifndef SYBORG_NO_FFMPEG
static void snd_dec_close(Slot *sl)
{
    avcodec_free_context(&sl->dec);
    if (sl->parser) {
        av_parser_close(sl->parser);
        sl->parser = NULL;
    }
    av_packet_free(&sl->pkt);
    av_frame_free(&sl->frame);
    swr_free(&sl->swr);
    sl->fifo_frames = sl->fifo_head = 0;
    sl->dec_open = sl->drained = 0;
}

static bool snd_dec_open(Slot *sl)
{
    enum AVCodecID id = sl->format == F_MP3 ? AV_CODEC_ID_MP3 : AV_CODEC_ID_AAC;
    const AVCodec *codec = avcodec_find_decoder(id);

    snd_dec_close(sl);
    if (!codec) {
        return false;
    }
    sl->dec = avcodec_alloc_context3(codec);
    sl->parser = av_parser_init(id);
    sl->pkt = av_packet_alloc();
    sl->frame = av_frame_alloc();
    if (!sl->dec || !sl->parser || !sl->pkt || !sl->frame || avcodec_open2(sl->dec, codec, NULL) < 0) {
        snd_dec_close(sl);
        return false;
    }
    if (!sl->fifo) {
        sl->fifo = g_malloc(DEC_FIFO * 4);
    }
    sl->dec_open = 1;
    return true;
}

/* decoded frames -> 48 kHz stereo S16 into the FIFO; false on a resampler error */
static bool snd_dec_output(Slot *sl)
{
    int16_t tmp[4096 * 2];
    while (avcodec_receive_frame(sl->dec, sl->frame) == 0) {
        if (!sl->swr) {
            AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
            if (swr_alloc_set_opts2(&sl->swr, &out, AV_SAMPLE_FMT_S16, OUT_RATE, &sl->frame->ch_layout,
                                    sl->frame->format, sl->frame->sample_rate, 0, NULL) < 0
                || swr_init(sl->swr) < 0) {
                swr_free(&sl->swr);
                return false;
            }
        }
        uint8_t *out = (uint8_t *)tmp;
        int room = MIN(4096u, DEC_FIFO - sl->fifo_frames);
        int n = swr_convert(sl->swr, &out, room, (const uint8_t **)sl->frame->extended_data,
                            sl->frame->nb_samples);
        if (n > 0) {
            snd_fifo_put(sl, tmp, n);
        }
    }
    return true;
}

static void snd_dec_send(SyborgAudioState *s, int k, AVPacket *pkt)
{
    Slot *sl = &s->slot[k];
    int r = avcodec_send_packet(sl->dec, pkt);
    if (r == AVERROR(EAGAIN)) {           /* output first, then the packet again */
        snd_dec_output(sl);
        r = avcodec_send_packet(sl->dec, pkt);
    }
    if (r < 0 && r != AVERROR_EOF && !(sl->status & ST_ERR)) {
        snd_raise(s, k, ST_ERR);          /* a damaged frame: reported once, decoding goes on */
    }
    if (!snd_dec_output(sl)) {
        snd_raise(s, k, ST_ERR);
    }
}

/* feed ring bytes to the parser/decoder until `want` frames are decoded or the ring is empty */
static void snd_dec_fill(SyborgAudioState *s, int k, uint32_t want)
{
    Slot *sl = &s->slot[k];
    uint8_t in[2048 + AV_INPUT_BUFFER_PADDING_SIZE];

    for (int step = 0; step < DEC_STEPS && sl->dec && sl->fifo_frames < want && sl->fifo_frames < DEC_FIFO / 2;
         step++) {
        uint32_t n = MIN(2048u, snd_queued(sl));
        if (sl->swap16) {
            n &= ~1u;                     /* whole words: an odd last byte waits for its partner */
            if (!n && sl->eos_set && snd_queued(sl) == 1) {
                n = 1;                    /* ... unless the stream ended on it */
            }
        }
        if (!n) {
            break;
        }
        if (!snd_ring_read(sl, sl->rpos, in, n)) {
            snd_raise(s, k, ST_ERR);
            sl->rpos = sl->wpos;
            return;
        }
        if (sl->swap16) {
            for (uint32_t i = 0; i + 1 < n; i += 2) {
                uint8_t t = in[i];
                in[i] = in[i + 1];
                in[i + 1] = t;
            }
        }
        memset(in + n, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        uint8_t *p = in;
        int left = n;
        while (left > 0) {
            uint8_t *data;
            int size;
            int used = av_parser_parse2(sl->parser, sl->dec, &data, &size, p, left, AV_NOPTS_VALUE,
                                        AV_NOPTS_VALUE, 0);
            if (used < 0) {
                snd_raise(s, k, ST_ERR);
                break;
            }
            p += used;
            left -= used;
            if (size) {
                sl->pkt->data = data;
                sl->pkt->size = size;
                snd_dec_send(s, k, sl->pkt);
            }
        }
        sl->rpos += n;
    }
    /* end of stream: flush the parser, then drain the decoder and the resampler, once */
    if (sl->dec && sl->eos_set && !snd_queued(sl) && !sl->drained && sl->fifo_frames < want) {
        uint8_t *data;
        int size;
        sl->drained = 1;
        av_parser_parse2(sl->parser, sl->dec, &data, &size, NULL, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (size) {
            sl->pkt->data = data;
            sl->pkt->size = size;
            snd_dec_send(s, k, sl->pkt);
        }
        snd_dec_send(s, k, NULL);
        if (sl->swr) {
            int16_t tmp[4096 * 2];
            uint8_t *out = (uint8_t *)tmp;
            int n = swr_convert(sl->swr, &out, MIN(4096u, DEC_FIFO - sl->fifo_frames), NULL, 0);
            if (n > 0) {
                snd_fifo_put(sl, tmp, n);
            }
        }
    }
}
#else
static void snd_dec_close(Slot *sl)
{
    sl->dec_open = sl->drained = 0;
    sl->fifo_frames = sl->fifo_head = 0;
}

static bool snd_dec_open(Slot *sl)
{
    sl->dec_open = 0;
    return false;
}

static void snd_dec_fill(SyborgAudioState *s, int k, uint32_t want)
{
}
#endif

/* ---- PCM input ------------------------------------------------------------------------------- */
static int16_t alaw_to_s16(uint8_t a)
{
    a ^= 0x55;
    int t = (a & 0x0f) << 4, seg = (a & 0x70) >> 4;
    t += seg ? 0x108 : 8;
    if (seg > 1) {
        t <<= seg - 1;
    }
    return (a & 0x80) ? t : -t;
}

static int32_t snd_sample(const Slot *sl, const uint8_t *b)
{
    switch (sl->format) {
    case F_S16: return (int16_t)(b[0] | b[1] << 8);
    case F_S8: return (int8_t)b[0] << 8;
    case F_ALAW: return alaw_to_s16(b[0]);
    case F_S24_32: return MAX(-32768, MIN(32767, (int32_t)ldl_le_p(b) >> 8));
    default: return (int32_t)ldl_le_p(b) >> 16;
    }
}

/* the staged copy of ring bytes [pos, pos + n): refilled from the ring when they are not staged */
static const uint8_t *snd_staged(SyborgAudioState *s, int k, uint64_t pos, uint32_t n)
{
    Slot *sl = &s->slot[k];
    if (pos < sl->stage_pos || pos + n > sl->stage_pos + sl->stage_len) {
        uint32_t len = (uint32_t)MIN((uint64_t)STAGE, sl->wpos > pos ? sl->wpos - pos : 0);
        uint32_t unit = snd_unit(sl);
        len -= len % unit;
        if (len < n || !snd_ring_read(sl, pos, sl->stage, len)) {
            return NULL;
        }
        sl->stage_pos = pos;
        sl->stage_len = len;
    }
    return sl->stage + (pos - sl->stage_pos);
}

/* next input frame of a PCM slot into f[2]; false when the ring has no whole frame (or chunk) */
static bool snd_pcm_next(SyborgAudioState *s, int k, int32_t f[2])
{
    Slot *sl = &s->slot[k];
    uint32_t fb = snd_frame_bytes(sl), unit = snd_unit(sl);

    if (sl->planar && sl->channels == 2) {
        /* chunks of N left samples then N right ones; the whole chunk must be in the ring */
        uint32_t ss = fb / 2, at = (uint32_t)(sl->rpos % unit);
        uint64_t base = sl->rpos - at;
        if (snd_queued(sl) + at < unit) {
            return false;
        }
        const uint8_t *c = snd_staged(s, k, base, unit);
        if (!c) {
            return false;
        }
        f[0] = snd_sample(sl, c + at);
        f[1] = snd_sample(sl, c + sl->planar * ss + at);
        sl->rpos += ss;
        if (at + ss >= sl->planar * ss) {
            sl->rpos += sl->planar * ss;          /* past the right half: the next chunk */
        }
        return true;
    }
    if (snd_queued(sl) < fb) {
        return false;
    }
    const uint8_t *b = snd_staged(s, k, sl->rpos, fb);
    if (!b) {
        return false;
    }
    sl->rpos += fb;
    f[0] = snd_sample(sl, b);
    f[1] = sl->channels == 2 ? snd_sample(sl, b + fb / 2) : f[0];
    return true;
}

/* ---- mixer ----------------------------------------------------------------------------------- */
/* mix up to n output frames of slot k into acc; returns frames produced */
static int snd_mix_slot(SyborgAudioState *s, int k, int32_t *acc, int n)
{
    Slot *sl = &s->slot[k];
    int i = 0;
    int32_t gain = sl->gain;

    if (snd_encoded(sl)) {
        if (!sl->dec_open) {
            /* no decoder: consume at about 128 kbit/s so the guest's player still ends on time */
            uint32_t q = snd_queued(sl);
            if (!q) {
                return 0;
            }
            uint32_t take = MIN(q, (uint32_t)n * 16000 / OUT_RATE + 1);
            sl->rpos += take;
            sl->frames += n;
            return n;
        }
        snd_dec_fill(s, k, n);
        int m = MIN((uint32_t)n, sl->fifo_frames);
        for (i = 0; i < m; i++) {
            uint32_t h = (sl->fifo_head + i) % DEC_FIFO;
            acc[2 * i] += sl->fifo[2 * h] * gain >> 8;
            acc[2 * i + 1] += sl->fifo[2 * h + 1] * gain >> 8;
        }
        sl->fifo_head = (sl->fifo_head + m) % DEC_FIFO;
        sl->fifo_frames -= m;
        sl->frames += m;
        return m;
    }
    uint32_t step = (uint32_t)(((uint64_t)sl->rate << 16) / OUT_RATE);
    if (!sl->primed) {
        if (!snd_pcm_next(s, k, sl->cur)) {
            return 0;
        }
        sl->prev[0] = sl->cur[0];
        sl->prev[1] = sl->cur[1];
        sl->primed = 1;
        sl->phase = 0;
    }
    for (i = 0; i < n; i++) {
        while (sl->phase >= 0x10000) {
            int32_t f[2];
            if (!snd_pcm_next(s, k, f)) {
                return i;            /* ring empty: keep phase, resume on the next frame */
            }
            sl->prev[0] = sl->cur[0];
            sl->prev[1] = sl->cur[1];
            sl->cur[0] = f[0];
            sl->cur[1] = f[1];
            sl->phase -= 0x10000;
            sl->frames++;
        }
        int32_t fr = sl->phase;
        int32_t l = sl->prev[0] + (int32_t)(((int64_t)(sl->cur[0] - sl->prev[0]) * fr) >> 16);
        int32_t r = sl->prev[1] + (int32_t)(((int64_t)(sl->cur[1] - sl->prev[1]) * fr) >> 16);
        acc[2 * i] += l * gain >> 8;
        acc[2 * i + 1] += r * gain >> 8;
        sl->phase += step;
    }
    return n;
}

/* nothing more will come out of the slot until the guest writes (PCM: less than a frame or chunk queued) */
static bool snd_drained(const Slot *sl)
{
    if (snd_encoded(sl)) {
        return !snd_queued(sl) && (!sl->dec_open || (!sl->fifo_frames && (!sl->eos_set || sl->drained)));
    }
    return snd_queued(sl) < snd_unit(sl);
}

/* slot bookkeeping after a mix step: watermark, EOS, underrun */
static void snd_slot_events(SyborgAudioState *s, int k, int produced, int want)
{
    Slot *sl = &s->slot[k];
    uint32_t free_b = sl->ring_size - snd_queued(sl);

    if (sl->wm_armed && sl->watermark && free_b >= sl->watermark) {
        sl->wm_armed = 0;
        snd_raise(s, k, ST_WM);
    }
    if (produced >= want) {
        return;
    }
    if (sl->eos_set && snd_drained(sl)) {
        sl->rpos = sl->wpos;               /* a partial frame or chunk at the end is dropped */
        sl->eos_set = 0;
        sl->status &= ~ST_RUN;
        snd_raise(s, k, ST_EOS);
    } else if (!sl->eos_set) {
        sl->status |= ST_UNDER;
    }
}

/* the voice runs while a slot can produce sound or reach its end, or a tone sounds */
static void snd_update_active(SyborgAudioState *s)
{
    bool any = s->tone_left != 0 || s->pend_bytes != 0;
    for (int k = 0; k < NSLOTS && !any; k++) {
        const Slot *sl = &s->slot[k];
        any = (sl->status & (ST_RUN | ST_PAUSE)) == ST_RUN && (!snd_drained(sl) || sl->eos_set);
    }
    if (any != s->active && s->voice) {
        s->active = any;
        audio_be_set_active_out(s->audio_be, s->voice, any);
    }
}

static void snd_mix_tone(SyborgAudioState *s, int32_t *acc, int n)
{
    static float table[1024];
    if (!table[256]) {
        for (int i = 0; i < 1024; i++) {
            table[i] = sinf(2 * (float)M_PI * i / 1024);
        }
    }
    uint32_t inc = (uint32_t)(((uint64_t)s->tone_hz << 32) / OUT_RATE);
    for (int i = 0; i < n && s->tone_left; i++, s->tone_left--) {
        /* sine with 4 ms linear fade in and out; phase in 2^-32 cycles */
        uint32_t pos = s->tone_len - s->tone_left, fade = OUT_RATE / 250;
        float env = MIN(1.0f, MIN((float)pos / fade, (float)s->tone_left / fade));
        int32_t v = (int32_t)(table[s->tone_phase >> 22] * env * 32767.0f * s->tone_gain / 256);
        s->tone_phase += inc;
        acc[2 * i] += v;
        acc[2 * i + 1] += v;
    }
}

static void snd_callback(void *opaque, int free)
{
    SyborgAudioState *s = opaque;
    int32_t acc[CHUNK * 2];

    /* what the backend did not take last time goes first */
    while (s->pend_bytes && free > 0) {
        size_t w = audio_be_write(s->audio_be, s->voice, (uint8_t *)s->pend + s->pend_off, s->pend_bytes);
        s->pend_off += w;
        s->pend_bytes -= w;
        free -= w;
        if (!w) {
            return;
        }
    }
    while (free >= 4) {
        int n = MIN(free / 4, CHUNK);
        memset(acc, 0, n * 8);
        for (int k = 0; k < NSLOTS; k++) {
            Slot *sl = &s->slot[k];
            if ((sl->status & (ST_RUN | ST_PAUSE)) != ST_RUN) {
                continue;
            }
            int got = snd_mix_slot(s, k, acc, n);
            snd_slot_events(s, k, got, n);
        }
        snd_mix_tone(s, acc, n);
        for (int i = 0; i < 2 * n; i++) {
            int32_t v = (int32_t)(((int64_t)acc[i] * s->master) >> 8);
            s->pend[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
        }
        s->clock += n;
        size_t w = audio_be_write(s->audio_be, s->voice, s->pend, n * 4);
        free -= w;
        if (w < (size_t)n * 4) {
            s->pend_off = w;                    /* kept for the next callback, not dropped */
            s->pend_bytes = n * 4 - w;
            break;
        }
    }
    snd_update_active(s);
}

/* ---- slot control ---------------------------------------------------------------------------- */
static void snd_slot_stop(Slot *sl)
{
    sl->status &= ~(ST_RUN | ST_PAUSE);
    sl->eos_set = 0;
    sl->fifo_frames = sl->fifo_head = 0;
    sl->stage_len = 0;
}

static void snd_slot_start(SyborgAudioState *s, int k)
{
    Slot *sl = &s->slot[k];

    snd_slot_stop(sl);
    sl->wpos = sl->rpos = sl->frames = 0;
    sl->primed = 0;
    sl->wm_armed = 1;
    snd_dec_close(sl);
    if (!snd_slot_valid(sl)) {
        sl->status = ST_ERR;
        snd_raise(s, k, ST_ERR);
        snd_logf(s, "invalid geometry (ring %" PRIu64 " bytes, planar %" PRIu64 "): not started", k,
                 sl->ring_size, sl->planar);
        return;
    }
    sl->status = ST_RUN;
    if (snd_encoded(sl) && !snd_dec_open(sl)) {
        sl->status |= ST_ERR;       /* played silently at the stream's bitrate, its end still reported */
        snd_logf(s, "no decoder for format %" PRIu64 " (%" PRIu64 ")", k, sl->format, sl->swap16);
    }
    snd_update_active(s);
}

static void snd_slot_ctrl(SyborgAudioState *s, int k, uint32_t v)
{
    Slot *sl = &s->slot[k];

    if (v & 2) {
        snd_slot_stop(sl);
    }
    if (v & 1) {
        snd_slot_start(s, k);
    }
    if (v & 4) {
        sl->status |= ST_PAUSE;
    }
    if (v & 8) {
        sl->status &= ~ST_PAUSE;
    }
    if (v & 16) {
        sl->eos_set = 1;
        sl->status &= ~ST_EOS;
    }
    if ((v & 32) && sl->dec_open) {
        snd_dec_open(sl);
    }
    snd_update_active(s);
}

/* the producer's position moved to `pos` (low 32 bits of the monotonic count): only forward, within the free
   space; false if refused */
static bool snd_write_pos(SyborgAudioState *s, int k, uint32_t pos)
{
    Slot *sl = &s->slot[k];
    uint32_t d = pos - (uint32_t)sl->wpos;
    uint32_t room = sl->ring_size - snd_queued(sl);
    if (d > room) {
        snd_logf(s, "write position refused (advance %" PRIu64 ", free %" PRIu64 ")", k, d, room);
        return false;
    }
    sl->wpos += d;
    sl->wm_armed = 1;
    sl->status &= ~ST_WM;
    return true;
}

static uint64_t snd_read(void *opaque, hwaddr offset, unsigned size)
{
    SyborgAudioState *s = opaque;

    if (offset >= 0x100) {
        int k = (offset - 0x100) / 0x40;
        if (k >= NSLOTS) {
            return 0;
        }
        Slot *sl = &s->slot[k];
        switch ((offset & 0x3f) >> 2) {
        case 0: return 0;
        case 1: return sl->format | sl->swap16 << 8;
        case 2: return sl->rate;
        case 3: return sl->channels;
        case 4: return sl->ring_addr;
        case 5: return sl->ring_size;
        case 6: return (uint32_t)sl->wpos;
        case 7: return (uint32_t)sl->rpos;
        case 8: return sl->watermark;
        case 9: return sl->gain;
        case 10: return sl->klass;
        case 11: {
            uint32_t v = sl->status;
            sl->status &= ~ST_UNDER;
            return v;
        }
        case 12: return (uint32_t)sl->frames;
        case 13: return (uint32_t)(sl->frames >> 32);
        case 14: return sl->planar;
        }
        return 0;
    }
    switch (offset >> 2) {
    case 0: return SYBORG_ID_AUDIO;
    case 1: {
        uint32_t caps = NSLOTS << 8;
#ifndef SYBORG_NO_FFMPEG
        caps |= 1 | (avcodec_find_decoder(AV_CODEC_ID_MP3) ? 2 : 0) | (avcodec_find_decoder(AV_CODEC_ID_AAC) ? 4 : 0);
#endif
        return caps;
    }
    case 2: return (uint32_t)s->clock;
    case 3: return (uint32_t)(s->clock >> 32);
    case 4: return s->master;
    case 5: return s->route;
    case 6: return s->int_status;
    case 7: return s->int_enable;
    case 8: return NSLOTS - 1;
    }
    return 0;
}

static void snd_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    SyborgAudioState *s = opaque;
    uint32_t v = value;

    if (offset >= 0x100) {
        int k = (offset - 0x100) / 0x40;
        if (k >= NSLOTS) {
            return;
        }
        Slot *sl = &s->slot[k];
        bool running = sl->status & ST_RUN;
        switch ((offset & 0x3f) >> 2) {
        case 0: snd_slot_ctrl(s, k, v); return;
        case 6:
            if (running) {
                snd_write_pos(s, k, v);
                snd_update_active(s);
            }
            return;
        case 8: sl->watermark = v; sl->wm_armed = 1; return;
        case 9: sl->gain = MIN(v, 1024u); return;
        case 10: sl->klass = v; return;
        }
        if (running) {             /* the geometry of a running stream does not change under the mixer */
            snd_logf(s, "geometry write +%" PRIu64 " while running ignored (%" PRIu64 ")", k, offset & 0x3f, v);
            return;
        }
        switch ((offset & 0x3f) >> 2) {
        case 1: sl->format = v & 0xff; sl->swap16 = (v >> 8) & 1; break;
        case 2: sl->rate = v; break;
        case 3: sl->channels = v == 1 ? 1 : 2; break;
        case 4: sl->ring_addr = v; break;
        case 5: sl->ring_size = v; break;
        case 14: sl->planar = v; break;
        }
        return;
    }
    switch (offset >> 2) {
    case 4: s->master = MIN(v, 1024u); break;
    case 5: s->route = v; break;
    case 6: s->int_status &= ~v; snd_update_irq(s); break;
    case 7: s->int_enable = v; snd_update_irq(s); break;
    }
}

/* ---- host-side API for the fake modem (RAPIDO F3 0x34 / 0x35) --------------------------------- */
/* the guest wrote up to ring offset `pos` of slot k: advance WRITE_POS; *target = the monotonic position that must
   be played before the write counts as consumed. false: the slot has no running ring. A write of exactly the whole
   free ring cannot be told from none (offsets only): the engine writes 19200 of 30720 bytes per block. */
bool syborg_snd_ring_written(int k, uint32_t pos, uint64_t *target)
{
    SyborgAudioState *s = snd_singleton;
    if (!s || k < 0 || k >= NSLOTS || !(s->slot[k].status & ST_RUN)) {
        return false;
    }
    Slot *sl = &s->slot[k];
    uint32_t size = sl->ring_size;
    uint32_t d = ((pos % size) + size - (uint32_t)(sl->wpos % size)) % size;
    uint32_t room = size - snd_queued(sl);
    if (d > room) {                 /* the engine lapped the player: drop the oldest whole units */
        uint32_t unit = snd_unit(sl), drop = d - room;
        drop = (drop + unit - 1) / unit * unit;
        sl->rpos += MIN(drop, snd_queued(sl));
        sl->primed = 0;
    }
    sl->wpos += d;
    if (sl->wpos > sl->rpos + size) {
        sl->rpos = sl->wpos - size;
    }
    sl->wm_armed = 1;
    *target = sl->wpos;
    snd_update_active(s);
    return true;
}

/* bytes of slot k still queued before `target` (0 = played) */
uint32_t syborg_snd_pending(int k, uint64_t target)
{
    SyborgAudioState *s = snd_singleton;
    if (!s || k < 0 || k >= NSLOTS || !(s->slot[k].status & ST_RUN)) {
        return 0;
    }
    return target > s->slot[k].rpos ? (uint32_t)MIN(target - s->slot[k].rpos, (uint64_t)UINT32_MAX) : 0;
}

uint32_t syborg_snd_ring_size(int k)
{
    SyborgAudioState *s = snd_singleton;
    return s && k >= 0 && k < NSLOTS ? s->slot[k].ring_size : 0;
}

/* a host-generated tone mixed over the slots (the fake modem's key tones) */
void syborg_snd_beep(uint32_t hz, uint32_t ms, uint32_t gain)
{
    SyborgAudioState *s = snd_singleton;
    if (!s || !hz || hz >= OUT_RATE / 2 || !ms || ms > 10000) {
        return;
    }
    s->tone_hz = hz;
    s->tone_len = s->tone_left = OUT_RATE / 1000 * ms;
    s->tone_phase = 0;
    s->tone_gain = MIN(gain, 256u);
    snd_update_active(s);
}

static const MemoryRegionOps snd_ops = {
    .read = snd_read,
    .write = snd_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void snd_realize(DeviceState *dev, Error **errp)
{
    SyborgAudioState *s = SYBORG_AUDIO(dev);

    if (snd_singleton) {
        error_setg(errp, "syborg-snd: one per machine");
        return;
    }
    if (!audio_be_check(&s->audio_be, errp)) {
        return;
    }
    memory_region_init_io(&s->iomem, OBJECT(s), &snd_ops, s, "syborg-snd", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    struct audsettings as = { .freq = OUT_RATE, .nchannels = 2, .fmt = AUDIO_FORMAT_S16, .big_endian = false };
    s->voice = audio_be_open_out(s->audio_be, NULL, "syborg-snd", s, snd_callback, &as);
    if (s->voice) {
        audio_be_set_volume_out_lr(s->audio_be, s->voice, false, 255, 255);
    }
    snd_singleton = s;
}

static void snd_unrealize(DeviceState *dev)
{
    SyborgAudioState *s = SYBORG_AUDIO(dev);
    for (int k = 0; k < NSLOTS; k++) {
        snd_dec_close(&s->slot[k]);
        g_free(s->slot[k].fifo);
        s->slot[k].fifo = NULL;
    }
    if (s->voice) {
        audio_be_close_out(s->audio_be, s->voice);
        s->voice = NULL;
    }
    if (snd_singleton == s) {
        snd_singleton = NULL;
    }
}

static void snd_reset(DeviceState *dev)
{
    SyborgAudioState *s = SYBORG_AUDIO(dev);

    for (int k = 0; k < NSLOTS; k++) {
        Slot *sl = &s->slot[k];
        snd_dec_close(sl);
        int16_t *fifo = sl->fifo;
        memset(sl, 0, offsetof(Slot, dec));
        sl->fifo = fifo;
        sl->gain = 256;
        sl->channels = 1;
        sl->rate = 8000;
    }
    s->master = 256;
    s->route = 0;
    s->int_status = s->int_enable = 0;
    s->clock = 0;
    s->tone_left = 0;
    s->pend_bytes = s->pend_off = 0;
    snd_update_active(s);
    snd_update_irq(s);
}

/* after a snapshot load: decoders are host state, reopened (the stream resumes at the next frame sync; decoded but
   unplayed samples of the saved machine are lost); loaded geometry is checked like a START */
static int snd_post_load(void *opaque, int version_id)
{
    SyborgAudioState *s = opaque;

    for (int k = 0; k < NSLOTS; k++) {
        Slot *sl = &s->slot[k];
        snd_dec_close(sl);
        sl->stage_len = 0;
        if (sl->status & ST_RUN) {
            if (!snd_slot_valid(sl) || sl->wpos < sl->rpos || sl->wpos - sl->rpos > sl->ring_size) {
                return -EINVAL;
            }
            if (snd_encoded(sl) && !snd_dec_open(sl)) {
                sl->status |= ST_ERR;
            }
        }
    }
    if (s->pend_bytes > sizeof(s->pend) || s->pend_off > sizeof(s->pend) - s->pend_bytes ||
        s->tone_left > s->tone_len) {
        return -EINVAL;
    }
    s->active = false;
    snd_update_active(s);
    snd_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_snd_slot = {
    .name = "syborg-snd-slot",
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(format, Slot),
        VMSTATE_UINT32(swap16, Slot),
        VMSTATE_UINT32(planar, Slot),
        VMSTATE_UINT32(rate, Slot),
        VMSTATE_UINT32(channels, Slot),
        VMSTATE_UINT32(ring_addr, Slot),
        VMSTATE_UINT32(ring_size, Slot),
        VMSTATE_UINT32(watermark, Slot),
        VMSTATE_UINT32(gain, Slot),
        VMSTATE_UINT32(klass, Slot),
        VMSTATE_UINT32(status, Slot),
        VMSTATE_UINT32(eos_set, Slot),
        VMSTATE_UINT32(wm_armed, Slot),
        VMSTATE_UINT64(wpos, Slot),
        VMSTATE_UINT64(rpos, Slot),
        VMSTATE_UINT64(frames, Slot),
        VMSTATE_UINT32(phase, Slot),
        VMSTATE_INT32_ARRAY(prev, Slot, 2),
        VMSTATE_INT32_ARRAY(cur, Slot, 2),
        VMSTATE_UINT32(primed, Slot),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_snd = {
    .name = "syborg-audio",
    .version_id = 4,
    .minimum_version_id = 4,
    .post_load = snd_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(master, SyborgAudioState),
        VMSTATE_UINT32(route, SyborgAudioState),
        VMSTATE_UINT32(int_status, SyborgAudioState),
        VMSTATE_UINT32(int_enable, SyborgAudioState),
        VMSTATE_UINT64(clock, SyborgAudioState),
        VMSTATE_STRUCT_ARRAY(slot, SyborgAudioState, NSLOTS, 3, vmstate_snd_slot, Slot),
        VMSTATE_UINT32(tone_left, SyborgAudioState),
        VMSTATE_UINT32(tone_len, SyborgAudioState),
        VMSTATE_UINT32(tone_phase, SyborgAudioState),
        VMSTATE_UINT32(tone_hz, SyborgAudioState),
        VMSTATE_UINT32(tone_gain, SyborgAudioState),
        VMSTATE_INT16_ARRAY(pend, SyborgAudioState, CHUNK * 2),
        VMSTATE_UINT32(pend_bytes, SyborgAudioState),
        VMSTATE_UINT32(pend_off, SyborgAudioState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property snd_properties[] = {
    DEFINE_AUDIO_PROPERTIES(SyborgAudioState, audio_be),
};

static void snd_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = snd_realize;
    dc->unrealize = snd_unrealize;
    device_class_set_legacy_reset(dc, snd_reset);
    dc->vmsd = &vmstate_snd;
    device_class_set_props(dc, snd_properties);
}

static const TypeInfo snd_info = {
    .name = TYPE_SYBORG_AUDIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SyborgAudioState),
    .class_init = snd_class_init,
};

static void snd_register_types(void)
{
    type_register_static(&snd_info);
}

type_init(snd_register_types)
