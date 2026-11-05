/*
 * Syborg audio output for the N95 hybrid: the "speaker" behind our fake DSP.
 *
 * On the N95 the DSP decodes audio and drives the codec itself; decoded sound never comes back
 * to the ARM side. Our DSP-bridge stand-in (src/n95compat/stubs/ehwa.cpp) therefore hands the
 * streams it receives to this device, which plays them through QEMU's audio backend
 * (-audiodev ...; the board passes it on as the "audiodev" property).
 *
 * Registers (word index, at 0xc000e000, irq 13):
 *   0 ID            0xc51d0010
 *   1 CTRL          write: bit0 START (open the voice with FORMAT/RATE/CHANNELS and play),
 *                   bit1 STOP (drop queued buffers, silence), bit2 END (raise EOS once the
 *                   queue has played out)
 *   2 FORMAT        0 = PCM signed 16-bit little endian, 1 = tone generator,
 *                   2 = MP3 stream, 3 = AAC stream (ADTS, or raw with CONFIG = AudioSpecificConfig);
 *                   encoded formats are decoded with FFmpeg (libavcodec) and played at 44.1 kHz
 *                   stereo; a buffer counts as done once the decoder has taken all its bytes
 *   3 RATE          sample rate in Hz (PCM; tones use 48000)
 *   4 CHANNELS      1 or 2 (PCM)
 *   5 BUF_ADDR      guest physical address of the next PCM buffer
 *   6 BUF_LEN       its length in bytes
 *   7 SUBMIT        write: queue {BUF_ADDR, BUF_LEN}
 *   8 STATUS        bit0 playing, bit1 end of stream reached
 *   9 INT_ENABLE    bit0 buffer done, bit1 end of stream
 *  10 INT_STATUS    same bits, write 1 to clear
 *  11 TONE_FREQ1    Hz (tone generator)
 *  12 TONE_FREQ2    Hz, 0 = single tone (DTMF-style dual tones otherwise)
 *  13 TONE_MS       tone length in ms
 *  14 VOLUME        0..255
 *  15 QUEUE_FREE    free buffer slots (read)
 *  16 DONE_COUNT    buffers completed since reset (read)
 *  17 CONFIG_ADDR   guest physical address of codec config (raw AAC AudioSpecificConfig)
 *  18 CONFIG_LEN    its length (0 = none)
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
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

OBJECT_DECLARE_SIMPLE_TYPE(SyborgAudioState, SYBORG_AUDIO)

#define QLEN 16
#define TONE_RATE 48000
#define DEC_RATE 44100
#define FIFO_BYTES (DEC_RATE * 4)   /* one second of 16-bit stereo */

typedef struct {
    uint32_t addr, len, pos;
} AudioBuf;

struct SyborgAudioState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    AudioBackend *audio_be;
    SWVoiceOut *voice;
    uint32_t format, rate, channels, buf_addr, buf_len, status, int_enable, int_status;
    uint32_t tone_freq1, tone_freq2, tone_ms, volume, done_count, eos_pending;
    uint32_t q_head, q_count;
    AudioBuf q[QLEN];
    uint32_t tone_left;       /* tone samples still to generate */
    uint32_t tone_phase;      /* sample index within the tone */
    uint32_t config_addr, config_len;
    /* encoded formats: FFmpeg decoder state, decoded PCM waiting for the backend */
    AVCodecContext *dec;
    AVCodecParserContext *parser;
    AVPacket *pkt;
    AVFrame *frame;
    SwrContext *swr;
    uint8_t *fifo;
    uint32_t fifo_len;
};

static bool syborg_audio_encoded(SyborgAudioState *s)
{
    return s->format == 2 || s->format == 3;
}

static void syborg_audio_dec_close(SyborgAudioState *s)
{
    avcodec_free_context(&s->dec);
    if (s->parser) {
        av_parser_close(s->parser);
        s->parser = NULL;
    }
    av_packet_free(&s->pkt);
    av_frame_free(&s->frame);
    swr_free(&s->swr);
    s->fifo_len = 0;
}

