// Copyright 2012 Evrytania LLC (http://www.evrytania.com)
//
// Written by James Peroulas <james@evrytania.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

// Improved by Jiao Xianjun (putaoshu@gmail.com):
// 1. TD-LTE support
// 2. fast pre-search frequencies (external mixer/LNB support)
// 3. multiple tries at one frequency
// 4. .bin file recording and replaying

// Relationships between the various frequencies, correction factors, etc.
//
// Fixed relationships:
// xtal_spec=28.8MHz (usually...)
// k_factor=xtal_true/xtal_spec
// xtal_true=xtal_spec*k_factor
// fs_true=k_s*xtal_true
// fs_prog=k_s*xtal_spec
// fc_true=k_c*xtal_true
// fc_prog=k_c*xtal_spec
// freq_offset=fc_req-fc_true
//
// k_factor as a function of known parameters:
// fc_true=fc_req-freq_offset
// xtal_true=fc_true/k_c=(fc_req-freq_offset)/k_c
// k_factor=(fc_req-freq_offset)/(k_c*xtal_spec)
// k_factor=(fc_req-freq_offset)/(fc_prog/xtal_spec*xtal_spec)
// k_factor=(fc_req-freq_offset)/fc_prog
//
// fs_prog*k_factor=k_s*xtal_spec*k_factor=k_s*xtal_true=fs_true
//
// N LTE samples sampled at a rate of FS_LTE/16 is equivalent to this
// many samples sampled at fs_true.
// N*(1/(FS_LTE/16))/(1/(fs_prog*k_factor))
// N*(16/FS_LTE)/(1/(fs_prog*k_factor))
// N*16/FS_LTE*fs_prog*k_factor

#include <itpp/itbase.h>
#include <itpp/signal/transforms.h>
#include <itpp/stat/misc_stat.h>
#include <math.h>
#include <list>
#include <iomanip>
#include <algorithm>
#include <vector>
#include <boost/math/special_functions/gamma.hpp>
#include <sys/time.h>
#include "rtl-sdr.h"
#include "common.h"
#include "lte_lib.h"
#include "constants.h"
#include "macros.h"
#include "itpp_ext.h"
#include "dsp.h"
#include "searcher.h"

// This LTE cell search algorithm was designed under the following guidelines:
//
// 1) Low performance hardware
// This algorithm was designed to work with the RTL-SDR dongle which has a
// noise figure of 20dB. As such, attempts were made to maximize signal
// processing performance on the SDR side so as to make up for some of the
// peroformance lost in the hardware. This was done at the cost of algorithm
// complexity and execution speed.
//
// 2) Capture-then-process
// The algorithm was designed so that data would first be captured and then
// analysis would begin. It is not possible for the analysis algorithm to
// request 'more' data part-way through the analysis step.
//
// For maximal signal processing performance, the entire MIB must be captured.
// The MIB spans 40ms but the location of the start of the MIB is not knwon
// in advance. Thus, to ensure that an entire MIB is captured, approximately
// 80ms of data must be captured.
//
// 3) Use all available data
// For example, PSS detection only needs 5ms of data. However, since 80ms
// of data will be available, all 80ms of captured data will be used to
// improve PSS detection performance.
//
// 4) Handle all cell types
// This algorithm can handle synchronous or asynchronous networks and can
// also be used in situations where the receiver is receiving a signal
// from both normal and extended CP cells. All found cells will be reported.
//
// 5) Work for all cell loads
// In a highly loaded cell, the transmitted (and received) power is more
// or less constant in time and hence the entire received signal can be used
// to estimate the total received power. In a lightly loaded cell, the TX
// power varies wildly between OFDM symbols containing PSS/SSS, OFDM symbols
// containing RS, and OFDM symbols with no data being transmitted.
//
// Final performance:
// ==================
// Simulations indicate that this algorithm can reliably detect the PSS/SSS
// and therefore the Cell ID down to SNR's (AWGN) of about -12dB. MIB decoding
// is only reliable down to about -10dB and thus overal performance is limited
// by MIB decoding.

using namespace itpp;
using namespace std;

//#define DBG(CODE) CODE
#define DBG(CODE)

void xc_correlate_new(
  // Inputs
  const cvec & capbuf,
  const vec & f_search_set,
  const cmat & pss_fo_set,
  // Outputs
  vcf3d & xc
) {

}
// Correlate the received data against various frequency shifted versions
// of the three PSS sequences.
// This is likely to be the slowest routine since it needs to process so
// much data.
void xc_correlate(
  // Inputs
  const cvec & capbuf,
  const vec & f_search_set,
  const double & fc_requested,
  const double & fc_programmed,
  const double & fs_programmed,
  const bool & sampling_carrier_twist,
  double & k_factor,
  // Outputs
  vcf3d & xc
) {
  const uint32 n_cap=length(capbuf);
  const uint16 n_f=length(f_search_set);

  // Set aside space for the vector and initialize with NAN's.
#ifndef NDEBUG
  xc=vector < vector < vector < complex < float > > > > (3,vector< vector < complex < float > > >(n_cap-136, vector < complex < float > > (n_f,NAN)));
#else
  xc=vector < vector < vector < complex < float > > > > (3,vector< vector < complex < float > > >(n_cap-136, vector < complex < float > > (n_f)));
#endif

  // Local variables declared outside of the loop.
  double f_off;
  cvec temp;
  complex <double> acc;
  uint16 foi;
  uint8 t;
  uint32 k;
  uint8 m;

  // Loop and perform correlations.
  //Real_Timer tt;
  //tt.tic();
  for (foi=0;foi<n_f;foi++) {
    f_off=f_search_set(foi);
    if (sampling_carrier_twist) {
        k_factor=(fc_requested-f_off)/fc_programmed;
    }
    //cout << "f_off " << f_off << " k_factor " << k_factor << " fc_requested " << fc_requested << " fc_programmed " << fc_programmed << " fs_programmed " << fs_programmed << "\n";
    for (t=0;t<3;t++) {
      temp=ROM_TABLES.pss_td[t];
      temp=fshift(temp,f_off,fs_programmed*k_factor);
      temp=conj(temp)/137;
#ifdef _OPENMP
#pragma omp parallel for shared(temp,capbuf,xc) private(k,acc,m)
#endif
      for (k=0;k<n_cap-136;k++) {
        acc=0;
        for (m=0;m<137;m++) {
          // Correlations are performed at the 2x rate which effectively
          // performs filtering and correlating at the same time. Thus,
          // this algorithm can handle huge frequency offsets limited only
          // by the bandwidth of the capture device.
          // Correlations can also be done at the 1x rate if filtering is
          // peformed first, but this will limit the set of frequency offsets
          // that this algorithm can detect. 1x rate correlations will,
          // however, be nearly twice as fast as the 2x correlations
          // performed here.
          acc+=temp(m)*capbuf(k+m);
        }
        xc[t][k][foi]=acc;
      }
    }
  }
  //tt.toc_print();
}

// Estimate the received signal power within 2 OFDM symbols of a particular
// sample.
//
// In the 6 center RB's, the transmitted power is the same for all PSS and
// SSS OFDM symbols regardless of the cell load.
//
// This function is slightly inaccurate because it estimates the received
// power in all the RB's (approximately 12) instead of the signal power
// only in the center 6 RB's.
void sp_est(
  // Inputs
  const cvec & capbuf,
  // Outputs
  vec & sp,
  vec & sp_incoherent,
  uint16 & n_comb_sp
) {
  const uint32 n_cap=length(capbuf);
  n_comb_sp=floor_i((n_cap-136-137)/9600);
  const uint32 n_sp=n_comb_sp*9600;

  // Set aside space for the vector and initialize with NAN's.
  sp=vec(n_sp);
#ifndef NDEBUG
  sp=NAN;
#endif
  sp[0]=0;
  // Estimate power for first time offset
  for (uint16 t=0;t<274;t++) {
    sp[0]+=pow(capbuf[t].real(),2)+pow(capbuf[t].imag(),2);
  }
  sp[0]=sp[0]/274;
  // Estimate RX power for remaining time offsets.
  for (uint32 t=1;t<n_sp;t++) {
    sp[t]=sp[t-1]+(-pow(capbuf[t-1].real(),2)-pow(capbuf[t-1].imag(),2)+pow(capbuf[t+274-1].real(),2)+pow(capbuf[t+274-1].imag(),2))/274;
  }

  // Combine incoherently
  sp_incoherent=sp.left(9600);
  for (uint16 t=1;t<n_comb_sp;t++) {
    sp_incoherent+=sp.mid(t*9600,9600);
  }
  sp_incoherent=sp_incoherent/n_comb_sp;
  // Shift to the right by 137 samples to align with the correlation peaks.
  tshift(sp_incoherent,137);
}

// Perform incoherent combining on the correlations.
//
// There is no guarantee that PSS/SSS pairs will always be transmitted from
// the same antenna. Thus, PSS/SSS pairs separated by 5ms can only be combined
// incoherently.
//
// Because the size of the capture buffer is very large (80ms) and because this
// algorithm must be able to handle very large frequency offsets (50kHz+),
// care must be taken to combine the correct samples incoherently.
//
// With no frequency error, the start of the next frame will be 19200 samples
// after the start of the current frame. With a +50kHz frequency offset and
// a 740MHz center frequency, the start of the next frame is actually 19198.7
// samples after the start of this frame. This function makes sure that
// the correct samples are combined incoherently.
//
// A side benefit of capturing multiple frames is that the correct downlink
// center frequency can be inferred. For an extreme example, suppose that
// the true downlink center frequency was 740MHz and that the frequency error
// of the oscillator could be up to 100kHz, but currently happens, by chance,
// to be zero. (We do not know in advance that the frequency error is zero.)
//
// For each programmed center frequency, the search will search frequency
// offsets from -100kHz to +100kHz. The searcher will program a center
// frequency of 739.9 MHz and search from -100kHz to +100kHz and it will
// then program a frequency of 740MHz and search from -100kHz to +100kHz,
// and it will also program a frequency of 740.1MHz and search from -100kHz
// to +100kHz.
//
// The correlations performed at 739.9MHz+100kHz, 740MHz+0kHz, and 740.1-100kHz
// will all be nearly the same and it will not be possible for the receiver
// to determine whether the true center frequency was 739.9, 740, or 740.1
// MHz.
//
// Although the correlation peaks for all 3 of the above scenarios will
// have the same magnitude, the spacing between the peaks will vary if
// multiple frames are captured. Only in the 740Mhz+0kHz case will the peaks
// be aligned with a spacing of 19200 samples and thus it is possible
// to determine boht that the true downlink center frequency is 740MHz and
// that the frequency error of the local oscillator is 0Hz.
void xc_combine(
  // Inputs
  const cvec & capbuf,
  const vcf3d & xc,
  const double & fc_requested,
  const double & fc_programmed,
  const double & fs_programmed,
  const vec & f_search_set,
  // Outputs
  vf3d & xc_incoherent_single,
  uint16 & n_comb_xc,
  const bool & sampling_carrier_twist,
  double & k_factor
) {
  const uint16 n_f=f_search_set.length();
  n_comb_xc=floor_i((xc[0].size()-100)/9600);

  // Create space for some arrays
#ifndef NDEBUG
  xc_incoherent_single=vector < vector < vector < float > > > (3,vector< vector < float > >(9600, vector < float > (n_f,NAN)));
#else
  xc_incoherent_single=vector < vector < vector < float > > > (3,vector< vector < float > >(9600, vector < float > (n_f)));
#endif
  for (uint16 foi=0;foi<n_f;foi++) {
    // Combine incoherently
    const double f_off=f_search_set[foi];
    if (sampling_carrier_twist) {
        k_factor=(fc_requested-f_off)/fc_programmed;
    }

    for (uint8 t=0;t<3;t++) {
      for (uint16 idx=0;idx<9600;idx++) {
        xc_incoherent_single[t][idx][foi]=0;
      }
      for (uint16 m=0;m<n_comb_xc;m++) {
        // Because of the large supported frequency offsets and the large
        // amount of time represented by the capture buffer, the length
        // in samples, of a frame varies by the frequency offset.
        //double actual_time_offset=m*.005*k_factor;
        //double actual_start_index=itpp::round_i(actual_time_offset*FS_LTE/16);
        double actual_start_index=itpp::round_i(m*.005*k_factor*fs_programmed);
        for (uint16 idx=0;idx<9600;idx++) {
          xc_incoherent_single[t][idx][foi]+=sqr(xc[t][idx+actual_start_index][foi]);
        }
      }
      for (uint16 idx=0;idx<9600;idx++) {
        xc_incoherent_single[t][idx][foi]=xc_incoherent_single[t][idx][foi]/n_comb_xc;
      }
    }
  }
}

