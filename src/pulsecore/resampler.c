/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <string.h>

#include <samplerate.h>
#include <liboil/liboilfuncs.h>
#include <liboil/liboil.h>

#include <pulse/xmalloc.h>

#include <pulsecore/sconv.h>
#include <pulsecore/log.h>

#include "resampler.h"

struct pa_resampler {
    pa_resample_method_t resample_method;
    pa_sample_spec i_ss, o_ss;
    pa_channel_map i_cm, o_cm;
    size_t i_fz, o_fz;
    pa_mempool *mempool;

    void (*impl_free)(pa_resampler *r);
    void (*impl_update_input_rate)(pa_resampler *r, uint32_t rate);
    void (*impl_run)(pa_resampler *r, const pa_memchunk *in, pa_memchunk *out);
    void *impl_data;
};

struct impl_libsamplerate {
    pa_memchunk buf1, buf2, buf3, buf4;
    unsigned buf1_samples, buf2_samples, buf3_samples, buf4_samples;

    pa_convert_to_float32ne_func_t to_float32ne_func;
    pa_convert_from_float32ne_func_t from_float32ne_func;
    SRC_STATE *src_state;

    int map_table[PA_CHANNELS_MAX][PA_CHANNELS_MAX];
    int map_required;
};

struct impl_trivial {
    unsigned o_counter;
    unsigned i_counter;
};

static int libsamplerate_init(pa_resampler*r);
static int trivial_init(pa_resampler*r);

pa_resampler* pa_resampler_new(
        pa_mempool *pool,
        const pa_sample_spec *a,
        const pa_channel_map *am,
        const pa_sample_spec *b,
        const pa_channel_map *bm,
        pa_resample_method_t resample_method) {

    pa_resampler *r = NULL;

    assert(pool);
    assert(a);
    assert(b);
    assert(pa_sample_spec_valid(a));
    assert(pa_sample_spec_valid(b));
    assert(resample_method != PA_RESAMPLER_INVALID);

    r = pa_xnew(pa_resampler, 1);
    r->impl_data = NULL;
    r->mempool = pool;
    r->resample_method = resample_method;

    r->impl_free = NULL;
    r->impl_update_input_rate = NULL;
    r->impl_run = NULL;

    /* Fill sample specs */
    r->i_ss = *a;
    r->o_ss = *b;

    if (am)
        r->i_cm = *am;
    else
        pa_channel_map_init_auto(&r->i_cm, r->i_ss.channels, PA_CHANNEL_MAP_DEFAULT);

    if (bm)
        r->o_cm = *bm;
    else
        pa_channel_map_init_auto(&r->o_cm, r->o_ss.channels, PA_CHANNEL_MAP_DEFAULT);

    r->i_fz = pa_frame_size(a);
    r->o_fz = pa_frame_size(b);

    /* Choose implementation */
    if (a->channels != b->channels ||
        a->format != b->format ||
        !pa_channel_map_equal(&r->i_cm, &r->o_cm) ||
        resample_method != PA_RESAMPLER_TRIVIAL) {

        /* Use the libsamplerate based resampler for the complicated cases */
        if (resample_method == PA_RESAMPLER_TRIVIAL)
            r->resample_method = PA_RESAMPLER_SRC_ZERO_ORDER_HOLD;

        if (libsamplerate_init(r) < 0)
            goto fail;

    } else {
        /* Use our own simple non-fp resampler for the trivial cases and when the user selects it */
        if (trivial_init(r) < 0)
            goto fail;
    }

    return r;

fail:
    if (r)
        pa_xfree(r);

    return NULL;
}

void pa_resampler_free(pa_resampler *r) {
    assert(r);

    if (r->impl_free)
        r->impl_free(r);

    pa_xfree(r);
}

