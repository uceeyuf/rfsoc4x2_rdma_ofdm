/*
 * I/Q OFDM modem on the host: the transmitter and receiver of sw/src/ofdm.c (A53) and
 * host/ofdm.py, without the board I/O, reentrant (one ofdm_ctx per thread).
 * Copyright (c) 2026, Yijie Yu. BSD-3-Clause.
 */

#ifndef OFDM_MODEM_H_
#define OFDM_MODEM_H_

#include <stdint.h>

#define OFDM_N          1024
#define OFDM_CP         128
#define OFDM_SYM        (OFDM_N + OFDM_CP)
#define OFDM_K_LO       16
#define OFDM_K_HI       460
#define OFDM_DATA_SYM   26
#define OFDM_FRAME      32768           /* 2 training + 26 data symbols + 512 zeros */

typedef struct {
    int    frame_pos;           /* start of the first training body */
    int    q_sign, q_lag;       /* Q rail polarity and offset vs I (samples) */
    double irr_db;              /* image rejection, median over the active sub-carriers */
    double evm_lin_db, evm_wl_db;
    int    err_lin, err_wl, bits;
} ofdm_result;

typedef struct ofdm_ctx ofdm_ctx;

ofdm_ctx *ofdm_new(void);
void      ofdm_free(ofdm_ctx *c);
int       ofdm_data_carriers(void);
double    ofdm_rate_bps(int bits_per_sym, double fs_hz);

/* one frame (OFDM_FRAME samples per rail), RMS per rail as a fraction of full scale;
   returns the number of clipped samples */
int ofdm_mod(ofdm_ctx *c, int bits_per_sym, double rms_fs, int16_t *i_out, int16_t *q_out);

/* demodulate one frame from nsamp >= 2 * OFDM_FRAME samples per rail (acquire = 1: search the
   frame and the Q offset as the A53 receiver does; acquire = 0: use r->frame_pos, r->q_sign and
   r->q_lag as given, as a receiver that is locked to the stream would) */
int ofdm_demod(ofdm_ctx *c, int bits_per_sym, const int16_t *ci, const int16_t *cq, int nsamp,
               int acquire, ofdm_result *r);

/* streaming receiver: a receiver locked to the stream (frame_pos, q_sign from ofdm_demod with
   acquire = 1) demodulates each frame with the widely linear equalizer and a hard decision, and
   writes the payload bits packed LSB first; returns the number of bits (no EVM / BER work) */
int ofdm_demod_stream(ofdm_ctx *c, int bits_per_sym, const int16_t *ci, const int16_t *cq,
                      int frame_pos, int q_sign, uint8_t *bits_out);

/* the same on the stream memory format (per 16 int16: 8 I samples, then 8 Q samples), with the
   first training body at sample p >= 136 of mem (no wrap) */
int ofdm_demod_stream_mem(ofdm_ctx *c, int bits_per_sym, const int16_t *mem, int p, int q_sign,
                          uint8_t *bits_out);

/* streaming transmitter: one frame (OFDM_FRAME samples) of the given payload bits (packed LSB
   first, ofdm_frame_bits() of them = a whole number of bytes; the buffer must be readable 8 bytes
   beyond) in the stream memory format, scaled by ofdm_stream_gain(); returns clipped values.
   The receiver's output bits are the same bits in the same order. */
int    ofdm_frame_bits(int bits_per_sym);
double ofdm_stream_gain(double rms_fs);
int    ofdm_mod_stream(ofdm_ctx *c, int bits_per_sym, double gain, const uint8_t *bits, int16_t *mem);

/* the payload bits the transmitter sends in one frame, packed LSB first; returns the count */
int ofdm_ref_bits(int bits_per_sym, uint8_t *bits_out);

#endif