// Combine adjacent taps that likely come from the same channel.
// Simply: xc_incoherent(t,idx,foi)=mean(xc_incoherent_single(t,idx-ds_comb_arm:idx+ds_comb_arm,foi);
void xc_delay_spread(
  // Inputs
  const vf3d & xc_incoherent_single,
  const uint8 & ds_comb_arm,
  // Outputs
  vf3d & xc_incoherent
) {
  const int n_f=xc_incoherent_single[0][0].size();

  // Create space for some arrays
#ifndef NDEBUG
  xc_incoherent=vector < vector < vector < float > > > (3,vector< vector < float > >(9600, vector < float > (n_f,NAN)));
#else
  xc_incoherent=vector < vector < vector < float > > > (3,vector< vector < float > >(9600, vector < float > (n_f)));
#endif
  for (uint16 foi=0;foi<n_f;foi++) {
    for (uint8 t=0;t<3;t++) {
      for (uint16 idx=0;idx<9600;idx++) {
        xc_incoherent[t][idx][foi]=xc_incoherent_single[t][idx][foi];
      }
    }
    for (uint8 t=1;t<=ds_comb_arm;t++) {
      for (uint8 k=0;k<3;k++) {
        for (uint16 idx=0;idx<9600;idx++) {
          xc_incoherent[k][idx][foi]+=xc_incoherent_single[k][itpp_ext::matlab_mod(idx-t,9600)][foi]+xc_incoherent_single[k][itpp_ext::matlab_mod(idx+t,9600)][foi];
        }
      }
    }
    // Normalize
    for (uint8 t=0;t<3;t++) {
      for (uint16 idx=0;idx<9600;idx++) {
        xc_incoherent[t][idx][foi]=xc_incoherent[t][idx][foi]/(2*ds_comb_arm+1);
      }
    }
  }
}

// Search for the peak correlation among all frequency offsets.
// For each time offset and each PSS index, examine the correlations
// for all of the frequency offsets and only keep the correlation with
// the largest magnitude.
void xc_peak_freq(
  // Inputs
  const vf3d & xc_incoherent,
  // Outputs
  mat & xc_incoherent_collapsed_pow,
  imat & xc_incoherent_collapsed_frq
) {
  const int n_f=xc_incoherent[0][0].size();

  xc_incoherent_collapsed_pow=mat(3,9600);
  xc_incoherent_collapsed_frq=imat(3,9600);
#ifndef NDEBUG
  xc_incoherent_collapsed_pow=NAN;
  xc_incoherent_collapsed_frq=-1;
#endif

  for (uint8 t=0;t<3;t++) {
    for (uint16 k=0;k<9600;k++) {
      double best_pow=xc_incoherent[t][k][0];
      uint16 best_idx=0;
      for (uint16 foi=1;foi<n_f;foi++) {
        if (xc_incoherent[t][k][foi]>best_pow) {
          best_pow=xc_incoherent[t][k][foi];
          best_idx=foi;
        }
      }
      xc_incoherent_collapsed_pow(t,k)=best_pow;
      xc_incoherent_collapsed_frq(t,k)=best_idx;
    }
  }
}

// normalize a vector to unit power for each sample (average meaning)
void normalize(
  // Input&Output
  cvec & s
) {
  uint32 len = length(s);
//  double acc = 0;
//  for( uint32 i=0; i<len; i++){
//    acc = acc + real(s(i)*conj(s(i)));
//  }
  double acc = sum( real(elem_mult(s, conj(s))) );
  s = sqrt(len)*s/sqrt(acc);
}

// FIR 6RB filter
void filter_my(
  //Inputs
  const vec & coef,
  //Inputs&Outputs
  cvec & capbuf
) {
  uint32 len = length(capbuf);
  uint16 len_fir = length(coef);
  uint16 len_half = (len_fir-1)/2;
//  cout << len_half << "\n";
  complex <double> acc;

  cvec tmpbuf(len);

  // to conform matlab filter
  for (uint32 i=len_half; i<len_fir; i++) {
    acc=0;
    for (uint16 j=0; j<(i+1); j++){
      acc = acc + coef[j]*capbuf[i-j];
    }
    tmpbuf[i-len_half] = acc;
  }

  for (uint32 i=len_fir; i<len; i++) {
    acc=0;
    for (uint16 j=0; j<len_fir; j++){
      acc = acc + coef[j]*capbuf[i-j];
    }
    tmpbuf[i-len_half] = acc;
  }

  for (uint32 i=len; i<(len+len_half); i++) {
    acc=0;
    for (uint16 j=(i-len+1); j<len_fir; j++){
      acc = acc + coef[j]*capbuf[i-j];
    }
    tmpbuf[i-len_half] = acc;
  }

  capbuf = tmpbuf;
}

// sub function of sampling_ppm_f_search_set_by_pss()
// perform corr in a specific window, and return locations of maximum values
// of interesting frequencies.
void pss_fix_location_corr(
  // Inputs
  const cvec & s,
  int32 start_position,
  int32 end_position,
  const cmat & pss_fo_set,
  const ivec & hit_pss_fo_set_idx,
  // Outputs
  ivec & hit_time_idx,
  vec & max_val
){
  uint16 len_pss = length( ROM_TABLES.pss_td[0] );
  uint16 num_fo_pss = length(hit_pss_fo_set_idx);

  vec tmp(num_fo_pss);
  mat corr_store(end_position-start_position+1, num_fo_pss);
  corr_store.zeros();
  cvec chn_tmp(len_pss);

  for(int32 i=start_position; i<=end_position; i++) {
    chn_tmp = s(i, (i+len_pss-1));
    normalize(chn_tmp);

//    for (uint16 j=0; j<num_fo_pss; j++){
//      complex <double> acc=0;
//      for (uint16 k=0; k<len_pss; k++){
//        acc = acc + chn_tmp(k)*pss_fo_set[hit_pss_fo_set_idx(j)][k];
//      }
//      tmp(j) = real( acc*conj(acc) );
//    }

    tmp = abs(pss_fo_set.get_rows(hit_pss_fo_set_idx)*chn_tmp);
    tmp = elem_mult( tmp,tmp );

    corr_store.set_row(i-start_position, tmp);
  }
  ivec max_idx(num_fo_pss);
  max_val.set_length(num_fo_pss,false);
  max_val = max(corr_store, max_idx, 1);

  hit_time_idx.set_length(num_fo_pss,false);
  hit_time_idx = start_position + max_idx;
//  cout << hit_time_idx << "\n";
}


// sub function of sampling_ppm_f_search_set_by_pss()
// perform moving corr until any peak at any frequencies exceeds specific threshold
void pss_moving_corr(
  // Inputs
  const cvec & s,
  const vec & f_search_set,
  const cmat & pss_fo_set,
  double th,
  // Outputs
  ivec & hit_pss_fo_set_idx,
  ivec & hit_time_idx,
  vec & hit_corr_val
) {
  uint16 num_pss = 3;
  uint16 len_pss = length( ROM_TABLES.pss_td[0] );
  uint16 num_fo_pss = num_pss*length( f_search_set );

  uint32 len = length(s);
  uint32 len_half_store = 64;
  mat corr_store(2*len_half_store+1, num_fo_pss);
  corr_store.zeros();

//  hit_pss_fo_set_idx.set_length(0,false);
//  hit_time_idx.set_length(0,false);
//  hit_corr_val.set_length(0,false);

  int32 end_idx = -1;
  int32 current_idx = -1;

  cvec chn_tmp(len_pss);
  vec tmp(num_fo_pss);
  for(uint32 i=0; i<(len - (len_pss-1)); i++) {
    chn_tmp = s(i, (i+len_pss-1));
    normalize(chn_tmp);

//    for (uint16 j=0; j<num_fo_pss; j++){
////      complex <double> acc=0;
////      for (uint16 k=0; k<len_pss; k++){
////        acc = acc + chn_tmp(k)*pss_fo_set[j][k];
////      }
//      complex <double> acc = elem_mult_sum(chn_tmp, pss_fo_set[j]);
////      tmp(j) = real( acc*conj(acc) );
//      tmp(j) = abs(acc);
//    }
    tmp = abs(pss_fo_set*chn_tmp);
    tmp = elem_mult( tmp,tmp );

    for (uint16 j=2*len_half_store; j>=1; j--) {
//      for (uint16 k=0; k<num_fo_pss; k++){
//        corr_store(j,k) = corr_store(j-1,k);
//      }
      corr_store.set_row(j, corr_store.get_row(j-1));
    }
//    for (uint16 k=0; k<num_fo_pss; k++){
//      corr_store(0,k) = tmp(k);
//    }
    corr_store.set_row(0, tmp);

//    if (i==0)
//      cout << tmp << "\n";
//    uint16 acc=0;
//    for (uint16 k=0; k<num_fo_pss; k++){
//      acc = acc + (tmp(k)>th?1:0);
//    }
    uint16 acc = sum(to_ivec(tmp>th));
    if (acc) {
//      cout << tmp << "\n";
      current_idx = i;
      end_idx = current_idx + len_half_store;
      break;
    }
  }

  if (end_idx != -1){
    double tmp_val;
    int tmpi;
    tmpi = (len - (len_pss-1))-1;
    int32 last_idx = end_idx>tmpi?tmpi:end_idx;

    for (uint32 i=(current_idx+1); i<(last_idx+1); i++ ){
      chn_tmp = s(i, (i+len_pss-1));
      normalize(chn_tmp);

//      for (uint16 j=0; j<num_fo_pss; j++){
//        complex <double> acc=0;
//        for (uint16 k=0; k<len_pss; k++){
//          acc = acc + chn_tmp(k)*pss_fo_set[j][k];
//        }
//        tmp(j) = real( acc*conj(acc) );
//      }
      tmp = abs(pss_fo_set*chn_tmp);
      tmp = elem_mult( tmp,tmp );

      for (uint16 j=2*len_half_store; j>=1; j--) {
//        for (uint16 k=0; k<num_fo_pss; k++){
//          corr_store(j,k) = corr_store(j-1,k);
//        }
        corr_store.set_row(j, corr_store.get_row(j-1));
      }
//      for (uint16 k=0; k<num_fo_pss; k++){
//        corr_store(0,k) = tmp(k);
//      }
      corr_store.set_row(0, tmp);
    }
//    cout << tmp << "\n";

    vec max_val(num_fo_pss);
    ivec max_idx(num_fo_pss);
    max_val = max(corr_store, max_idx, 1);
    ivec sort_idx = sort_index(max_val);
    sort_idx = reverse(sort_idx);
    max_val = max_val.get(sort_idx);
//    cout << sort_idx << "\n";
//    cout << max_val << "\n";
    tmp_val = max_val(0)/2;
    uint16 k;
    for (k=0; k<num_fo_pss; k++) {
      if (max_val(k)<tmp_val)
        break;
    }
    int16 num_valid = (k==num_fo_pss)?num_fo_pss:k;
//    if (k==num_fo_pss){
//      num_valid = num_fo_pss;
//    }
//    else {
//      num_valid = k;
//    }
//    cout << num_valid << "\n";

    hit_pss_fo_set_idx = sort_idx(0,(num_valid-1));
    hit_corr_val = max_val(0,(num_valid-1));
    hit_time_idx = last_idx - max_idx.get(hit_pss_fo_set_idx);
//    hit_pss_fo_set_idx.set_length(num_valid, false);
//    hit_time_idx.set_length(num_valid, false);
//    hit_corr_val.set_length(num_valid, false);
//    for (k=0; k<num_valid; k++) {
//      hit_pss_fo_set_idx(k) = sort_idx(num_fo_pss-1-k);
//      hit_corr_val(k) = max_val(num_fo_pss-1-k);
//      hit_time_idx(k) = last_idx - max_idx(hit_pss_fo_set_idx(k));
//    }
//    cout << hit_pss_fo_set_idx << "\n";
//    cout << hit_corr_val << "\n";
//    cout << hit_time_idx << "\n";
  }
}

// pre-generate td-pss of all frequencies offsets for non twisted mode
void pss_fo_set_gen_non_twist(
  // Input
  const vec & fo_search_set,
  const double & fs_programmed,
  const double & k_factor,
  // Output
  cmat & pss_fo_set
){
  uint16 num_pss = 3;
  uint16 len_pss = length(ROM_TABLES.pss_td[0]);

  uint16 num_fo = length(fo_search_set);
  uint32 num_fo_pss = num_fo*num_pss;
  cvec temp(len_pss);

  pss_fo_set.set_size(num_fo_pss, len_pss, false);
  for (uint32 fo_pss_i=0; fo_pss_i<num_fo_pss; fo_pss_i++) {
    uint32 pssi = fo_pss_i/num_fo;
    uint32 foi = fo_pss_i - pssi*num_fo;

    double f_off = fo_search_set(foi);
    temp = ROM_TABLES.pss_td[pssi];
    temp = fshift(temp,f_off,fs_programmed*k_factor);
    temp = conj(temp)/137;
    pss_fo_set.set_row(fo_pss_i, temp);
  }
}

// pre-generate td-pss of all frequencies offsets for twisted mode
void pss_fo_set_gen_twist(
  // Input
  const vec & fo_search_set,
  const double & fc_requested,
  const double & fc_programmed,
  const double & fs_programmed,
  // Output
  cmat & pss_fo_set
){
  uint16 num_pss = 3;
  uint16 len_pss = length(ROM_TABLES.pss_td[0]);

  uint16 num_fo = length(fo_search_set);
  uint32 num_fo_pss = num_fo*num_pss;
  cvec temp(len_pss);

  pss_fo_set.set_size(num_fo_pss, len_pss, false);
  for (uint32 fo_pss_i=0; fo_pss_i<num_fo_pss; fo_pss_i++) {
    uint32 pssi = fo_pss_i/num_fo;
    uint32 foi = fo_pss_i - pssi*num_fo;

    double f_off = fo_search_set(foi);
    double k_factor=(fc_requested-f_off)/fc_programmed;
    temp = ROM_TABLES.pss_td[pssi];
    temp = fshift(temp,f_off,fs_programmed*k_factor);
    temp = conj(temp)/137;
    pss_fo_set.set_row(fo_pss_i, temp);
  }
}