void pa_resampler_set_input_rate(pa_resampler *r, uint32_t rate) {
    assert(r);
    assert(rate > 0);

    if (r->i_ss.rate == rate)
        return;

    r->i_ss.rate = rate;

    if (r->impl_update_input_rate)
        r->impl_update_input_rate(r, rate);
}

void pa_resampler_run(pa_resampler *r, const pa_memchunk *in, pa_memchunk *out) {
    assert(r && in && out && r->impl_run);

    r->impl_run(r, in, out);
}

size_t pa_resampler_request(pa_resampler *r, size_t out_length) {
    assert(r);

    return (((out_length / r->o_fz)*r->i_ss.rate)/r->o_ss.rate) * r->i_fz;
}

pa_resample_method_t pa_resampler_get_method(pa_resampler *r) {
    assert(r);
    return r->resample_method;
}

static const char * const resample_methods[] = {
    "src-sinc-best-quality",
    "src-sinc-medium-quality",
    "src-sinc-fastest",
    "src-zero-order-hold",
    "src-linear",
    "trivial"
};

const char *pa_resample_method_to_string(pa_resample_method_t m) {

    if (m < 0 || m >= PA_RESAMPLER_MAX)
        return NULL;

    return resample_methods[m];
}

pa_resample_method_t pa_parse_resample_method(const char *string) {
    pa_resample_method_t m;

    assert(string);

    for (m = 0; m < PA_RESAMPLER_MAX; m++)
        if (!strcmp(string, resample_methods[m]))
            return m;

    return PA_RESAMPLER_INVALID;
}


/*** libsamplerate based implementation ***/

static void libsamplerate_free(pa_resampler *r) {
    struct impl_libsamplerate *u;

    assert(r);
    assert(r->impl_data);

    u = r->impl_data;

    if (u->src_state)
        src_delete(u->src_state);

    if (u->buf1.memblock)
        pa_memblock_unref(u->buf1.memblock);
    if (u->buf2.memblock)
        pa_memblock_unref(u->buf2.memblock);
    if (u->buf3.memblock)
        pa_memblock_unref(u->buf3.memblock);
    if (u->buf4.memblock)
        pa_memblock_unref(u->buf4.memblock);
    pa_xfree(u);
}

static void calc_map_table(pa_resampler *r) {
    struct impl_libsamplerate *u;
    unsigned oc;
    assert(r);
    assert(r->impl_data);

    u = r->impl_data;

    if (!(u->map_required = (!pa_channel_map_equal(&r->i_cm, &r->o_cm) || r->i_ss.channels != r->o_ss.channels)))
        return;

    for (oc = 0; oc < r->o_ss.channels; oc++) {
        unsigned ic, i = 0;

        for (ic = 0; ic < r->i_ss.channels; ic++) {
            pa_channel_position_t a, b;

            a = r->i_cm.map[ic];
            b = r->o_cm.map[oc];

            if (a == b ||
                (a == PA_CHANNEL_POSITION_MONO && b == PA_CHANNEL_POSITION_LEFT) ||
                (a == PA_CHANNEL_POSITION_MONO && b == PA_CHANNEL_POSITION_RIGHT) ||
                (a == PA_CHANNEL_POSITION_LEFT && b == PA_CHANNEL_POSITION_MONO) ||
                (a == PA_CHANNEL_POSITION_RIGHT && b == PA_CHANNEL_POSITION_MONO))

                u->map_table[oc][i++] = ic;
        }

        /* Add an end marker */
        if (i < PA_CHANNELS_MAX)
            u->map_table[oc][i] = -1;
    }
}

