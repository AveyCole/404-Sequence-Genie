#include "sequence_perform.h"

static uint32_t random_word(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state ^ (*state >> 16);
}

int sp_seq_generate(sp_sequence *seq, unsigned pad, unsigned steps, unsigned hits,
                    unsigned rotation, unsigned root, unsigned octaves,
                    unsigned scale_mask, unsigned velocity_min, unsigned velocity_max,
                    uint32_t seed)
{
    if (!seq || pad >= 160 || !steps || steps > SP_SEQ_STEPS || hits > steps ||
        root > 127 || !octaves || octaves > 10 || !(scale_mask & 0xfff) ||
        !velocity_min || velocity_min > velocity_max || velocity_max > 127)
        return -1;
    uint8_t notes[128];
    unsigned count = 0;
    for (unsigned i = 0; i < 12 * octaves && root + i <= 127; ++i)
        if (scale_mask & (1u << (i % 12))) notes[count++] = (uint8_t)(root + i);
    if (!count) return -1;
    seq->length = (uint8_t)steps;
    seq->source_pad = (uint8_t)pad;
    rotation %= steps;
    for (unsigned i = 0; i < steps; ++i) {
        unsigned phase = (i + steps - rotation) % steps;
        seq->step[i].hit = (phase * hits) % steps < hits;
        seq->step[i].note = notes[random_word(&seed) % count];
        seq->step[i].velocity = velocity_min + random_word(&seed) % (velocity_max - velocity_min + 1);
    }
    return 0;
}

void sp_seq_mutate(sp_sequence *seq, uint32_t seed)
{
    if (!seq || seq->length < 2 || seq->length > SP_SEQ_STEPS) return;
    unsigned a = random_word(&seed) % seq->length;
    unsigned b = (a + 1 + random_word(&seed) % (seq->length - 1)) % seq->length;
    sp_seq_step tmp = seq->step[a];
    seq->step[a] = seq->step[b];
    seq->step[b] = tmp;
}

int sp_seq_start(sp_performance *p, const sp_sequence *seq, sp_seq_audio audio,
                 unsigned rate, unsigned bpm_x10, int dest, int16_t *storage, unsigned capacity)
{
    if (!p || !seq || !seq->length || seq->length > SP_SEQ_STEPS || seq->source_pad >= 160 ||
        !audio.trigger || !audio.render || rate < 8000 || rate > 96000 ||
        bpm_x10 < 100 || bpm_x10 > 4000 || dest < -1 || dest >= 160)
        return -1;
    /* Divide once per step with carried remainder to avoid cumulative drift. */
    unsigned numerator = rate * 150u;
    unsigned total = (numerator / bpm_x10) * seq->length +
                     ((numerator % bpm_x10) * seq->length) / bpm_x10;
    if (dest >= 0 && (dest == seq->source_pad || !storage || capacity < total)) return -1;
    p->sequence = *seq;
    p->audio = audio;
    p->destination = dest;
    p->capture = dest >= 0 ? storage : 0;
    p->position = p->next = 0;
    p->total = total;
    for (unsigned i = 0; i <= seq->length; ++i)
        p->boundary[i] = (numerator / bpm_x10) * i + ((numerator % bpm_x10) * i) / bpm_x10;
    return 0;
}

unsigned sp_seq_play(sp_performance *p, int16_t *out, unsigned frames)
{
    if (!p || !out) return 0;
    unsigned filled = 0;
    while (filled < frames && p->position < p->total) {
        if (p->position == p->boundary[p->next]) {
            sp_seq_step event = p->sequence.step[p->next++];
            if (event.hit) p->audio.trigger(p->audio.context, p->sequence.source_pad,
                                           event.note, event.velocity);
        }
        unsigned n = p->boundary[p->next] - p->position;
        if (n > frames - filled) n = frames - filled;
        p->audio.render(p->audio.context, out + 2 * filled, n);
        if (p->capture)
            for (unsigned j = 0; j < 2 * n; ++j)
                p->capture[2 * p->position + j] = out[2 * filled + j];
        filled += n;
        p->position += n;
    }
    return filled;
}

int sp_seq_commit(sp_performance *p,
                  int (*publish_empty)(void *, unsigned, const int16_t *, unsigned))
{
    if (!p || !publish_empty || !p->capture || p->position != p->total) return -1;
    int result = publish_empty(p->audio.context, (unsigned)p->destination, p->capture, p->total);
    if (result == 0) p->capture = 0;
    return result;
}