// pre-generate td-pss of all frequencies offsets for non-twisted mode
void pss_fo_set_gen(
  // Input
  const vec & fo_search_set,
  // Output
  cmat & pss_fo_set
){
  uint16 num_pss = 3;
  uint16 len_pss = length(ROM_TABLES.pss_td[0]);

  double sampling_rate = FS_LTE/16; // LTE spec
  uint16 num_fo = length(fo_search_set);
  uint32 num_fo_pss = num_fo*num_pss;
  cvec temp(len_pss);

  pss_fo_set.set_size(num_fo_pss, len_pss, false);
  for (uint32 fo_pss_i=0; fo_pss_i<num_fo_pss; fo_pss_i++) {
    uint32 pssi = fo_pss_i/num_fo;
    uint32 foi = fo_pss_i - pssi*num_fo;

    double f_off = fo_search_set(foi);
    temp = ROM_TABLES.pss_td[pssi];
    temp = fshift(temp,f_off,sampling_rate);
    temp = conj(temp);
    normalize(temp);
    pss_fo_set.set_row(fo_pss_i, temp);
  }
}

// pre-processing before xcorr_pss
void sampling_ppm_f_search_set_by_pss(
  // Inputs
  const cvec & s,
  const cmat & pss_fo_set,
  // Inputs&Outputs
  vec & fo_search_set,
  // Outpus
  double & ppm
) {
  uint16 len_pss = length(ROM_TABLES.pss_td[0]);
  ppm = NAN;

  uint32 len = length(s);

  uint16 num_fo_orig = length(fo_search_set);

  double th = 25*265.1154;

  double sampling_rate = FS_LTE/16; // LTE spec

  double len_time_subframe = 1e-3; //% 1ms. //LTE spec
  uint32 num_subframe_per_radioframe = 10;
  uint32 num_sample_per_subframe = (uint32)(len_time_subframe*sampling_rate);
  uint32 num_sample_per_radioframe = num_sample_per_subframe*num_subframe_per_radioframe;

  ivec hit_pss_fo_set_idx;
  ivec hit_time_idx;
  vec corr_val;

//  timeval tim;
//  gettimeofday(&tim, NULL);
//  double t1=tim.tv_sec+(tim.tv_usec/1000000.0);

  pss_moving_corr(s(0,(2*num_sample_per_radioframe-1)), fo_search_set, pss_fo_set, th, hit_pss_fo_set_idx, hit_time_idx, corr_val);

//  gettimeofday(&tim, NULL);
//  double t2=tim.tv_sec+(tim.tv_usec/1000000.0);
//  printf("%.6lf seconds elapsed\n", t2-t1);

//  cout << hit_pss_fo_set_idx << "\n";
//  cout << hit_time_idx << "\n";
//  cout << corr_val << "\n";
  if (length(hit_pss_fo_set_idx)==0){
    DBG( cout << "No strong enough PSS correlation peak.\n" );
    return;
  }

  int32 pss_period = num_sample_per_radioframe/2;

  uint16 num_fo = length(hit_pss_fo_set_idx);
  uint16 max_reserve_per_pss = 8;
  ivec pss_idx = hit_pss_fo_set_idx/(int)num_fo_orig;
  bmat pss_reserve_idx_bin(3, num_fo);
  for (uint16 i=0; i<3; i++) {
    pss_reserve_idx_bin.set_row(i, pss_idx==i);
    uint16 tmp_num = sum(to_ivec( pss_reserve_idx_bin.get_row(i) ) );
    if (tmp_num > max_reserve_per_pss){
      uint16 num_discard = tmp_num - max_reserve_per_pss;
      for ( int16 j=(num_fo-1); j>=0; j-- ) {
        if ( pss_reserve_idx_bin(i,j) == (bin)1) {
          pss_reserve_idx_bin(i,j) = (bin)0;
          num_discard--;
          if (num_discard==0) {
            break;
          }
        }
      }
    }
  }
//  cout << pss_reserve_idx_bin << "\n";

  uint16 num_drop_idx = 0;
  uint16 num_reserve_idx = 0;
  ivec drop_idx;
  ivec reserve_idx;
  bvec drop_idx_bin;
  bvec reserve_idx_bin;

  drop_idx.set_length(num_fo, false);
  drop_idx = to_ivec( sum( pss_reserve_idx_bin, 1 ) );
  num_drop_idx = sum(drop_idx);
  reserve_idx.set_length(num_drop_idx, false);
  num_reserve_idx = 0;
  for (uint16 i=0; i<num_fo; i++){
    if (drop_idx(i)==1) {
      reserve_idx(num_reserve_idx) = i;
      num_reserve_idx++;
    }
  }
//  cout << hit_pss_fo_set_idx << "\n";
//  cout << reserve_idx << "\n";
  hit_pss_fo_set_idx = hit_pss_fo_set_idx.get(reserve_idx);
  hit_pss_fo_set_idx.set_length(num_reserve_idx,true);
  hit_time_idx = hit_time_idx.get(reserve_idx);
  hit_time_idx.set_length(num_reserve_idx,true);
  corr_val = corr_val.get(reserve_idx);
  corr_val.set_length(num_reserve_idx,true);

  num_fo = num_reserve_idx;

  uint16 max_num_hit = ceil((double)len/(double)pss_period);
  imat time_location(max_num_hit, num_fo);
  time_location.zeros();
  time_location.set_row(0, hit_time_idx);
  mat hit_corr_val(max_num_hit, num_fo);
  hit_corr_val.zeros();
  hit_corr_val.set_row(0,corr_val);

  uint16 pss_count = 0;
  uint16 max_offset = 32;
  imat time_location_invalid_record(max_num_hit, num_fo);
  time_location_invalid_record.zeros();

  ivec next_location(num_fo);
  while(1) {
    next_location = time_location.get_row(pss_count) + pss_period;
    int32 min_next_location = min(next_location);
    int32 max_next_location = max(next_location);

    if( (max_next_location+max_offset) > (len-(len_pss-1)-1) ){
      break;
    }

    pss_fix_location_corr(s, min_next_location-max_offset, max_next_location+max_offset, pss_fo_set, hit_pss_fo_set_idx, hit_time_idx, corr_val);

    pss_count = pss_count + 1;
    time_location.set_row(pss_count, hit_time_idx);
    hit_corr_val.set_row(pss_count, corr_val);

//    for (uint16 k=0; k<num_fo; k++){
//      if (corr_val(k)<(th*3.0/4.0)){
//        time_location(pss_count, k) = next_location(k);
//        time_location_invalid_record(pss_count, k) = 1;
//      }
//    }
    bvec tmp_bin_vec = ( corr_val < (th*3.0/4.0) ) ;
    ivec tmp_logic_vec = to_ivec(tmp_bin_vec);
    ivec tmp_logic_vec_inv = to_ivec(tmp_bin_vec+(bin)1);

    ivec tmp_ivec = time_location.get_row(pss_count);
    tmp_ivec = elem_mult( tmp_ivec,tmp_logic_vec_inv );
    tmp_ivec = tmp_ivec + elem_mult( next_location, tmp_logic_vec );
    time_location.set_row(pss_count,tmp_ivec);

    time_location_invalid_record.set_row(pss_count, tmp_logic_vec);
  }
//  cout << pss_count << "\n";
//  cout << num_fo << "\n";

  time_location.set_size(pss_count+1, num_fo, true);
  hit_corr_val.set_size(pss_count+1, num_fo, true);
  time_location_invalid_record.set_size(pss_count+1, num_fo,true);

  vec ppm_store(num_fo);
  ivec valid_idx(num_fo);
  int32 min_dist = (int32)floor( ((double)len/(double)pss_period) * (1.0/2.0) );
  int16 ppm_idx = -1;

  for (uint16 i=0; i<num_fo; i++){
    uint16 col_idx = i;

    int16 sp = -1;
    for (uint16 j=0; j<=pss_count; j++){
      if (time_location_invalid_record(j,col_idx) == 0){
        sp = j;
        break;
      }
    }

    int16 ep = -1;
    for (int16 j=pss_count; j>=0; j--){
      if (time_location_invalid_record(j,col_idx) == 0){
        ep = j;
        break;
      }
    }

    if (sp==-1){
      continue;
    }

    if ( (ep-sp)<min_dist ){
      continue;
    }

    uint32 distance = time_location(ep,col_idx) - time_location(sp,col_idx);
    int32 len_ppm = ep-sp+1;
    len_ppm = pss_period*(len_ppm-1);
    double ppm_raw = 1.0e6 * ( (double)distance - (double)len_ppm )/(double)len_ppm;

    ppm_idx = ppm_idx + 1;
    ppm_store(ppm_idx) = ppm_raw;
    valid_idx(ppm_idx) = i;
  }

  if (ppm_idx == -1){
    DBG( cout << "No valid PSS hit sequence.\n" );
    return;
  }

  ppm_store.set_length(ppm_idx+1, true);
  valid_idx.set_length(ppm_idx+1, true);
//  cout << ppm_store << "\n";
//  cout << valid_idx << "\n";

  ivec valid_idx_backup = valid_idx;

  bool extra_frequency_flag = false;
  DBG( cout << "PPM: " << ppm_store << "\n" );

  pss_idx.set_length(ppm_idx+1, false);
  pss_idx = hit_pss_fo_set_idx.get(valid_idx)/(int)num_fo_orig;
  DBG( cout << "PSS: " << pss_idx << "\n" );

  if (ppm_idx==0){
    ppm = ppm_store(0);
    DBG( cout << "Total " << (ppm_idx+1) << " freq. idx for PPM: " << valid_idx << "\n" );

//    pss_idx = hit_pss_fo_set_idx.get(valid_idx)/(int)num_fo_orig;
    DBG( cout << "Total " << (ppm_idx+1) << "  pss. idx for PPM: " << pss_idx << "\n" );

    DBG( cout << "Average PPM: " << ppm << "\n" );

    uint16 idx_in_fo_search_set = hit_pss_fo_set_idx(valid_idx(0));
    double f_set = fo_search_set( idx_in_fo_search_set%num_fo_orig );
    DBG( cout << "Period PPM " << ppm << "PPM; f_set " << f_set/1.0e3 << "kHz\n" );
    DBG( cout << "Final PSS idx " << pss_idx << "\n"; );

    fo_search_set.set_length(1, false);
    fo_search_set(0) = f_set;
    return;
  } else if (ppm_idx==1) {
    ppm = ( ppm_store(0) + ppm_store(1) )/2.0;
    DBG( cout << "Total " << (ppm_idx+1) << " freq. idx for PPM: " << valid_idx << "\n" );

//    pss_idx = hit_pss_fo_set_idx.get(valid_idx)/(int)num_fo_orig;
    DBG( cout << "Total " << (ppm_idx+1) << "  pss. idx for PPM: " << pss_idx << "\n" );

    DBG( cout << "Average PPM: " << ppm << "\n" );

    if ( ( abs(ppm_store(1)-ppm_store(0))/abs(ppm_store(0)) ) > (1.0/20.0) ){
      ivec idx_in_fo_search_set = hit_pss_fo_set_idx.get(valid_idx);

      ivec fo_idx = idx_in_fo_search_set - pss_idx*(int)num_fo_orig;
      if ( fo_idx(0) == fo_idx(1) ) {
        DBG( cout << "Discard duplicated frequency idx " << idx_in_fo_search_set << "\n" );
        idx_in_fo_search_set.set_length(1,true);
      }

      vec f_set = fo_search_set.get( idx_in_fo_search_set - pss_idx*(int)num_fo_orig );
      DBG( cout << "Period PPM " << ppm << "PPM; f_set " << f_set/1.0e3 << "kHz\n" );
      DBG( cout << "Final PSS idx " << pss_idx << "\n"; );

      fo_search_set = f_set;
      return;
    }
  } else if ( variance(ppm_store) > 0.01 ) {
    double mean_ppm = mean(ppm_store);
    vec tmp = abs(ppm_store-mean_ppm);
    int32 max_idx;
    max(tmp, max_idx);
    drop_idx_bin = (ppm_store == ppm_store(max_idx));
    reserve_idx_bin = drop_idx_bin+(bin)1;
    num_drop_idx = sum(to_ivec(drop_idx_bin));
    if ( ( (double)num_drop_idx ) >= ( ((double)ppm_idx+1.0)*3.0/8.0 ) ){
      DBG( cout << "Too many PPM drops. Will not do it.\n" );
      extra_frequency_flag = true;
    } else {
      DBG( cout << "Drop PPM: " << drop_idx_bin << "\n" );

      // delete dropped elements
      num_reserve_idx = 0;
      reserve_idx.set_length(ppm_idx+1-num_drop_idx,false);
      for (uint16 i=0; i<=ppm_idx; i++) {
        if (reserve_idx_bin(i)==1){
          reserve_idx(num_reserve_idx) = i;
          num_reserve_idx++;
        }
      }

//      vec tmp_ppm_store = ppm_store;
//      ivec tmp_valid_idx = valid_idx;
//      uint16 tmp_ppm_idx = 0;
//      for (uint16 i=0; i<=ppm_idx; i++) {
//        if (tmp_valid_idx(i) != -1) {
//          ppm_store(tmp_ppm_idx) = tmp_ppm_store(i);
//          valid_idx(tmp_ppm_idx) = tmp_valid_idx(i);
//          tmp_ppm_idx++;
//        }
//      }
      ppm_idx = ppm_idx - num_drop_idx;
      ppm_store = ppm_store.get(reserve_idx);
      ppm_store.set_length(num_reserve_idx,true);
      valid_idx = valid_idx.get(reserve_idx);
      valid_idx.set_length(num_reserve_idx,true);
    }
    ppm = mean(ppm_store);
    DBG( cout << "Total " << (ppm_idx+1) << " freq. idx for PPM: " << valid_idx << "\n" );

    pss_idx.set_length(ppm_idx+1,false);
    pss_idx = hit_pss_fo_set_idx.get(valid_idx)/(int)num_fo_orig;
    DBG( cout << "Total " << (ppm_idx+1) << "  pss. idx for PPM: " << pss_idx << "\n" );

    DBG( cout << "Average PPM: " << ppm << "\n" );
  }

  vec sum_corr_val(ppm_idx+1);
  sum_corr_val.zeros();

  for (uint16 i=0; i<=ppm_idx; i++) {
    uint16 col_idx = valid_idx(i);
    sum_corr_val(i) = sum( elem_mult( to_vec( to_bvec(time_location_invalid_record.get_col(col_idx))+(bin)1 ), hit_corr_val.get_col(col_idx) ) );
//    for (uint16 j=0; j<=pss_count; j++) {
//      sum_corr_val(i) = sum_corr_val(i) + (time_location_invalid_record(j,col_idx)==0?hit_corr_val(j,col_idx):0.0);
//    }
  }

  int32 max_idx;
  max(sum_corr_val, max_idx);
  DBG( cout << "Freq. idx for f_set: " << valid_idx(max_idx) << "\n" );

  ivec idx_in_fo_search_set(1);
  idx_in_fo_search_set(0) = hit_pss_fo_set_idx(valid_idx(max_idx));

  if (extra_frequency_flag) {
    ivec extra_valid_idx;

    drop_idx.set_length(num_drop_idx,false);
    num_drop_idx = 0;
    for (uint16 i=0; i<=ppm_idx; i++) {
      if (drop_idx_bin(i)==1){
        drop_idx(num_drop_idx) = i;
        num_drop_idx++;
      }
    }

    reserve_idx.set_length(ppm_idx+1-num_drop_idx,false);
    num_reserve_idx = 0;
    for (uint16 i=0; i<=ppm_idx; i++) {
      if (reserve_idx_bin(i)==1){
        reserve_idx(num_reserve_idx) = i;
        num_reserve_idx++;
      }
    }

    if ( prod( abs(drop_idx - max_idx) ) == 0 ) {
      // delete dropped elements
      extra_valid_idx=valid_idx.get(reserve_idx);
    } else if ( prod( abs(reserve_idx - max_idx) ) == 0 ) {
      // delete dropped elements
      extra_valid_idx=valid_idx.get(drop_idx);
    } else {
      cout << "Abnormal!\n";
      return;
    }

    sum_corr_val.set_length(length(extra_valid_idx),false);
    for (uint16 i=0; i<length(extra_valid_idx); i++) {
      uint16 col_idx = extra_valid_idx(i);
      sum_corr_val(i) = sum( elem_mult( to_vec( to_bvec(time_location_invalid_record.get_col(col_idx))+(bin)1 ), hit_corr_val.get_col(col_idx) ) );
    }

    int32 max_idx;
    max(sum_corr_val, max_idx);
    DBG( cout << "Extra Freq. idx for f_set: " << extra_valid_idx(max_idx) << "\n" );

    pss_idx.set_length(1,false);
    pss_idx = hit_pss_fo_set_idx(extra_valid_idx(max_idx))/(int)num_fo_orig;
    DBG( cout << "Extra  pss. idx for f_set: " << pss_idx << "\n" );

//    cout << hit_pss_fo_set_idx( extra_valid_idx(max_idx) )/(int)num_fo_orig << "\n";

    idx_in_fo_search_set.set_length(2, true);
    idx_in_fo_search_set(1) = hit_pss_fo_set_idx(extra_valid_idx(max_idx));
  }

  //% add fo from other PSSs if there are
  pss_idx.set_length(length(idx_in_fo_search_set),false);
  pss_idx = idx_in_fo_search_set/(int)num_fo_orig;

  ivec extra_pss_set(3);
  extra_pss_set.zeros();
  for(uint16 idx=0; idx<3; idx++) {
    if ( prod( abs(pss_idx - idx) ) == 0 ) {
      extra_pss_set(idx) = 1;
    }
  }

//  cout << pss_idx << "\n";
//  cout << extra_pss_set << "\n";
  if (sum(extra_pss_set) < 3) {
    ivec exist_pss_idx = hit_pss_fo_set_idx.get(valid_idx_backup)/(int)num_fo_orig;
    for (uint16 extra_pss_idx=0; extra_pss_idx<3; extra_pss_idx++) {
      if (extra_pss_set(extra_pss_idx) == 0) {
        uint16 len_col_set = sum( to_ivec(exist_pss_idx==extra_pss_idx) );
        ivec col_set(len_col_set);

//        cout << exist_pss_idx << "\n";
//        cout << extra_pss_idx << "\n";
//        cout << len_col_set << "\n";

        if (len_col_set > 0) {
          len_col_set = 0;
          for(uint16 i=0; i<length(exist_pss_idx); i++) {
            if (exist_pss_idx(i)==extra_pss_idx){
              col_set(len_col_set) = i;
              len_col_set++;
            }
          }

//          cout << col_set << "\n";
//          cout << len_col_set << "\n";
//          cout << valid_idx_backup << "\n";

          sum_corr_val.set_length(len_col_set, false);
          for (uint16 i=0; i<len_col_set; i++) {
            uint16 col_idx = valid_idx_backup(col_set(i));
            sum_corr_val(i) = sum( elem_mult( to_vec( to_bvec(time_location_invalid_record.get_col(col_idx))+(bin)1 ), hit_corr_val.get_col(col_idx) ) );
          }
//          cout << sum_corr_val << "\n";

          int32 max_idx;
          max(sum_corr_val, max_idx);
          DBG( cout << "Extra Freq. idx for f_set (multi-PSS): " << valid_idx_backup( col_set(max_idx) ) << "\n" );

          pss_idx.set_length(1,false);
          pss_idx = hit_pss_fo_set_idx(valid_idx_backup( col_set(max_idx) ))/(int)num_fo_orig;
          DBG( cout << "Extra  pss. idx for f_set (multi-PSS): " << pss_idx << "\n" );

//          cout << hit_pss_fo_set_idx.get(valid_idx_backup( col_set(max_idx) )) << "\n";
          idx_in_fo_search_set.set_length(length(idx_in_fo_search_set)+1, true);
          idx_in_fo_search_set(length(idx_in_fo_search_set)-1) = hit_pss_fo_set_idx(valid_idx_backup( col_set(max_idx) ));
        }
      }
    }
  }

  pss_idx.set_length(length(idx_in_fo_search_set),false);
  pss_idx = idx_in_fo_search_set/(int)num_fo_orig;

  vec tmp_f_set = fo_search_set.get( idx_in_fo_search_set - pss_idx*(int)num_fo_orig );
  sort(tmp_f_set);
  fo_search_set(0) = tmp_f_set(0);
  uint16 len_final_fo_set = 1;
  for (uint16 i=1; i<length(tmp_f_set); i++ ) {
    if (tmp_f_set(i) != fo_search_set(len_final_fo_set-1)) {
      fo_search_set(len_final_fo_set) = tmp_f_set(i);
      len_final_fo_set++;
    } else {
      DBG( cout << "Discard duplicated frequency (multi-PSS) " << tmp_f_set(i)/1.0e3 << "kHz\n" );
    }
  }

  fo_search_set.set_length(len_final_fo_set, true);

  DBG( cout << "Period PPM " << ppm << "PPM; f_set " << fo_search_set/1.0e3 << "kHz\n" );
  DBG( cout << "Final PSS idx " << pss_idx << "\n"; );
}