static pa_memchunk* convert_to_float(pa_resampler *r, pa_memchunk *input) {
    struct impl_libsamplerate *u;
    unsigned n_samples;
    void *src, *dst;

    assert(r);
    assert(input);
    assert(input->memblock);

    assert(r->impl_data);
    u = r->impl_data;

    /* Convert the incoming sample into floats and place them in buf1 */

    if (!u->to_float32ne_func || !input->length)
        return input;

    n_samples = (input->length / r->i_fz) * r->i_ss.channels;

    if (!u->buf1.memblock || u->buf1_samples < n_samples) {
        if (u->buf1.memblock)
            pa_memblock_unref(u->buf1.memblock);

        u->buf1_samples = n_samples;
        u->buf1.memblock = pa_memblock_new(r->mempool, u->buf1.length = sizeof(float) * n_samples);
        u->buf1.index = 0;
    }

    src = (uint8_t*) pa_memblock_acquire(input->memblock) + input->index;
    dst = (uint8_t*) pa_memblock_acquire(u->buf1.memblock);
    u->to_float32ne_func(n_samples, src, dst);
    pa_memblock_release(input->memblock);
    pa_memblock_release(u->buf1.memblock);

    u->buf1.length = sizeof(float) * n_samples;

    return &u->buf1;
}

static pa_memchunk *remap_channels(pa_resampler *r, pa_memchunk *input) {
    struct impl_libsamplerate *u;
    unsigned n_samples, n_frames;
    int i_skip, o_skip;
    unsigned oc;
    float *src, *dst;

    assert(r);
    assert(input);
    assert(input->memblock);

    assert(r->impl_data);
    u = r->impl_data;

    /* Remap channels and place the result int buf2 */

    if (!u->map_required || !input->length)
        return input;

    n_samples = input->length / sizeof(float);
    n_frames = n_samples / r->o_ss.channels;

    if (!u->buf2.memblock || u->buf2_samples < n_samples) {
        if (u->buf2.memblock)
            pa_memblock_unref(u->buf2.memblock);

        u->buf2_samples = n_samples;
        u->buf2.memblock = pa_memblock_new(r->mempool, u->buf2.length = sizeof(float) * n_samples);
        u->buf2.index = 0;
    }

    src = (float*) ((uint8_t*) pa_memblock_acquire(input->memblock) + input->index);
    dst = (float*) pa_memblock_acquire(u->buf2.memblock);

    memset(dst, 0, n_samples * sizeof(float));

    o_skip = sizeof(float) * r->o_ss.channels;
    i_skip = sizeof(float) * r->i_ss.channels;

    for (oc = 0; oc < r->o_ss.channels; oc++) {
        unsigned i;
        static const float one = 1.0;

        for (i = 0; i < PA_CHANNELS_MAX && u->map_table[oc][i] >= 0; i++)
            oil_vectoradd_f32(
                dst + oc, o_skip,
                dst + oc, o_skip,
                src + u->map_table[oc][i], i_skip,
                n_frames,
                &one, &one);
    }

    pa_memblock_release(input->memblock);
    pa_memblock_release(u->buf2.memblock);

    u->buf2.length = n_frames * sizeof(float) * r->o_ss.channels;

    return &u->buf2;
}

static pa_memchunk *resample(pa_resampler *r, pa_memchunk *input) {
    struct impl_libsamplerate *u;
    SRC_DATA data;
    unsigned in_n_frames, in_n_samples;
    unsigned out_n_frames, out_n_samples;
    int ret;

    assert(r);
    assert(input);
    assert(r->impl_data);
    u = r->impl_data;

    /* Resample the data and place the result in buf3 */

    if (!u->src_state || !input->length)
        return input;

    in_n_samples = input->length / sizeof(float);
    in_n_frames = in_n_samples * r->o_ss.channels;

    out_n_frames = (in_n_frames*r->o_ss.rate/r->i_ss.rate)+1024;
    out_n_samples = out_n_frames * r->o_ss.channels;

    if (!u->buf3.memblock || u->buf3_samples < out_n_samples) {
        if (u->buf3.memblock)
            pa_memblock_unref(u->buf3.memblock);

        u->buf3_samples = out_n_samples;
        u->buf3.memblock = pa_memblock_new(r->mempool, u->buf3.length = sizeof(float) * out_n_samples);
        u->buf3.index = 0;
    }

    data.data_in = (float*) ((uint8_t*) pa_memblock_acquire(input->memblock) + input->index);
    data.input_frames = in_n_frames;

    data.data_out = (float*) pa_memblock_acquire(u->buf3.memblock);
    data.output_frames = out_n_frames;

    data.src_ratio = (double) r->o_ss.rate / r->i_ss.rate;
    data.end_of_input = 0;

    ret = src_process(u->src_state, &data);
    assert(ret == 0);
    assert((unsigned) data.input_frames_used == in_n_frames);

    pa_memblock_release(input->memblock);
    pa_memblock_release(u->buf3.memblock);

    u->buf3.length = data.output_frames_gen * sizeof(float) * r->o_ss.channels;

    return &u->buf3;
}

