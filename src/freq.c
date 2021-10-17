/* Copyright (c) 2017-2018 Mozilla */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "kiss_fft.h"
#include "common.h"
#include <math.h>
#include "freq.h"
#include "pitch.h"
#include "arch.h"
#include "celt_lpc.h"
#include <assert.h>

#define SQUARE(x) ((x)*(x))

static const opus_int16 eband20ms[] = {
/*   0  100  200  300  400  500  600  700  800  900 1050 1200 1350 1550 1750 2000 2300 2600 2950 3350 3850 4400 5050 5850 6800 8000 */
     0,   2,   4,   6,   8,  10,  12,  14,  16,  18,  21,  24,  27,  31,  35,  40,  46,  52,  59,  67,  77,  88, 101, 117, 136, 160, };

static const float compensation[] = {
1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 0.800000, 0.666667, 0.666667, 0.571429, 0.500000, 0.444444, 0.363636, 0.333333, 0.307692, 0.266667, 0.222222, 0.190476, 0.166667, 0.137931, 0.114286, 0.093023, 0.093023, };


typedef struct {
  int init;
  kiss_fft_state *kfft;
  float half_window[OVERLAP_SIZE];
  float dct_table[NB_BANDS*NB_BANDS];
} CommonState;



void compute_band_energy(float *bandE, const kiss_fft_cpx *X) {
  int i;
  float sum[NB_BANDS] = {0};
  for (i=0;i<NB_BANDS-1;i++)
  {
    int j;
    int band_size;
    band_size = eband20ms[i+1]-eband20ms[i];
    for (j=0;j<band_size;j++) {
      float tmp;
      float frac = (float)j/band_size;
      tmp = SQUARE(X[eband20ms[i] + j].r);
      tmp += SQUARE(X[eband20ms[i] + j].i);
      sum[i] += (1-frac)*tmp;
      sum[i+1] += frac*tmp;
    }
  }
  sum[0] *= 2;
  sum[NB_BANDS-1] *= 2;
  for (i=0;i<NB_BANDS;i++)
  {
    bandE[i] = sum[i];
  }
}

void compute_band_corr(float *bandE, const kiss_fft_cpx *X, const kiss_fft_cpx *P) {
  int i;
  float sum[NB_BANDS] = {0};
  for (i=0;i<NB_BANDS-1;i++)
  {
    int j;
    int band_size;
    band_size = eband20ms[i+1]-eband20ms[i];
    for (j=0;j<band_size;j++) {
      float tmp;
      float frac = (float)j/band_size;
      tmp = X[eband20ms[i] + j].r * P[eband20ms[i] + j].r;
      tmp += X[eband20ms[i] + j].i * P[eband20ms[i] + j].i;
      sum[i] += (1-frac)*tmp;
      sum[i+1] += frac*tmp;
    }
  }
  sum[0] *= 2;
  sum[NB_BANDS-1] *= 2;
  for (i=0;i<NB_BANDS;i++)
  {
    bandE[i] = sum[i];
  }
}

void interp_band_gain(float *g, const float *bandE) {
  int i;
  memset(g, 0, FREQ_SIZE);
  for (i=0;i<NB_BANDS-1;i++)
  {
    int j;
    int band_size;
    band_size = eband20ms[i+1]-eband20ms[i];
    for (j=0;j<band_size;j++) {
      float frac = (float)j/band_size;
      g[eband20ms[i] + j] = (1-frac)*bandE[i] + frac*bandE[i+1];
    }
  }
}

CommonState common;

static void check_init() {
  int i;
  if (common.init) return;
  common.kfft = opus_fft_alloc_twiddles(WINDOW_SIZE, NULL, NULL, NULL, 0);
  for (i=0;i<OVERLAP_SIZE;i++)
    common.half_window[i] = sin(.5*M_PI*sin(.5*M_PI*(i+.5)/OVERLAP_SIZE) * sin(.5*M_PI*(i+.5)/OVERLAP_SIZE));
  for (i=0;i<NB_BANDS;i++) {
    int j;
    for (j=0;j<NB_BANDS;j++) {
      common.dct_table[i*NB_BANDS + j] = cos((i+.5)*j*M_PI/NB_BANDS);
      if (j==0) common.dct_table[i*NB_BANDS + j] *= sqrt(.5);
    }
  }
  common.init = 1;
}