// Correlate the received signal against all possible PSS and all possible
// frequency offsets.
// This is the main function that calls all of the previously declared
// subfunctions.
void xcorr_pss(
  // Inputs
  const cvec & capbuf,
  const vec & f_search_set,
  const uint8 & ds_comb_arm,
  const double & fc_requested,
  const double & fc_programmed,
  const double & fs_programmed,
  const cmat & pss_fo_set,
  // Outputs
  mat & xc_incoherent_collapsed_pow,
  imat & xc_incoherent_collapsed_frq,
  // Following used only for debugging...
  vf3d & xc_incoherent_single,
  vf3d & xc_incoherent,
  vec & sp_incoherent,
  vcf3d & xc,
  vec & sp,
  uint16 & n_comb_xc,
  uint16 & n_comb_sp,
  const bool & sampling_carrier_twist,
  double & k_factor
) {
  // Perform correlations
  xc_correlate(capbuf,f_search_set,fc_requested,fc_programmed,fs_programmed,sampling_carrier_twist,k_factor,xc);
//  xc_correlate_new(capbuf,f_search_set,pss_fo_set,xc);
  // Incoherently combine correlations
  xc_combine(capbuf,xc,fc_requested,fc_programmed,fs_programmed,f_search_set,xc_incoherent_single,n_comb_xc,sampling_carrier_twist,k_factor);
  // Combine according to delay spread
  xc_delay_spread(xc_incoherent_single,ds_comb_arm,xc_incoherent);
  // Estimate received signal power
  sp_est(capbuf,sp,sp_incoherent,n_comb_sp);
  // Search for peaks among all the frequency offsets.
  xc_peak_freq(xc_incoherent,xc_incoherent_collapsed_pow,xc_incoherent_collapsed_frq);
}

// Search through all the correlations and determine if any PSS were found.
void peak_search(
  // Inputs
  const mat & xc_incoherent_collapsed_pow,
  const imat & xc_incoherent_collapsed_frq,
  const vec & Z_th1,
  const vec & f_search_set,
  const double & fc_requested,
  const double & fc_programmed,
  const vf3d & xc_incoherent_single,
  const uint8 & ds_comb_arm,
  // Outputs
  list <Cell> & cells
) {
  // Create local copy we can write to and destroy.
  mat xc_incoherent_working=xc_incoherent_collapsed_pow;

  for (;;) {
    // Search for the largest peak. (Not the largest peak relative to
    // the detection threshold Z_th1.)
    ivec peak_ind_v;
    vec peak_pow_v=max(transpose(xc_incoherent_working),peak_ind_v);
    int32 peak_n_id_2;
    double peak_pow=max(peak_pow_v,peak_n_id_2);
    int32 peak_ind=peak_ind_v(peak_n_id_2);
    if (peak_pow<Z_th1(peak_ind)) {
      // This peak has too low of a received power. There are no more
      // interesting peaks. Break!
      break;
    }

    // A peak was found at location peak_ind and has frequency index
    // xc_incoherent_collapsed_frq(peak_n_id_2,peak_ind). This peak
    // is the sum of the energy within ds_comb_arm samples around this
    // peak location. From the samples within ds_comb_arm samples
    // around peak_ind, find the index with the highest power.
    double best_pow=-INFINITY;
    int16 best_ind=-1;
    for (uint16 t=peak_ind-ds_comb_arm;t<=peak_ind+ds_comb_arm;t++) {
      uint16 t_wrap=mod(t,9600);
      if (xc_incoherent_single[peak_n_id_2][t_wrap][xc_incoherent_collapsed_frq(peak_n_id_2,peak_ind)]>best_pow) {
        best_pow=xc_incoherent_single[peak_n_id_2][t_wrap][xc_incoherent_collapsed_frq(peak_n_id_2,peak_ind)];
        best_ind=t_wrap;
      }
    }

    // Record this peak for further processing
    Cell cell;
    cell.fc_requested=fc_requested;
    cell.fc_programmed=fc_programmed;
    cell.pss_pow=peak_pow;
    //cell.ind=peak_ind;
    cell.ind=best_ind;
    cell.freq=f_search_set(xc_incoherent_collapsed_frq(peak_n_id_2,peak_ind));
    cell.n_id_2=peak_n_id_2;
    cells.push_back(cell);

    // Cancel out the false peaks around this one.
    // No peaks with the same pss sequence are allowed within 274 samples of
    // this one.
    for (int16 t=-274;t<=274;t++) {
      //cout <<mod(peak_ind+t,9600)<<endl;
      xc_incoherent_working(peak_n_id_2,itpp_ext::matlab_mod(peak_ind+t,9600))=0;
    }
    // Cancel out other PSS sequences whose power is within 8dB of the current
    // sequence.
    double thresh=peak_pow*udb10(-8.0);
    for (uint8 n=0;n<=3;n++) {
      if (n==peak_n_id_2) {
        continue;
      }
      for (int16 t=-274;t<=274;t++) {
        if (xc_incoherent_working(peak_n_id_2,itpp_ext::matlab_mod(peak_ind+t,9600))<thresh) {
          xc_incoherent_working(peak_n_id_2,itpp_ext::matlab_mod(peak_ind+t,9600))=0;
        }
      }
    }
    // Because of the repetitive nature of the CRS, a PSS at offset I with power
    // P will produce correlation peaks during all CRS OFDM symbols with power
    // approximately P-14dB. Cancel them out.
    thresh=peak_pow*udb10(-12.0);
    for (uint8 r=0;r<3;r++) {
      for (uint16 c=0;c<9600;c++) {
        if (xc_incoherent_working(r,c)<thresh) {
          xc_incoherent_working(r,c)=0;
        }
      }
    }
  }
}