static pa_memchunk *convert_from_float(pa_resampler *r, pa_memchunk *input) {
    struct impl_libsamplerate *u;
    unsigned n_samples, n_frames;
    void *src, *dst;

    assert(r);
    assert(input);
    assert(r->impl_data);
    u = r->impl_data;

    /* Convert the data into the correct sample type and place the result in buf4 */

    if (!u->from_float32ne_func || !input->length)
        return input;

    n_frames = input->length / sizeof(float) / r->o_ss.channels;
    n_samples = n_frames * r->o_ss.channels;

    if (u->buf4_samples < n_samples) {
        if (u->buf4.memblock)
            pa_memblock_unref(u->buf4.memblock);

        u->buf4_samples = n_samples;
        u->buf4.memblock = pa_memblock_new(r->mempool, u->buf4.length = r->o_fz * n_frames);
        u->buf4.index = 0;
    }

    src = (uint8_t*) pa_memblock_acquire(input->memblock) + input->length;
    dst = pa_memblock_acquire(u->buf4.memblock);
    u->from_float32ne_func(n_samples, src, dst);
    pa_memblock_release(input->memblock);
    pa_memblock_release(u->buf4.memblock);

    u->buf4.length = r->o_fz * n_frames;

    return &u->buf4;
}

static void libsamplerate_run(pa_resampler *r, const pa_memchunk *in, pa_memchunk *out) {
    struct impl_libsamplerate *u;
    pa_memchunk *buf;

    assert(r);
    assert(in);
    assert(out);
    assert(in->length);
    assert(in->memblock);
    assert(in->length % r->i_fz == 0);
    assert(r->impl_data);

    u = r->impl_data;

    buf = convert_to_float(r, (pa_memchunk*) in);
    buf = remap_channels(r, buf);
    buf = resample(r, buf);

    if (buf->length) {
        buf = convert_from_float(r, buf);
        *out = *buf;

        if (buf == in)
            pa_memblock_ref(buf->memblock);
        else
            pa_memchunk_reset(buf);
    } else
        pa_memchunk_reset(out);

    pa_memblock_release(in->memblock);

}

static void libsamplerate_update_input_rate(pa_resampler *r, uint32_t rate) {
    struct impl_libsamplerate *u;

    assert(r);
    assert(rate > 0);
    assert(r->impl_data);
    u = r->impl_data;

    if (!u->src_state) {
        int err;
        u->src_state = src_new(r->resample_method, r->o_ss.channels, &err);
        assert(u->src_state);
    } else {
        int ret = src_set_ratio(u->src_state, (double) r->o_ss.rate / rate);
        assert(ret == 0);
    }
}