static bool syborg_audio_dec_open(SyborgAudioState *s)
{
    enum AVCodecID id = s->format == 2 ? AV_CODEC_ID_MP3 : AV_CODEC_ID_AAC;
    const AVCodec *codec = avcodec_find_decoder(id);

    syborg_audio_dec_close(s);
    if (!codec) {
        qemu_log_mask(LOG_UNIMP, "syborg-audio: no FFmpeg decoder for format %u\n", s->format);
        return false;
    }
    s->dec = avcodec_alloc_context3(codec);
    if (s->config_len && s->config_len < 64) {
        s->dec->extradata = av_mallocz(s->config_len + AV_INPUT_BUFFER_PADDING_SIZE);
        s->dec->extradata_size = s->config_len;
        address_space_read(&address_space_memory, s->config_addr, MEMTXATTRS_UNSPECIFIED,
                           s->dec->extradata, s->config_len);
    }
    if (avcodec_open2(s->dec, codec, NULL) < 0) {
        syborg_audio_dec_close(s);
        return false;
    }
    if (!s->config_len) {
        s->parser = av_parser_init(id);     /* ADTS / MP3 frame sync */
    }
    s->pkt = av_packet_alloc();
    s->frame = av_frame_alloc();
    if (!s->fifo) {
        s->fifo = g_malloc(FIFO_BYTES);
    }
    s->fifo_len = 0;
    return true;
}

/* decoded frame -> 44.1 kHz stereo S16 into the FIFO */
static void syborg_audio_dec_output(SyborgAudioState *s)
{
    while (avcodec_receive_frame(s->dec, s->frame) == 0) {
        if (!s->swr) {
            AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
            if (swr_alloc_set_opts2(&s->swr, &out, AV_SAMPLE_FMT_S16, DEC_RATE,
                                    &s->frame->ch_layout, s->frame->format,
                                    s->frame->sample_rate, 0, NULL) < 0 || swr_init(s->swr) < 0) {
                swr_free(&s->swr);
                return;
            }
        }
        int room = (FIFO_BYTES - s->fifo_len) / 4;
        uint8_t *out = s->fifo + s->fifo_len;
        int n = swr_convert(s->swr, &out, room, (const uint8_t **)s->frame->extended_data,
                            s->frame->nb_samples);
        if (n > 0) {
            s->fifo_len += n * 4;
        }
    }
}