// Simple helper function to perform FOC and return only the subcarriers
// occupied by the PSS or SSS.
//
// Called by more than one function!
inline cvec extract_psss(
  const cvec td_samps,
  const double foc_freq,
  const double & k_factor,
  const double & fs_programmed
) {
  // Frequency shift
  cvec dft_in=fshift(td_samps,foc_freq,fs_programmed*k_factor);
  // Remove the 2 sample time offset
  dft_in=concat(dft_in(2,-1),dft_in.left(2));
  // DFT
  cvec dft_out=dft(dft_in);
  // Extract interesting samples.
  return concat(dft_out.right(31),dft_out.mid(1,31));
}

// Perform channel estimation and extract the SSS subcarriers.
void sss_detect_getce_sss(
  // Inputs
  const Cell & cell,
  const cvec & capbuf,
  const double & fc_requested,
  const double & fc_programmed,
  const double & fs_programmed,
  // Outputs
  vec & sss_h1_np_est,
  vec & sss_h2_np_est,
  cvec & sss_h1_nrm_est,
  cvec & sss_h2_nrm_est,
  cvec & sss_h1_ext_est,
  cvec & sss_h2_ext_est,
  const bool & sampling_carrier_twist,
  double & k_factor,
  const int & tdd_flag
) {
  // Local copies
  double peak_loc=cell.ind;
  const double peak_freq=cell.freq;
  const uint8 n_id_2_est=cell.n_id_2;

  if (sampling_carrier_twist) {
      k_factor=(fc_requested-peak_freq)/fc_programmed;
  }
  // Skip to the right by 5 subframes if there is no room here to detect
  // the SSS.
  int min_idx = 0;
  int sss_ext_offset = 0;
  int sss_nrm_offset = 0;
  if (tdd_flag == 1)
  {
    min_idx = 3*(128+32)+32;
    sss_ext_offset = 3*(128+32);
    sss_nrm_offset = 412;
  }
  else
  {
    min_idx = 163-9;
    sss_ext_offset = 128+32;
    sss_nrm_offset = 128+9;
  }

  if (peak_loc<min_idx) {
    peak_loc+=9600*k_factor;
  }
  // The location of all PSS's in the capture buffer where we also have
  // access to an SSS.
  vec pss_loc_set=itpp_ext::matlab_range(peak_loc,k_factor*9600,(double)capbuf.length()-125-9);
  uint16 n_pss=length(pss_loc_set);
  vec pss_np(n_pss);
  cmat h_raw(n_pss,62);
  cmat h_sm(n_pss,62);
  cmat sss_nrm_raw(n_pss,62);
  cmat sss_ext_raw(n_pss,62);
#ifndef NDEBUG
  pss_np=NAN;
  h_raw=NAN;
  h_sm=NAN;
  sss_nrm_raw=NAN;
  sss_ext_raw=NAN;
#endif

  for (uint16 k=0;k<n_pss;k++) {
    uint32 pss_loc=itpp::round_i(pss_loc_set(k));
    uint32 pss_dft_location=pss_loc+9-2;

    // Calculate channel response
    h_raw.set_row(k,elem_mult(extract_psss(capbuf.mid(pss_dft_location,128),-peak_freq,k_factor,fs_programmed),conj(ROM_TABLES.pss_fd[n_id_2_est])));
    // Basic smoothing. Average nearest 6 subcarriers.
    for (uint8 t=0;t<62;t++) {
      uint8 lt=MAX(0,t-6);
      uint8 rt=MIN(61,t+6);
      h_sm(k,t)=mean(h_raw.get_row(k).mid(lt,rt-lt+1));
    }

    // Estimate noise power
    pss_np(k)=sigpower(h_sm.get_row(k)-h_raw.get_row(k));

    // Calculate SSS in the frequency domain for extended and normal CP
    uint32 sss_dft_location=0;
    sss_dft_location=pss_dft_location-sss_ext_offset;
    sss_ext_raw.set_row(k,extract_psss(capbuf.mid(sss_dft_location,128),-peak_freq,k_factor,fs_programmed));

    sss_dft_location=pss_dft_location-sss_nrm_offset;
    sss_nrm_raw.set_row(k,extract_psss(capbuf.mid(sss_dft_location,128),-peak_freq,k_factor,fs_programmed));
  }

  // Combine results from different slots
  // Pre-allocate some vectors that are often used
  vec pss_np_inv_h1=1.0/pss_np(itpp_ext::matlab_range(0,2,n_pss-1));
  vec pss_np_inv_h2=1.0/pss_np(itpp_ext::matlab_range(1,2,n_pss-1));
  sss_h1_np_est.set_size(62);
  sss_h2_np_est.set_size(62);
  sss_h1_nrm_est.set_size(62);
  sss_h2_nrm_est.set_size(62);
  sss_h1_ext_est.set_size(62);
  sss_h2_ext_est.set_size(62);
#ifndef NDEBUG
  sss_h1_np_est=NAN;
  sss_h2_np_est=NAN;
  sss_h1_nrm_est=NAN;
  sss_h2_nrm_est=NAN;
  sss_h1_ext_est=NAN;
  sss_h2_ext_est=NAN;
#endif
  for (uint8 t=0;t<62;t++) {
    // First half (h1) and second half (h2) channel estimates.
    cvec h_sm_h1=h_sm.get_col(t).get(itpp_ext::matlab_range(0,2,n_pss-1));
    cvec h_sm_h2=h_sm.get_col(t).get(itpp_ext::matlab_range(1,2,n_pss-1));
    // Estimate noise power in each subcarrier
    sss_h1_np_est(t)=1/(1+sum(elem_mult(sqr(h_sm_h1),pss_np_inv_h1)));
    sss_h2_np_est(t)=1/(1+sum(elem_mult(sqr(h_sm_h2),pss_np_inv_h2)));
    // Estimate SSS assuming normal CP
    sss_h1_nrm_est(t)=sss_h1_np_est(t)*sum(elem_mult(conj(h_sm_h1),to_cvec(pss_np_inv_h1),sss_nrm_raw.get_col(t).get(itpp_ext::matlab_range(0,2,n_pss-1))));
    sss_h2_nrm_est(t)=sss_h2_np_est(t)*sum(elem_mult(conj(h_sm_h2),to_cvec(pss_np_inv_h2),sss_nrm_raw.get_col(t).get(itpp_ext::matlab_range(1,2,n_pss-1))));
    // Estimate SSS assuming extended CP
    sss_h1_ext_est(t)=sss_h1_np_est(t)*sum(elem_mult(conj(h_sm_h1),to_cvec(pss_np_inv_h1),sss_ext_raw.get_col(t).get(itpp_ext::matlab_range(0,2,n_pss-1))));
    sss_h2_ext_est(t)=sss_h2_np_est(t)*sum(elem_mult(conj(h_sm_h2),to_cvec(pss_np_inv_h2),sss_ext_raw.get_col(t).get(itpp_ext::matlab_range(1,2,n_pss-1))));
  }
}

// Helper function that compares the received SSS against one of the
// known transmitted SSS sequences.
double sss_detect_ml_helper(
  const vec & sss_h12_np_est,
  const cvec & sss_h12_est,
  const ivec & sss_h12_try_orig
) {
  cvec sss_h12_try(to_cvec(sss_h12_try_orig));

  // Compensate for phase errors between the est and try sequences
  double ang=arg(sum(elem_mult(conj(sss_h12_est),sss_h12_try)));
  sss_h12_try*=exp(J*-ang);

  // Calculate the log likelihood
  cvec diff=sss_h12_try-sss_h12_est;
  double log_lik=-sum(elem_div(elem_mult(real(diff),real(diff)),sss_h12_np_est))-sum(elem_div(elem_mult(imag(diff),imag(diff)),sss_h12_np_est));

  return log_lik;
}

// Perform maximum likelihood detection on the combined SSS signals.
void sss_detect_ml(
  // Inputs
  const Cell & cell,
  const vec & sss_h1_np_est,
  const vec & sss_h2_np_est,
  const cvec & sss_h1_nrm_est,
  const cvec & sss_h2_nrm_est,
  const cvec & sss_h1_ext_est,
  const cvec & sss_h2_ext_est,
  // Outputs
  mat & log_lik_nrm,
  mat & log_lik_ext
) {
  log_lik_nrm.set_size(168,2);
  log_lik_ext.set_size(168,2);
#ifndef NDEBUG
  log_lik_nrm=NAN;
  log_lik_ext=NAN;
#endif

  vec sss_h12_np_est=concat(sss_h1_np_est,sss_h2_np_est);
  cvec sss_h12_nrm_est=concat(sss_h1_nrm_est,sss_h2_nrm_est);
  cvec sss_h12_ext_est=concat(sss_h1_ext_est,sss_h2_ext_est);
  for (uint8 t=0;t<168;t++) {
    // Construct the SSS sequence that will be compared against the
    // received sequence.
    ivec sss_h1_try=ROM_TABLES.sss_fd(t,cell.n_id_2,0);
    ivec sss_h2_try=ROM_TABLES.sss_fd(t,cell.n_id_2,10);
    ivec sss_h12_try=concat(sss_h1_try,sss_h2_try);
    ivec sss_h21_try=concat(sss_h2_try,sss_h1_try);

    // Calculate log likelihood for normal/extended and 12/21 ordering
    // of SSS sequence.
    log_lik_nrm(t,0)=sss_detect_ml_helper(sss_h12_np_est,sss_h12_nrm_est,sss_h12_try);
    log_lik_nrm(t,1)=sss_detect_ml_helper(sss_h12_np_est,sss_h12_nrm_est,sss_h21_try);
    log_lik_ext(t,0)=sss_detect_ml_helper(sss_h12_np_est,sss_h12_ext_est,sss_h12_try);
    log_lik_ext(t,1)=sss_detect_ml_helper(sss_h12_np_est,sss_h12_ext_est,sss_h21_try);
  }
}

// Detect the SSS, if present
Cell sss_detect(
  // Inputs
  const Cell & cell,
  const cvec & capbuf,
  const double & thresh2_n_sigma,
  const double & fc_requested,
  const double & fc_programmed,
  const double & fs_programmed,
  // Only used for testing...
  vec & sss_h1_np_est,
  vec & sss_h2_np_est,
  cvec & sss_h1_nrm_est,
  cvec & sss_h2_nrm_est,
  cvec & sss_h1_ext_est,
  cvec & sss_h2_ext_est,
  mat & log_lik_nrm,
  mat & log_lik_ext,
  const bool & sampling_carrier_twist,
  double & k_factor,
  const int & tdd_flag
) {
  // Get the channel estimates and extract the raw SSS subcarriers
  sss_detect_getce_sss(cell,capbuf,fc_requested,fc_programmed,fs_programmed,sss_h1_np_est,sss_h2_np_est,sss_h1_nrm_est,sss_h2_nrm_est,sss_h1_ext_est,sss_h2_ext_est,sampling_carrier_twist,k_factor,tdd_flag);
  // Perform maximum likelihood detection
  sss_detect_ml(cell,sss_h1_np_est,sss_h2_np_est,sss_h1_nrm_est,sss_h2_nrm_est,sss_h1_ext_est,sss_h2_ext_est,log_lik_nrm,log_lik_ext);

  // Determine normal/ extended CP
  mat log_lik;
  cp_type_t::cp_type_t cp_type;
  int cp_type_flag = 0;
  if (max(max(log_lik_nrm))>max(max(log_lik_ext))) {
    log_lik=log_lik_nrm;
    cp_type=cp_type_t::NORMAL;
    cp_type_flag = 0;
  } else {
    log_lik=log_lik_ext;
    cp_type=cp_type_t::EXTENDED;
    cp_type_flag = 1;
  }

  // Locate the 'frame start' defined as the start of the CP of the frame.
  // The first DFT should be located at frame_start + cp_length.
  // It is expected (not guaranteed!) that a DFT performed at this
  // location will have a measured time offset of 2 samples.
  if (sampling_carrier_twist==1) {
    k_factor=(fc_requested-cell.freq)/fc_programmed;
  }
  double frame_start=0;

  if (tdd_flag == 1)
  {
      if (cp_type_flag == 0)
        frame_start=cell.ind+(-(2*(128+9)+1)-1920-2)*16/FS_LTE*fs_programmed*k_factor;// TDD NORMAL CP
      else
        frame_start=cell.ind+(-(2*(128+32))-1920-2)*16/FS_LTE*fs_programmed*k_factor; //TDD EXTENDED CP
  }
  else
    frame_start=cell.ind+(128+9-960-2)*16/FS_LTE*fs_programmed*k_factor;

  vec ll;
  if (max(log_lik.get_col(0))>max(log_lik.get_col(1))) {
    ll=log_lik.get_col(0);
  } else {
    ll=log_lik.get_col(1);
    frame_start=frame_start+9600*k_factor*16/FS_LTE*fs_programmed*k_factor;
  }
  frame_start=WRAP(frame_start,-0.5,(2*9600.0-0.5)*16/FS_LTE*fs_programmed*k_factor);

  // Estimate n_id_1.
  int32 n_id_1_est;
  double lik_final=max(ll,n_id_1_est);

  // Second threshold check to weed out some weak signals.
  Cell cell_out(cell);
  vec L=concat(cvectorize(log_lik_nrm),cvectorize(log_lik_ext));
  double lik_mean=mean(L);
  double lik_var=variance(L);
  if (lik_final>=lik_mean+pow(lik_var,0.5)*thresh2_n_sigma) {
    cell_out.n_id_1=n_id_1_est;
    cell_out.cp_type=cp_type;
    cell_out.frame_start=frame_start;
    cell_out.duplex_mode=tdd_flag;
  }

  return cell_out;
}