static int libsamplerate_init(pa_resampler *r) {
    struct impl_libsamplerate *u = NULL;
    int err;

    r->impl_data = u = pa_xnew(struct impl_libsamplerate, 1);

    pa_memchunk_reset(&u->buf1);
    pa_memchunk_reset(&u->buf2);
    pa_memchunk_reset(&u->buf3);
    pa_memchunk_reset(&u->buf4);
    u->buf1_samples = u->buf2_samples = u->buf3_samples = u->buf4_samples = 0;

    if (r->i_ss.format == PA_SAMPLE_FLOAT32NE)
        u->to_float32ne_func = NULL;
    else if (!(u->to_float32ne_func = pa_get_convert_to_float32ne_function(r->i_ss.format)))
        goto fail;

    if (r->o_ss.format == PA_SAMPLE_FLOAT32NE)
        u->from_float32ne_func = NULL;
    else if (!(u->from_float32ne_func = pa_get_convert_from_float32ne_function(r->o_ss.format)))
        goto fail;

    if (r->o_ss.rate == r->i_ss.rate)
        u->src_state = NULL;
    else if (!(u->src_state = src_new(r->resample_method, r->o_ss.channels, &err)))
        goto fail;

    r->impl_free = libsamplerate_free;
    r->impl_update_input_rate = libsamplerate_update_input_rate;
    r->impl_run = libsamplerate_run;

    calc_map_table(r);

    return 0;

fail:
    pa_xfree(u);
    return -1;
}

/* Trivial implementation */

static void trivial_run(pa_resampler *r, const pa_memchunk *in, pa_memchunk *out) {
    size_t fz;
    unsigned  n_frames;
    struct impl_trivial *u;

    assert(r);
    assert(in);
    assert(out);
    assert(r->impl_data);

    u = r->impl_data;

    fz = r->i_fz;
    assert(fz == r->o_fz);

    n_frames = in->length/fz;

    if (r->i_ss.rate == r->o_ss.rate) {

        /* In case there's no diefference in sample types, do nothing */
        *out = *in;
        pa_memblock_ref(out->memblock);

        u->o_counter += n_frames;
    } else {
        /* Do real resampling */
        size_t l;
        unsigned o_index;
        void *src, *dst;

        /* The length of the new memory block rounded up */
        l = ((((n_frames+1) * r->o_ss.rate) / r->i_ss.rate) + 1) * fz;

        out->index = 0;
        out->memblock = pa_memblock_new(r->mempool, l);

        src = (uint8_t*) pa_memblock_acquire(in->memblock) + in->index;
        dst = pa_memblock_acquire(out->memblock);

        for (o_index = 0;; o_index++, u->o_counter++) {
            unsigned j;

            j = (u->o_counter * r->i_ss.rate / r->o_ss.rate);
            j = j > u->i_counter ? j - u->i_counter : 0;

            if (j >= n_frames)
                break;

            assert(o_index*fz < pa_memblock_get_length(out->memblock));

            memcpy((uint8_t*) dst + fz*o_index,
                   (uint8_t*) src + fz*j, fz);

        }

        pa_memblock_release(in->memblock);
        pa_memblock_release(out->memblock);

        out->length = o_index*fz;
    }

    u->i_counter += n_frames;

    /* Normalize counters */
    while (u->i_counter >= r->i_ss.rate) {
        u->i_counter -= r->i_ss.rate;
        assert(u->o_counter >= r->o_ss.rate);
        u->o_counter -= r->o_ss.rate;
    }
}

static void trivial_free(pa_resampler *r) {
    assert(r);

    pa_xfree(r->impl_data);
}

static void trivial_update_input_rate(pa_resampler *r, uint32_t rate) {
    struct impl_trivial *u;

    assert(r);
    assert(rate > 0);
    assert(r->impl_data);

    u = r->impl_data;
    u->i_counter = 0;
    u->o_counter = 0;
}

static int trivial_init(pa_resampler*r) {
    struct impl_trivial *u;

    assert(r);
    assert(r->i_ss.format == r->o_ss.format);
    assert(r->i_ss.channels == r->o_ss.channels);

    r->impl_data = u = pa_xnew(struct impl_trivial, 1);
    u->o_counter = u->i_counter = 0;

    r->impl_run = trivial_run;
    r->impl_free = trivial_free;
    r->impl_update_input_rate = trivial_update_input_rate;

    return 0;
}


