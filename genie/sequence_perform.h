#ifndef SP_SEQUENCE_PERFORM_H
#define SP_SEQUENCE_PERFORM_H
#include <stdint.h>

#ifdef GENIE_ON_DEVICE
#define SP_SEQ_STEPS 128
#else
#define SP_SEQ_STEPS 64
#endif
enum { SP_SEQ_AUDITION = -1 };
typedef struct {
    uint8_t hit, note, velocity;
} sp_seq_step;

typedef struct {
    sp_seq_step step[SP_SEQ_STEPS];
    uint8_t length, source_pad;
} sp_sequence;

/* Scale mask uses bit zero for the root. Regenerate either dimension by
 * changing the rhythm settings or the note seed independently. Pads are zero-based 0..159. */
int sp_seq_generate(sp_sequence *seq, unsigned pad, unsigned steps, unsigned hits,
                    unsigned rotation, unsigned root, unsigned octaves,
                    unsigned scale_mask, unsigned velocity_min, unsigned velocity_max,
                    uint32_t seed);
void sp_seq_mutate(sp_sequence *seq, uint32_t seed);

/* The adapter triggers a voice and renders the resulting stereo performance
 * through its FX chain. Capture copies this same audible output. */
typedef struct {
    void *context;
    void (*trigger)(void *, unsigned pad, unsigned note, unsigned velocity);
    void (*render)(void *, int16_t *stereo, unsigned frames);
} sp_seq_audio;

typedef struct {
    sp_sequence sequence;
    sp_seq_audio audio;
    int16_t *capture;
    uint32_t position, total, boundary[SP_SEQ_STEPS + 1];
    int destination;
    unsigned next;
} sp_performance;

/* Runs one phrase, quantized to sixteenth notes. Capture storage belongs to
 * the caller until commit or discard. Start a new take on an idle instance. */
int sp_seq_start(sp_performance *p, const sp_sequence *seq, sp_seq_audio audio,
                 unsigned sample_rate, unsigned bpm_x10, int destination,
                 int16_t *storage, unsigned capacity_frames);
/* Returns frames filled, including the last partial block. */
unsigned sp_seq_play(sp_performance *p, int16_t *output, unsigned frames);
/* Call off the audio thread. Adapter must atomically claim an empty pad and
 * publish the sample; a failed commit can be retried. Zero means success. */
int sp_seq_commit(sp_performance *p,
                  int (*publish_empty)(void *, unsigned, const int16_t *, unsigned));
#endif