// Perform FOE using only the PSS and SSS.
// The PSS correlation peak gives us the frequency offset within 2.5kHz.
// The PSS/SSS can be used to estimate the frequency offset within a
// much finer resolution.
Cell pss_sss_foe(
  const Cell & cell_in,
  const cvec & capbuf,
  const double & fc_requested,
  const double & fc_programmed,
  const double & fs_programmed,
  const bool & sampling_carrier_twist,
  double & k_factor,
  const int & tdd_flag
) {
  if (sampling_carrier_twist){
    k_factor=(fc_requested-cell_in.freq)/fc_programmed;
  }

  // Determine where we can find both PSS and SSS
  uint16 pss_sss_dist;
  double first_sss_dft_location;
  if (cell_in.cp_type==cp_type_t::NORMAL) {
    if (tdd_flag==0)
    {
        pss_sss_dist=itpp::round_i((128+9)*16/FS_LTE*fs_programmed*k_factor); //FDD
        first_sss_dft_location=cell_in.frame_start+(960-128-9-128)*16/FS_LTE*fs_programmed*k_factor; //FDD
    }
    else
    {
        pss_sss_dist=itpp::round_i((3*(128+9)+1)*16/FS_LTE*fs_programmed*k_factor); //TDD
        first_sss_dft_location=cell_in.frame_start+(1920-128)*16/FS_LTE*fs_programmed*k_factor; //TDD
    }
  } else if (cell_in.cp_type==cp_type_t::EXTENDED) {
    if (tdd_flag==0)
    {
        pss_sss_dist=round_i((128+32)*16/FS_LTE*fs_programmed*k_factor); //FDD
        first_sss_dft_location=cell_in.frame_start+(960-128-32-128)*16/FS_LTE*fs_programmed*k_factor; //FDD
    }
    else
    {
        pss_sss_dist=round_i((3*(128+32))*16/FS_LTE*fs_programmed*k_factor); //TDD
        first_sss_dft_location=cell_in.frame_start+(1920-128)*16/FS_LTE*fs_programmed*k_factor; //TDD
    }
  } else {
    throw("Error... check code...");
  }
  uint8 sn;
  first_sss_dft_location=WRAP(first_sss_dft_location,-0.5,9600*2-0.5);
  if (first_sss_dft_location-9600*k_factor>-0.5) {
    first_sss_dft_location-=9600*k_factor;
    sn=10;
  } else {
    sn=0;
  }
  vec sss_dft_loc_set=itpp_ext::matlab_range(first_sss_dft_location,9600*16/FS_LTE*fs_programmed*k_factor,(double)(length(capbuf)-127-pss_sss_dist-100));
  uint16 n_sss=length(sss_dft_loc_set);

  // Loop around for each PSS/SSS pair
  sn=(1-(sn/10))*10;
  complex <double> M(0,0);
  cmat h_raw_fo_pss(n_sss,62);
  cmat h_sm(n_sss,62);
  cmat sss_raw_fo(n_sss,62);
  vec pss_np(n_sss);
#ifndef NDEBUG
  h_raw_fo_pss=NAN;
  h_sm=NAN;
  sss_raw_fo=NAN;
  pss_np=NAN;
#endif
  for (uint16 k=0;k<n_sss;k++) {
    sn=(1-(sn/10))*10;
    uint32 sss_dft_location=round_i(sss_dft_loc_set(k));

    // Find the PSS and use it to estimate the channel.
    uint32 pss_dft_location=sss_dft_location+pss_sss_dist;
    h_raw_fo_pss.set_row(k,extract_psss(capbuf.mid(pss_dft_location,128),-cell_in.freq,k_factor,fs_programmed));
    h_raw_fo_pss.set_row(k,elem_mult(h_raw_fo_pss.get_row(k),conj(ROM_TABLES.pss_fd[cell_in.n_id_2])));

    // Smoothing... Basic...
    for (uint8 t=0;t<62;t++) {
      uint8 lt=MAX(0,t-6);
      uint8 rt=MIN(61,t+6);
      h_sm(k,t)=mean(h_raw_fo_pss.get_row(k).mid(lt,rt-lt+1));
    }

    // Estimate noise power.
    pss_np(k)=sigpower(h_sm.get_row(k)-h_raw_fo_pss.get_row(k));

    // Calculate the SSS in the frequency domain
    sss_raw_fo.set_row(k,extract_psss(capbuf.mid(sss_dft_location,128),-cell_in.freq,k_factor,fs_programmed)*exp(J*pi*-cell_in.freq/(FS_LTE/16/2)*-pss_sss_dist));
    sss_raw_fo.set_row(k,elem_mult(sss_raw_fo.get_row(k),to_cvec(ROM_TABLES.sss_fd(cell_in.n_id_1,cell_in.n_id_2,sn))));

    // Compare PSS to SSS. With no frequency offset, arg(M) is zero.
    M=M+sum(elem_mult(
      conj(sss_raw_fo.get_row(k)),
      h_raw_fo_pss.get_row(k),
      to_cvec(elem_mult(
        sqr(h_sm.get_row(k)),
        1.0/(2*sqr(h_sm.get_row(k))*pss_np(k)+sqr(pss_np(k)))
      ))
    ));
  }

  // Store results.
  Cell cell_out(cell_in);
  cell_out.freq_fine=cell_in.freq+arg(M)/(2*pi)/(1/(fs_programmed*k_factor)*pss_sss_dist);
  return cell_out;
}

// Extract the time/ frequency grid.
//
// Note that this function is inefficient in that it returns the time/
// frequency grid for nearly all samples in the capture buffer whereas
// in reality, we are only interested in the OFDM symbols containing the MIB.
void extract_tfg(
  // Inputs
  const Cell & cell,
  const cvec & capbuf_raw,
  const double & fc_requested,
  const double & fc_programmed,
  const double & fs_programmed,
  // Outputs
  cmat & tfg,
  vec & tfg_timestamp,
  const bool & sampling_carrier_twist,
  double & k_factor
) {
  // Local shortcuts
  const double frame_start=cell.frame_start;
  const cp_type_t::cp_type_t cp_type=cell.cp_type;
  const double freq_fine=cell.freq_fine;

  // Derive some values
  // fc*k_factor is the receiver's actual RX center frequency.
  if (sampling_carrier_twist){
    k_factor=(fc_requested-cell.freq_fine)/fc_programmed;
  }
  const int8 n_symb_dl=cell.n_symb_dl();
  double dft_location;
  if (cp_type==cp_type_t::NORMAL) {
    dft_location=frame_start+10*16/FS_LTE*fs_programmed*k_factor;
  } else if (cp_type==cp_type_t::EXTENDED) {
    dft_location=frame_start+32*16/FS_LTE*fs_programmed*k_factor;
  } else {
    throw("Check code...");
  }

  // See if we can advance the frame start
  if (dft_location-.01*fs_programmed*k_factor>-0.5) {
    dft_location=dft_location-.01*fs_programmed*k_factor;
  }

  // Perform FOC
  cvec capbuf=fshift(capbuf_raw,-freq_fine,fs_programmed*k_factor);

  // Extract 6 frames + 2 slots worth of data
  uint16 n_ofdm_sym=6*10*2*n_symb_dl+2*n_symb_dl;
  tfg=cmat(n_ofdm_sym,72);
  tfg_timestamp=vec(n_ofdm_sym);
#ifndef NDEBUG
  tfg=NAN;
  tfg_timestamp=NAN;
#endif
  uint16 sym_num=0;
  for (uint16 t=0;t<n_ofdm_sym;t++) {
    cvec dft_out=dft(capbuf.mid(round_i(dft_location),128));
    tfg.set_row(t,concat(dft_out.right(36),dft_out.mid(1,36)));
    // Record the time offset where the DFT _should_ have been taken.
    // It was actually taken at the nearest sample boundary.
    tfg_timestamp(t)=dft_location;
    // Calculate location of next DFT
    if (n_symb_dl==6) {
      dft_location+=(128+32)*16/FS_LTE*fs_programmed*k_factor;
    } else {
      if (sym_num==6) {
        dft_location+=(128+10)*16/FS_LTE*fs_programmed*k_factor;
      } else {
        dft_location+=(128+9)*16/FS_LTE*fs_programmed*k_factor;
      }
      sym_num=mod(sym_num+1,7);
    }
  }

  // Compensate for the residual time offset.
  ivec cn=concat(itpp_ext::matlab_range(-36,-1),itpp_ext::matlab_range(1,36));
  for (uint16 t=0;t<n_ofdm_sym;t++) {
    double ideal_offset=tfg_timestamp(t);
    double actual_offset=round_i(ideal_offset);
    // How late were we in locating the DFT
    double late=actual_offset-ideal_offset;
    // Compensate for the improper location of the DFT
    tfg.set_row(t,elem_mult(tfg.get_row(t),exp((-J*2*pi*late/128)*cn)));
  }
  // At this point, tfg(t,:) contains the results of a DFT that was performed
  // at time offset tfg_timestamp(t). Note that tfg_timestamp(t) is not an
  // integer!
}

// Perform 'superfine' TOE/FOE/TOC/FOC.
//
// First, the residual frequency offset is measured using all the samples
// in the TFG. This frequency offset will be less noisy than the one produced
// by comparing the SSS and PSS, and will have significantly better performance
// in low SNR situations. In high SNR situations, the PSS/SSS measured
// frequency offset is sufficient.
//
// Then, FOC is performed. At this point, if the UE had transmitted the same
// symbol on all subcarriers of all OFDM symbols:
//   arg(E[conj(tfg(t,f))*tfg(t+1,f)])=0;
//
// Next, TOE is performed followed by TOC. At this point, if the UE had
// transmitted the same symbol on all subcarriers of all OFDM symbols:
//   arg(E[conj(tfg(t,f))*tfg(t,f+1)])=0;
Cell tfoec(
  // Inputs
  const Cell & cell,
  const cmat & tfg,
  const vec & tfg_timestamp,
  const double & fc_requested,
  const double & fc_programmed,
  const RS_DL & rs_dl,
  // Outputs
  cmat & tfg_comp,
  vec & tfg_comp_timestamp,
  const bool & sampling_carrier_twist,
  double & k_factor_residual
) {
  // Local shortcuts
  const int8 n_symb_dl=cell.n_symb_dl();
  uint16 n_ofdm=tfg.rows();
  uint16 n_slot=floor(((double)n_ofdm)/n_symb_dl);

  // Perform super-fine FOE
  complex <double> foe;
  for (uint8 sym_num=0;sym_num<=n_symb_dl-3;sym_num+=n_symb_dl-3) {
    // Extract all the RS and compensate for the known transmitted symbols.
    cmat rs_extracted(n_slot,12);
#ifndef NDEBUG
    rs_extracted=NAN;
#endif
    for (uint16 t=0;t<n_slot;t++) {
      // Extract RS
      rs_extracted.set_row(t,tfg.get_row(t*n_symb_dl+sym_num).get(itpp_ext::matlab_range((uint32)rs_dl.get_shift(mod(t,20),sym_num,0),(uint32)6,(uint32)71)));
      // Compensate
      rs_extracted.set_row(t,elem_mult(rs_extracted.get_row(t),conj(rs_dl.get_rs(mod(t,20),sym_num))));
    }
    // FOE, subcarrier by subcarrier.
    for (uint16 t=0;t<12;t++) {
      cvec col=rs_extracted.get_col(t);
      foe=foe+sum(elem_mult(conj(col(0,n_slot-2)),col(1,-1)));
    }
  }
  double residual_f=arg(foe)/(2*pi)/0.0005;

  // Perform FOC. Does not fix ICI!
  if (sampling_carrier_twist){
    k_factor_residual=(fc_requested-residual_f)/fc_programmed;
  }

  tfg_comp=cmat(n_ofdm,72);
#ifndef NDEBUG
  tfg_comp=NAN;
#endif
  tfg_comp_timestamp=k_factor_residual*tfg_timestamp;
  ivec cn=concat(itpp_ext::matlab_range(-36,-1),itpp_ext::matlab_range(1,36));
  for (uint16 t=0;t<n_ofdm;t++) {
    tfg_comp.set_row(t,tfg.get_row(t)*exp(J*2*pi*-residual_f*tfg_comp_timestamp(t)/(FS_LTE/16)));
    // How late were we in locating the DFT
    double late=tfg_timestamp(t)-tfg_comp_timestamp(t);
    // Compensate for the improper location of the DFT
    tfg_comp.set_row(t,elem_mult(tfg_comp.get_row(t),exp((-J*2*pi*late/128)*cn)));
  }

  // Perform TOE.
  // Implemented by comparing subcarrier k of one OFDM symbol with subcarrier
  // k+3 of another OFDM symbol. This is why FOE must be performed first.
  // Slightly less performance but faster execution time could be obtained
  // by comparing subcarrier k with subcarrier k+6 of the same OFDM symbol.
  complex <double> toe=0;
  for (uint16 t=0;t<2*n_slot-1;t++) {
    // Current OFDM symbol containing RS
    uint8 current_sym_num=(t&1)?(n_symb_dl-3):0;
    uint8 current_slot_num=mod((t>>1),20);
    uint16 current_offset=(t>>1)*n_symb_dl+current_sym_num;
    // Since we are using port 0, the shift is the same for all slots.
    uint8 current_shift=rs_dl.get_shift(0,current_sym_num,0);
    // Next OFDM symbol containing RS
    uint8 next_sym_num=((t+1)&1)?(n_symb_dl-3):0;
    uint8 next_slot_num=mod(((t+1)>>1),20);
    uint16 next_offset=((t+1)>>1)*n_symb_dl+next_sym_num;
    // Since we are using port 0, the shift is the same for all slots.
    uint8 next_shift=rs_dl.get_shift(0,next_sym_num,0);

    uint16 r1_offset,r2_offset;
    uint8 r1_shift,r2_shift;
    uint8 r1_sym_num,r2_sym_num;
    uint8 r1_slot_num,r2_slot_num;
    if (current_shift<next_shift) {
      r1_offset=current_offset;
      r1_shift=current_shift;
      r1_sym_num=current_sym_num;
      r1_slot_num=current_slot_num;
      r2_offset=next_offset;
      r2_shift=next_shift;
      r2_sym_num=next_sym_num;
      r2_slot_num=next_slot_num;
    } else {
      r1_offset=next_offset;
      r1_shift=next_shift;
      r1_sym_num=next_sym_num;
      r1_slot_num=next_slot_num;
      r2_offset=current_offset;
      r2_shift=current_shift;
      r2_sym_num=current_sym_num;
      r2_slot_num=current_slot_num;
    }
    cvec r1v=tfg_comp.get_row(r1_offset).get(itpp_ext::matlab_range(r1_shift,6,71));
    r1v=elem_mult(r1v,conj(rs_dl.get_rs(r1_slot_num,r1_sym_num)));
    cvec r2v=tfg_comp.get_row(r2_offset).get(itpp_ext::matlab_range(r2_shift,6,71));
    r2v=elem_mult(r2v,conj(rs_dl.get_rs(r2_slot_num,r2_sym_num)));
    complex<double> toe1=sum(elem_mult(conj(r1v),r2v));
    complex<double> toe2=sum(elem_mult(conj(r2v(0,10)),r1v(1,11)));
    toe+=toe1+toe2;
  }
  double delay=-arg(toe)/3/(2*pi/128);

  // Perform TOC
  cvec comp_vector=exp((J*2*pi/128*delay)*cn);
  for (uint16 t=0;t<n_ofdm;t++) {
    tfg_comp.set_row(t,elem_mult(tfg_comp.get_row(t),comp_vector));
  }

  Cell cell_out(cell);
  cell_out.freq_superfine=cell_out.freq_fine+residual_f;
  return cell_out;
}