void dct(float *out, const float *in) {
  int i;
  check_init();
  for (i=0;i<NB_BANDS;i++) {
    int j;
    float sum = 0;
    for (j=0;j<NB_BANDS;j++) {
      sum += in[j] * common.dct_table[j*NB_BANDS + i];
    }
    out[i] = sum*sqrt(2./NB_BANDS);
  }
}

void idct(float *out, const float *in) {
  int i;
  check_init();
  for (i=0;i<NB_BANDS;i++) {
    int j;
    float sum = 0;
    for (j=0;j<NB_BANDS;j++) {
      sum += in[j] * common.dct_table[i*NB_BANDS + j];
    }
    out[i] = sum*sqrt(2./NB_BANDS);
  }
}

void forward_transform(kiss_fft_cpx *out, const float *in) {
  int i;
  kiss_fft_cpx x[WINDOW_SIZE];
  kiss_fft_cpx y[WINDOW_SIZE];
  check_init();
  for (i=0;i<WINDOW_SIZE;i++) {
    x[i].r = in[i];
    x[i].i = 0;
  }
  opus_fft(common.kfft, x, y, 0);
  for (i=0;i<FREQ_SIZE;i++) {
    out[i] = y[i];
  }
}

void inverse_transform(float *out, const kiss_fft_cpx *in) {
  int i;
  kiss_fft_cpx x[WINDOW_SIZE];
  kiss_fft_cpx y[WINDOW_SIZE];
  check_init();
  for (i=0;i<FREQ_SIZE;i++) {
    x[i] = in[i];
  }
  for (;i<WINDOW_SIZE;i++) {
    x[i].r = x[WINDOW_SIZE - i].r;
    x[i].i = -x[WINDOW_SIZE - i].i;
  }
  opus_fft(common.kfft, x, y, 0);
  /* output in reverse order for IFFT. */
  out[0] = WINDOW_SIZE*y[0].r;
  for (i=1;i<WINDOW_SIZE;i++) {
    out[i] = WINDOW_SIZE*y[WINDOW_SIZE - i].r;
  }
}

float lpc_from_bands(float *lpc, const float *Ex)
{
   int i;
   float e;
   float ac[LPC_ORDER+1];
   float rc[LPC_ORDER];
   float Xr[FREQ_SIZE];
   kiss_fft_cpx X_auto[FREQ_SIZE];
   float x_auto[WINDOW_SIZE];
   interp_band_gain(Xr, Ex);
   Xr[FREQ_SIZE-1] = 0;
   RNN_CLEAR(X_auto, FREQ_SIZE);
   for (i=0;i<FREQ_SIZE;i++) X_auto[i].r = Xr[i];
   inverse_transform(x_auto, X_auto);
   for (i=0;i<LPC_ORDER+1;i++) ac[i] = x_auto[i];

   /* -40 dB noise floor. */
   ac[0] += ac[0]*1e-4 + 320/12/38.;
   /* Lag windowing. */
   for (i=1;i<LPC_ORDER+1;i++) ac[i] *= (1 - 6e-5*i*i);
   e = _celt_lpc(lpc, rc, ac, LPC_ORDER);
   return e;
}

float lpc_from_cepstrum(float *lpc, const float *cepstrum)
{
   int i;
   float Ex[NB_BANDS];
   float tmp[NB_BANDS];
   RNN_COPY(tmp, cepstrum, NB_BANDS);
   tmp[0] += 4;
   idct(Ex, tmp);
   for (i=0;i<NB_BANDS;i++) Ex[i] = pow(10.f, Ex[i])*compensation[i];
   return lpc_from_bands(lpc, Ex);
}

void apply_window(float *x) {
  int i;
  check_init();
  for (i=0;i<OVERLAP_SIZE;i++) {
    x[i] *= common.half_window[i];
    x[WINDOW_SIZE - 1 - i] *= common.half_window[i];
  }
}