static void syborg_audio_decode_more(SyborgAudioState *s, uint32_t want)
{
    uint8_t in[4096 + AV_INPUT_BUFFER_PADDING_SIZE];

    while (s->fifo_len < want && s->fifo_len < FIFO_BYTES / 2 && s->q_count && s->dec) {
        AudioBuf *b = &s->q[s->q_head];
        uint32_t n = MIN(4096u, b->len - b->pos);
        memset(in + n, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        address_space_read(&address_space_memory, b->addr + b->pos, MEMTXATTRS_UNSPECIFIED, in, n);
        uint8_t *p = in;
        int left = n;
        if (s->parser) {
            while (left > 0) {
                uint8_t *data;
                int size;
                int used = av_parser_parse2(s->parser, s->dec, &data, &size, p, left,
                                            AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
                if (used < 0) {
                    break;
                }
                p += used;
                left -= used;
                if (size) {
                    s->pkt->data = data;
                    s->pkt->size = size;
                    if (avcodec_send_packet(s->dec, s->pkt) == 0) {
                        syborg_audio_dec_output(s);
                    }
                }
            }
        } else {
            /* raw access unit per buffer (AAC with config) */
            s->pkt->data = in;
            s->pkt->size = n;
            if (avcodec_send_packet(s->dec, s->pkt) == 0) {
                syborg_audio_dec_output(s);
            }
        }
        b->pos += n;
        if (b->pos >= b->len) {
            s->q_head = (s->q_head + 1) % QLEN;
            s->q_count--;
            s->done_count++;
            s->int_status |= 1;
        }
    }
    qemu_set_irq(s->irq, (s->int_status & s->int_enable) != 0);
}

static void syborg_audio_update_irq(SyborgAudioState *s)
{
    qemu_set_irq(s->irq, (s->int_status & s->int_enable) != 0);
}

static void syborg_audio_finish(SyborgAudioState *s)
{
    /* queue drained (or tone done) and the client said END: report end of stream */
    s->status = (s->status & ~1u) | 2u;
    s->eos_pending = 0;
    s->int_status |= 2;
    syborg_audio_update_irq(s);
    audio_be_set_active_out(s->audio_be, s->voice, false);
}

/* fill up to `free` bytes of output */
static void syborg_audio_callback(void *opaque, int free)
{
    SyborgAudioState *s = opaque;
    uint8_t tmp[4096];

    while (free > 0) {
        size_t n = MIN((size_t)free, sizeof(tmp));
        if (syborg_audio_encoded(s)) {
            syborg_audio_decode_more(s, n);
            n = MIN(n, (size_t)s->fifo_len) & ~3u;
            if (!n) {
                break;
            }
            size_t w = audio_be_write(s->audio_be, s->voice, s->fifo, n);
            memmove(s->fifo, s->fifo + w, s->fifo_len - w);
            s->fifo_len -= w;
            if (w < n) {
                break;
            }
            free -= w;
            continue;
        }
        if (s->format == 1) {
            if (!s->tone_left) {
                break;
            }
            int16_t *o = (int16_t *)tmp;
            size_t k = MIN(n / 2, s->tone_left);
            double amp = 9000.0 * s->volume / 255.0;
            for (size_t i = 0; i < k; i++, s->tone_phase++) {
                double t = (double)s->tone_phase / TONE_RATE, v;
                v = sin(2 * M_PI * s->tone_freq1 * t);
                if (s->tone_freq2) {
                    v = (v + sin(2 * M_PI * s->tone_freq2 * t)) / 2;
                }
                o[i] = (int16_t)(v * amp);
            }
            s->tone_left -= k;
            n = k * 2;
        } else {
            if (!s->q_count) {
                break;
            }
            AudioBuf *b = &s->q[s->q_head];
            n = MIN(n, (size_t)(b->len - b->pos));
            address_space_read(&address_space_memory, b->addr + b->pos,
                               MEMTXATTRS_UNSPECIFIED, tmp, n);
            b->pos += n;
            if (b->pos >= b->len) {
                s->q_head = (s->q_head + 1) % QLEN;
                s->q_count--;
                s->done_count++;
                s->int_status |= 1;
                syborg_audio_update_irq(s);
            }
        }
        size_t w = audio_be_write(s->audio_be, s->voice, tmp, n);
        if (w < n) {
            /* backend full: put back what was not taken (PCM) or the samples (tone) */
            if (s->format == 1) {
                s->tone_left += (n - w) / 2;
                s->tone_phase -= (n - w) / 2;
            } else if (s->q_count || (n - w)) {
                /* rare: rewind within the current buffer when it was not completed */
                AudioBuf *b = &s->q[s->q_head];
                if (b->pos >= n - w) {
                    b->pos -= n - w;
                }
            }
            break;
        }
        free -= w;
    }
    bool idle = s->format == 1 ? !s->tone_left
              : syborg_audio_encoded(s) ? (!s->q_count && !s->fifo_len) : !s->q_count;
    if (idle && (s->eos_pending || s->format == 1) && (s->status & 1)) {
        syborg_audio_finish(s);
    }
}

static void syborg_audio_start(SyborgAudioState *s)
{
    struct audsettings as = {
        .freq = s->format == 1 ? TONE_RATE : syborg_audio_encoded(s) ? DEC_RATE
              : (s->rate ? s->rate : 8000),
        .nchannels = s->format == 1 ? 1 : syborg_audio_encoded(s) ? 2 : (s->channels == 2 ? 2 : 1),
        .fmt = AUDIO_FORMAT_S16,
        .big_endian = false,
    };
    s->voice = audio_be_open_out(s->audio_be, s->voice, "syborg-audio", s,
                                 syborg_audio_callback, &as);
    audio_be_set_volume_out_lr(s->audio_be, s->voice, false, 255, 255);
    if (s->format == 1) {
        s->tone_left = (uint64_t)s->tone_ms * TONE_RATE / 1000;
        s->tone_phase = 0;
    }
    if (syborg_audio_encoded(s) && !syborg_audio_dec_open(s)) {
        return;
    }
    s->status = 1;
    audio_be_set_active_out(s->audio_be, s->voice, true);
    qemu_log_mask(LOG_UNIMP, "syborg-audio: start format %u rate %u ch %u tone %u/%u Hz %u ms\n",
                  s->format, as.freq, as.nchannels, s->tone_freq1, s->tone_freq2, s->tone_ms);
}

static void syborg_audio_stop(SyborgAudioState *s)
{
    s->q_head = s->q_count = 0;
    s->tone_left = 0;
    s->eos_pending = 0;
    s->status &= ~1u;
    s->fifo_len = 0;
    if (s->voice) {
        audio_be_set_active_out(s->audio_be, s->voice, false);
    }
}

static uint64_t syborg_audio_read(void *opaque, hwaddr offset, unsigned size)
{
    SyborgAudioState *s = opaque;

    switch (offset >> 2) {
    case 0: return SYBORG_ID_AUDIO;
    case 2: return s->format;
    case 3: return s->rate;
    case 4: return s->channels;
    case 5: return s->buf_addr;
    case 6: return s->buf_len;
    case 8: return s->status;
    case 9: return s->int_enable;
    case 10: return s->int_status;
    case 11: return s->tone_freq1;
    case 12: return s->tone_freq2;
    case 13: return s->tone_ms;
    case 14: return s->volume;
    case 15: return QLEN - s->q_count;
    case 16: return s->done_count;
    case 17: return s->config_addr;
    case 18: return s->config_len;
    default: return 0;
    }
}

static void syborg_audio_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    SyborgAudioState *s = opaque;

    switch (offset >> 2) {
    case 1:
        if (value & 2) {
            syborg_audio_stop(s);
        }
        if (value & 1) {
            syborg_audio_start(s);
        }
        if (value & 4) {
            s->eos_pending = 1;
            if (s->format != 1 && !s->q_count && !s->fifo_len && (s->status & 1)) {
                syborg_audio_finish(s);
            }
        }
        break;
    case 2: s->format = value; break;
    case 3: s->rate = value; break;
    case 4: s->channels = value; break;
    case 5: s->buf_addr = value; break;
    case 6: s->buf_len = value; break;
    case 7:
        if (s->q_count < QLEN && s->buf_len) {
            AudioBuf *b = &s->q[(s->q_head + s->q_count) % QLEN];
            b->addr = s->buf_addr;
            b->len = s->buf_len;
            b->pos = 0;
            s->q_count++;
            s->status &= ~2u;
        }
        break;
    case 9: s->int_enable = value & 3; syborg_audio_update_irq(s); break;
    case 10: s->int_status &= ~value; syborg_audio_update_irq(s); break;
    case 11: s->tone_freq1 = value; break;
    case 12: s->tone_freq2 = value; break;
    case 13: s->tone_ms = value; break;
    case 14: s->volume = value & 0xff; break;
    case 17: s->config_addr = value; break;
    case 18: s->config_len = value; break;
    default: break;
    }
}

static const MemoryRegionOps syborg_audio_ops = {
    .read = syborg_audio_read,
    .write = syborg_audio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void syborg_audio_realize(DeviceState *dev, Error **errp)
{
    SyborgAudioState *s = SYBORG_AUDIO(dev);

    if (!audio_be_check(&s->audio_be, errp)) {
        return;
    }
    memory_region_init_io(&s->iomem, OBJECT(s), &syborg_audio_ops, s, "syborg-audio", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    s->volume = 200;
    s->rate = 8000;
    s->channels = 1;
}

static const VMStateDescription vmstate_syborg_audio = {
    .name = "syborg-audio",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(format, SyborgAudioState),
        VMSTATE_UINT32(rate, SyborgAudioState),
        VMSTATE_UINT32(channels, SyborgAudioState),
        VMSTATE_UINT32(status, SyborgAudioState),
        VMSTATE_UINT32(int_enable, SyborgAudioState),
        VMSTATE_UINT32(int_status, SyborgAudioState),
        VMSTATE_UINT32(volume, SyborgAudioState),
        VMSTATE_UINT32(done_count, SyborgAudioState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property syborg_audio_properties[] = {
    DEFINE_AUDIO_PROPERTIES(SyborgAudioState, audio_be),
};

static void syborg_audio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = syborg_audio_realize;
    dc->vmsd = &vmstate_syborg_audio;
    device_class_set_props(dc, syborg_audio_properties);
}

static const TypeInfo syborg_audio_info = {
    .name = TYPE_SYBORG_AUDIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SyborgAudioState),
    .class_init = syborg_audio_class_init,
};

static void syborg_audio_register_types(void)
{
    type_register_static(&syborg_audio_info);
}

type_init(syborg_audio_register_types)