// Helper function to remove entries that are out of bounds
void del_oob(
  ivec & v
) {
  int32 t=0;
  while (t<v.length()) {
    if ((v(t)<0)||(v(t)>11)) {
      v.del(t);
    } else {
      t++;
    }
  }
}

// Interpolate between the filtered channel estimates and the full
// time/frequency grid.
// This algorithm interpolates in the frequency domain and then
// in the time domain.
void ce_interp_freq_time(
  // Inputs
  const cmat & ce_filt,
  const ivec & shift,
  const int16 & n_ofdm,
  const int16 & n_rs_ofdm,
  const ivec & rs_set,
  // Outputs
  cmat & ce_tfg
) {
  // Interpolate in the frequency domain
  cmat ce_filt_frq(n_rs_ofdm,72);
#ifndef NDEBUG
  ce_filt_frq=NAN;
#endif
  for (int32 t=0;t<n_rs_ofdm;t++) {
    vec X=itpp_ext::matlab_range((double)shift(t&1),6.0,71.0);
    cvec Y=ce_filt.get_row(t);
    vec x=itpp_ext::matlab_range(0.0,71.0);
    ce_filt_frq.set_row(t,interp1(X,Y,x));
  }

  // Interpolate in the time domain
  ce_tfg=cmat(n_ofdm,72);
  for (uint8 t=0;t<=71;t++) {
    vec X=to_vec(rs_set);
    cvec Y=ce_filt_frq.get_col(t);
    vec x=itpp_ext::matlab_range(0.0,n_ofdm-1.0);
    ce_tfg.set_col(t,interp1(X,Y,x));
  }
}

// Interpolate between the filtered channel estimates and the full
// time/frequency grid.
// This algorithm first creates a uniformly spaced grid from the hexagonally
// spaced RS grid and then interpolates between the uniformly spaced grid.
void ce_interp_2stage(
  // Inputs
  const cmat & ce_filt,
  const ivec & shift,
  const int16 & n_ofdm,
  const int16 & n_rs_ofdm,
  const ivec & rs_set,
  // Outputs
  cmat & ce_tfg
) {
  // Fill in some missing samples to create a uniformly sampled grid.
  cmat ce_filt_exp(n_rs_ofdm,24);
#ifndef NDEBUG
  ce_filt_exp=NAN;
#endif
  bool current_row_leftmost=shift(0)<shift(1);
  for (uint16 t=0;t<n_rs_ofdm;t++) {
    for (int16 k=0;k<24;k++) {
      if ((k&1)==current_row_leftmost) {
        complex <double> total=0;
        uint8 n_total=0;
        // Up one
        if (t-1>=0) {
          total+=ce_filt(t-1,k>>1);
          n_total++;
        }
        // Down one
        if (t+1<n_rs_ofdm) {
          total+=ce_filt(t+1,k>>1);
          n_total++;
        }
        // Left
        if (((k-1)>>1)>=0) {
          total+=ce_filt(t,(k-1)>>1);
          n_total++;
        }
        // Right
        if (((k+1)>>1)<12) {
          total+=ce_filt(t,(k+1)>>1);
          n_total++;
        }
        ce_filt_exp(t,k)=total/n_total;
      } else {
        // There is already a sample here
        ce_filt_exp(t,k)=ce_filt(t,k>>1);
      }
    }
    current_row_leftmost=!current_row_leftmost;
  }
  ivec ce_filt_exp_x=itpp_ext::matlab_range(min(shift),3,71);

  // Interpolate (linearly) the uniformly sampled grid to create channel
  // estimates for every RE.
  ce_tfg=cmat(n_ofdm,72);
#ifndef NDEBUG
  ce_tfg=NAN;
#endif
  // First, interpolate in the frequency dimension.
  for (uint16 t=0;t<n_rs_ofdm;t++) {
    ivec X=ce_filt_exp_x;
    cvec Y=ce_filt_exp.get_row(t);
    ivec x=itpp_ext::matlab_range(0,71);
    ce_tfg.set_row(rs_set(t),interp1(to_vec(X),Y,to_vec(x)));
  }
  // Now, interpolate in the time dimension.
  for (uint16 t=0;t<72;t++) {
    ivec X=rs_set;
    cvec Y=ce_tfg.get_col(t).get(rs_set);
    ivec x=itpp_ext::matlab_range(0,n_ofdm-1);
    ce_tfg.set_col(t,interp1(to_vec(X),Y,to_vec(x)));
  }
}

// Helper function. Linearly interpolates the left/right edge samples so as to
// guarantee a vertex at the left and right subcarrier.
void ce_interp_hex_extend(
  vec & row_x,
  cvec & row_val
) {
  if (row_x(0)!=0) {
    row_val.ins(0,row_val(0)-row_x(0)*(row_val(1)-row_val(0))/(row_x(1)-row_x(0)));
    row_x.ins(0,0);
  }
  if (itpp_ext::last(row_x)!=71) {
    uint16 len=length(row_val);
    row_val.ins(len,row_val(len-1)+(71-itpp_ext::last(row_x))*(row_val(len-1)-row_val(len-2))/(row_x(len-1)-row_x(len-2)));
    row_x.ins(len,71);
  }
}

// Use Delaunay triangles to perform interpolation. (Similar to Matlab's
// griddata function.)
// This struct is only used by this function.
struct triangle_vertex_t {
  uint8 x_sc;
  uint16 y_symnum;
  complex <double> val;
};
void ce_interp_hex(
  // Inputs
  const cmat & ce_filt,
  const ivec & shift,
  const int16 & n_ofdm,
  const int16 & n_rs_ofdm,
  const ivec & rs_set,
  // Outputs
  cmat & ce_tfg
) {
  ce_tfg=cmat(n_ofdm,72);
#ifndef NDEBUG
  ce_tfg=NAN;
#endif
  for (uint16 t=0;t<=n_rs_ofdm-2;t++) {
    // Extract two rows and ensure that there are samples for subcarriers
    // 0 and 71.
    // In general, top_row_* is actually equal to bot_row_* from the
    // previous iteration.
    vec top_row_x=to_vec(itpp_ext::matlab_range((t&1)?shift(1):shift(0),6,71));
    cvec top_row_val=ce_filt.get_row(t);
    ce_interp_hex_extend(top_row_x,top_row_val);
    vec bot_row_x=to_vec(itpp_ext::matlab_range((t&1)?shift(0):shift(1),6,71));
    cvec bot_row_val=ce_filt.get_row(t+1);
    ce_interp_hex_extend(bot_row_x,bot_row_val);

    // First row is not handled inside the main loop.
    if (t==0) {
      ce_tfg.set_row(rs_set(0),interp1(top_row_x,top_row_val,itpp_ext::matlab_range(0.0,71.0)));
    }

    // Create initial triangle
    uint8 top_row_last_used;
    uint8 bot_row_last_used;
    vector <triangle_vertex_t> triangle(3);
    if (top_row_x(1)<bot_row_x(1)) {
      triangle[0].x_sc=top_row_x(0);
      triangle[0].y_symnum=rs_set(t);
      triangle[0].val=top_row_val(0);
      triangle[1].x_sc=bot_row_x(0);
      triangle[1].y_symnum=rs_set(t+1);
      triangle[1].val=bot_row_val(0);
      triangle[2].x_sc=top_row_x(1);
      triangle[2].y_symnum=rs_set(t);
      triangle[2].val=top_row_val(1);
      top_row_last_used=1;
      bot_row_last_used=0;
    } else {
      triangle[0].x_sc=bot_row_x(0);
      triangle[0].y_symnum=rs_set(t+1);
      triangle[0].val=bot_row_val(0);
      triangle[1].x_sc=top_row_x(0);
      triangle[1].y_symnum=rs_set(t);
      triangle[1].val=top_row_val(0);
      triangle[2].x_sc=bot_row_x(1);
      triangle[2].y_symnum=rs_set(t+1);
      triangle[2].val=bot_row_val(1);
      top_row_last_used=0;
      bot_row_last_used=1;
    }

    // This loop succesively creates triangles to cover the space between
    // top_row and bot_row.
    uint8 spacing=rs_set(t+1)-rs_set(t);
    vec x_offset(spacing+1);
    x_offset=0.0;
    while (true) {
      // Calculate the parameters of a plane passing through all points
      // of the triangle.
      // value=a_p*x_sc+b_p*y_symnum+c_p;
      cmat M(3,3);
      M(0,0)=triangle[0].x_sc;
      M(1,0)=triangle[1].x_sc;
      M(2,0)=triangle[2].x_sc;
      M(0,1)=triangle[0].y_symnum;
      M(1,1)=triangle[1].y_symnum;
      M(2,1)=triangle[2].y_symnum;
      M(0,2)=1;
      M(1,2)=1;
      M(2,2)=1;
      cvec V(3);
      V(0)=triangle[0].val;
      V(1)=triangle[1].val;
      V(2)=triangle[2].val;
      // In the future, inv(M) can be calculated directly for speed
      // since the last column of M is all ones.
      cvec abc=inv(M)*V;
      complex <double> a_p=abc(0);
      complex <double> b_p=abc(1);
      complex <double> c_p=abc(2);

      // Calculate the parameters of the line defining the rightmost
      // edge of the triangle.
      // x_sc=a_l*y_symnum+b_l;
      double x1=triangle[1].x_sc;
      double x2=triangle[2].x_sc;
      double y1=triangle[1].y_symnum;
      double y2=triangle[2].y_symnum;
      double a_l=(x1-x2)/(y1-y2);
      double b_l=(y1*x2-y2*x1)/(y1-y2);

      for (uint8 r=1;r<=spacing;r++) {
        while (x_offset(r)<=a_l*(rs_set(t)+r)+b_l) {
          ce_tfg(rs_set(t)+r,x_offset(r))=a_p*x_offset(r)+b_p*(rs_set(t)+r)+c_p;
          x_offset(r)++;
        }
      }

      if ((x_offset(1)==72)&&(itpp_ext::last(x_offset)==72)) {
        break;
      }

      // We are not done yet. Choose the points for the next triangle.
      if (triangle[2].y_symnum==rs_set(t)) {
        triangle[0]=triangle[1];
        triangle[1]=triangle[2];
        bot_row_last_used++;
        triangle[2].x_sc=bot_row_x(bot_row_last_used);
        triangle[2].y_symnum=rs_set(t+1);
        triangle[2].val=bot_row_val(bot_row_last_used);
      } else {
        triangle[0]=triangle[1];
        triangle[1]=triangle[2];
        top_row_last_used++;
        triangle[2].x_sc=top_row_x(top_row_last_used);
        triangle[2].y_symnum=rs_set(t);
        triangle[2].val=top_row_val(top_row_last_used);
      }
    }
  }

  // Rows before the first and after the last OFDM symbol with RS are
  // created simply by copying the nearest OFDM symbol with RS.
  for (uint8 t=0;t<rs_set(0);t++) {
    ce_tfg.set_row(t,ce_tfg.get_row(rs_set(0)));
  }
  for (uint16 t=itpp_ext::last(rs_set)+1;t<n_ofdm;t++) {
    ce_tfg.set_row(t,ce_tfg.get_row(itpp_ext::last(rs_set)));
  }
}

// Examine the time/frequency grid and perform channel estimation
// and filtering to produce channel estimates for every RE.
//
// Each invocation of this function performs CE/ filtering for one particular
// antenna port.
void chan_est(
  // Inputs
  const Cell & cell,
  const RS_DL & rs_dl,
  const cmat & tfg,
  const uint8 & port,
  // Outputs
  cmat & ce_tfg,
  double & np
) {
  const int8 n_symb_dl=cell.n_symb_dl();
  const uint16 n_ofdm=tfg.rows();

  // Set of OFDM symbols containing reference symbols.
  ivec rs_set;
  if (port<=1) {
    // There are better ways to implement this...
    Sort <int> sort;
    rs_set=concat(itpp_ext::matlab_range(0,n_symb_dl,n_ofdm-1),itpp_ext::matlab_range(n_symb_dl-3,n_symb_dl,n_ofdm-1));
    sort.sort(0,rs_set.length()-1,rs_set);
    //rs_set=reverse(rs_set);
  } else {
    rs_set=itpp_ext::matlab_range(1,n_symb_dl,n_ofdm-1);
  }
  const uint16 n_rs_ofdm=length(rs_set);

  // Extract the raw channel estimates. 12 raw channel estimates per OFDM
  // symbol containing RS.
  cmat ce_raw(n_rs_ofdm,12);
#ifndef NDEBUG
  ce_raw=NAN;
#endif
  uint8 slot_num=0;
  ivec shift(2);
  shift=-1000;
  for (uint16 t=0;t<n_rs_ofdm;t++) {
    uint8 sym_num=mod(rs_set(t),n_symb_dl);
    if (t<=1) {
      shift(t)=rs_dl.get_shift(mod(slot_num,20),sym_num,port);
    }

    cvec rs=rs_dl.get_rs(slot_num,sym_num);
    // Extract
    cvec raw_row=tfg.get_row(rs_set(t)).get(itpp_ext::matlab_range((int32)rs_dl.get_shift(mod(slot_num,20),sym_num,port),6,71));
    ce_raw.set_row(t,raw_row);
    // Compensate for known RS
    ce_raw.set_row(t,elem_mult(ce_raw.get_row(t),conj(rs)));
    if (((t&1)==1)||(port>=2)) {
      slot_num=mod(slot_num+1,20);
    }
  }

  // Simple filtering.
  //
  // 1   2   3   4   5   6
  //   7   8   9   A   B
  // C   D   E   F   G   H
  //
  // If the above is a representation of the location of the raw channel
  // estimates in the TFG, the filtered channel estimate for position 8
  // is the mean of the raw channel estimates for positions 2,3,7,8,9,D,
  // and E.
  cmat ce_filt(n_rs_ofdm,12);
  bool current_row_leftmost=shift(0)<shift(1);
  for (uint16 t=0;t<n_rs_ofdm;t++) {
    complex <double> total;
    uint8 n_total;
    for (uint16 k=0;k<12;k++) {
      // Current time offset
      ivec ind;
      ind=itpp_ext::matlab_range(k-1,k+1);
      del_oob(ind);
      total=sum(ce_raw.get_row(t).get(ind));
      n_total=length(ind);

      // Add in the previous and next time offset (if they exist)
      if (shift(0)==shift(1)) {
        ind=itpp_ext::matlab_range(k-1,k+1);
      } else {
        if (current_row_leftmost) {
          ind=itpp_ext::matlab_range(k-1,k);
        } else {
          ind=itpp_ext::matlab_range(k,k+1);
        }
      }
      del_oob(ind);
      // Previous time offset
      if (t!=0) {
        total+=sum(ce_raw.get_row(t-1).get(ind));
        n_total+=length(ind);
      }
      if (t!=n_rs_ofdm-1) {
        total+=sum(ce_raw.get_row(t+1).get(ind));
        n_total+=length(ind);
      }
      ce_filt(t,k)=total/n_total;
    }
    current_row_leftmost=!current_row_leftmost;
  }

  // Estimate noise power
  np=sigpower(cvectorize(ce_filt)-cvectorize(ce_raw));

  // There is no appreciable difference in performance between these
  // algorithms for high SNR values.
  //ce_interp_2stage(ce_filt,shift,n_ofdm,n_rs_ofdm,rs_set,ce_tfg);
  //ce_interp_freq_time(ce_filt,shift,n_ofdm,n_rs_ofdm,rs_set,ce_tfg);
  ce_interp_hex(ce_filt,shift,n_ofdm,n_rs_ofdm,rs_set,ce_tfg);
}

// Examine the time/ frequency grid and extract the RE that belong to the PBCH.
// Also return the channel estimates for that RE from all 4 possible eNodeB
// ports.
void pbch_extract(
  // Inputs
  const Cell & cell,
  const cmat & tfg,
  const Array <cmat> & ce,
  // Outputs
  cvec & pbch_sym,
  cmat & pbch_ce
) {
  // Shortcuts
  const int8 n_symb_dl=cell.n_symb_dl();
  const uint16 m_bit=(cell.cp_type==cp_type_t::NORMAL)?1920:1728;
  const uint8 v_shift_m3=mod(cell.n_id_cell(),3);

  pbch_sym=cvec(m_bit/2);
  // One channel estimate from each of 4 ports for each RE.
  pbch_ce=cmat(4,m_bit/2);
#ifndef NDEBUG
  pbch_sym=NAN;
  pbch_ce=NAN;
#endif
  uint32 idx=0;
  for (uint8 fr=0;fr<=3;fr++) {
    for (uint8 sym=0;sym<=3;sym++) {
      for (uint8 sc=0;sc<=71;sc++) {
        // Skip if there might be an RS occupying this position.
        if ((mod(sc,3)==v_shift_m3)&&((sym==0)||(sym==1)||((sym==3)&&(n_symb_dl==6)))) {
          continue;
        }
        uint16 sym_num=fr*10*2*n_symb_dl+n_symb_dl+sym;
        pbch_sym(idx)=tfg(sym_num,sc);
        pbch_ce(0,idx)=ce(0).get(sym_num,sc);
        pbch_ce(1,idx)=ce(1).get(sym_num,sc);
        pbch_ce(2,idx)=ce(2).get(sym_num,sc);
        pbch_ce(3,idx)=ce(3).get(sym_num,sc);
        idx++;
      }
    }
  }
  ASSERT(idx==m_bit/2);
}

// Blindly try various frame alignments and numbers of antennas to try
// to find a valid MIB.
Cell decode_mib(
  const Cell & cell,
  const cmat & tfg,
  const RS_DL & rs_dl
) {
  // Local shortcuts
  const int8 n_symb_dl=cell.n_symb_dl();

  Cell cell_out=cell;

  // Channel estimation. This is automatically performed for four antennas
  // and for every RE, not only the RE's that contain an MIB!!!
  Array <cmat> ce_tfg(4);
  vec np_v(4);
  chan_est(cell,rs_dl,tfg,0,ce_tfg(0),np_v(0));
  chan_est(cell,rs_dl,tfg,1,ce_tfg(1),np_v(1));
  chan_est(cell,rs_dl,tfg,2,ce_tfg(2),np_v(2));
  chan_est(cell,rs_dl,tfg,3,ce_tfg(3),np_v(3));

  // Try various frame offsets and number of TX antennas.
  bvec c_est;
  for (uint8 frame_timing_guess=0;frame_timing_guess<=3;frame_timing_guess++) {
    const uint16 ofdm_sym_set_start=frame_timing_guess*10*2*n_symb_dl;
    ivec ofdm_sym_set=itpp_ext::matlab_range(ofdm_sym_set_start,ofdm_sym_set_start+3*10*2*n_symb_dl+2*n_symb_dl-1);

    // Extract only the portion of the TFG containing the four frames
    // we are interested in.
    cmat tfg_try=tfg.get_rows(ofdm_sym_set);
    Array <cmat> ce_try(4);
    for (uint8 t=0;t<4;t++) {
      ce_try(t)=ce_tfg(t).get_rows(ofdm_sym_set);
    }

    // Extract symbols and channel estimates for the PBCH
    cvec pbch_sym;
    cmat pbch_ce;
    pbch_extract(cell,tfg_try,ce_try,pbch_sym,pbch_ce);

    // Try 1, 2, and 4 ports.
    vec np;
    cvec syms;
    for (uint8 n_ports_pre=1;n_ports_pre<=3;n_ports_pre++) {
      const uint8 n_ports=(n_ports_pre==3)?4:n_ports_pre;
      // Perform channel compensation and also estimate noise power in each
      // symbol.
      if (n_ports==1) {
        cvec gain=conj(elem_div(pbch_ce.get_row(0),to_cvec(sqr(pbch_ce.get_row(0)))));
        syms=elem_mult(pbch_sym,gain);
        np=np_v(0)*sqr(gain);
      } else {
        syms.set_size(length(pbch_sym));
        np.set_size(length(pbch_sym));
#ifndef NDEBUG
        syms=NAN;
        np=NAN;
#endif
        for (int32 t=0;t<length(syms);t+=2) {
          // Simple zero-forcing
          // http://en.wikipedia.org/wiki/Space-time_block_coding_based_transmit_diversity
          complex <double> h1,h2;
          double np_temp;
          if (n_ports==2) {
            h1=(pbch_ce(0,t)+pbch_ce(0,t+1))/2;
            h2=(pbch_ce(1,t)+pbch_ce(1,t+1))/2;
            np_temp=mean(np_v(0,1));
          } else {
            if (mod(t,4)==0) {
              h1=(pbch_ce(0,t)+pbch_ce(0,t+1))/2;
              h2=(pbch_ce(2,t)+pbch_ce(2,t+1))/2;
              np_temp=(np_v(0)+np_v(2))/2;
            } else {
              h1=(pbch_ce(1,t)+pbch_ce(1,t+1))/2;
              h2=(pbch_ce(3,t)+pbch_ce(3,t+1))/2;
              np_temp=(np_v(1)+np_v(3))/2;
            }
          }
          complex <double> x1=pbch_sym(t);
          complex <double> x2=pbch_sym(t+1);
          double scale=pow(h1.real(),2)+pow(h1.imag(),2)+pow(h2.real(),2)+pow(h2.imag(),2);
          syms(t)=(conj(h1)*x1+h2*conj(x2))/scale;
          syms(t+1)=conj((-conj(h2)*x1+h1*conj(x2))/scale);
          np(t)=(pow(abs(h1)/scale,2)+pow(abs(h2)/scale,2))*np_temp;
          np(t+1)=np(t);
        }
        // 3dB factor comes from precoding for transmit diversity
        syms=syms*pow(2,0.5);
      }

      // Extract the bits from the complex modulated symbols.
      vec e_est=lte_demodulate(syms,np,modulation_t::QAM);
      // Unscramble
      bvec scr=lte_pn(cell.n_id_cell(),length(e_est));
      for (int32 t=0;t<length(e_est);t++) {
        if (scr(t)) e_est(t)=-e_est(t);
      }
      // Undo ratematching
      mat d_est=lte_conv_deratematch(e_est,40);
      // Decode
      c_est=lte_conv_decode(d_est);
      // Calculate received CRC
      bvec crc_est=lte_calc_crc(c_est(0,23),CRC16);
      // Apply CRC mask
      if (n_ports==2) {
        for (uint8 t=0;t<16;t++) {
          crc_est(t)=1-((int)crc_est(t));
        }
      } else if (n_ports==4) {
        for (uint8 t=1;t<length(crc_est);t+=2) {
          crc_est(t)=1-((int)crc_est(t));
        }
      }
      // Did we find it?
      if (crc_est==c_est(24,-1)) {
        // YES!
        cell_out.n_ports=n_ports;
        // Unpack the MIB
        ivec c_est_ivec=to_ivec(c_est);
        // DL bandwidth
        const uint8 bw_packed=c_est_ivec(0)*4+c_est_ivec(1)*2+c_est_ivec(2);
        switch (bw_packed) {
          case 0:
            cell_out.n_rb_dl=6;
            break;
          case 1:
            cell_out.n_rb_dl=15;
            break;
          case 2:
            cell_out.n_rb_dl=25;
            break;
          case 3:
            cell_out.n_rb_dl=50;
            break;
          case 4:
            cell_out.n_rb_dl=75;
            break;
          case 5:
            cell_out.n_rb_dl=100;
            break;
        }
        // PHICH duration
        cell_out.phich_duration=c_est_ivec(3)?phich_duration_t::EXTENDED:phich_duration_t::NORMAL;
        // PHICH resources
        uint8 phich_res=c_est_ivec(4)*2+c_est_ivec(5);
        switch (phich_res) {
          case 0:
            cell_out.phich_resource=phich_resource_t::oneSixth;
            break;
          case 1:
            cell_out.phich_resource=phich_resource_t::half;
            break;
          case 2:
            cell_out.phich_resource=phich_resource_t::one;
            break;
          case 3:
            cell_out.phich_resource=phich_resource_t::two;
            break;
        }
        // Calculate SFN
        int8 sfn_temp=128*c_est_ivec(6)+64*c_est_ivec(7)+32*c_est_ivec(8)+16*c_est_ivec(9)+8*c_est_ivec(10)+4*c_est_ivec(11)+2*c_est_ivec(12)+c_est_ivec(13);
        cell_out.sfn=itpp_ext::matlab_mod(sfn_temp*4-frame_timing_guess,1024);
        return cell_out;
      }
    }
  }

  return cell_out;
}

