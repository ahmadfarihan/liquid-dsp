/*
 * Copyright (c) 2007, 2008, 2009, 2010, 2013 Joseph Gaeddert
 *
 * This file is part of liquid.
 *
 * liquid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * liquid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with liquid.  If not, see <http://www.gnu.org/licenses/>.
 */

//
// numerically-controlled oscillator (nco) API, 16-bit fixed-point precision
//

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include "liquidfpm.h"
#include "liquid.internal.h"

#define NCO_PLL_BANDWIDTH_DEFAULT   (0.1)
#define NCO_PLL_GAIN_DEFAULT        (1000)

#define LIQUID_DEBUG_NCO            (0)

struct nco_crcq16_s {
    liquid_ncotype type;
    q16_t theta;            // NCO phase
    q16_t d_theta;          // NCO frequency
    q16_t sintab[256];      // sine table
    unsigned int index; // table index
    q16_t sine;
    q16_t cosine;
    void (*compute_sincos)(nco_crcq16 _q);

    // phase-locked loop
    q16_t bandwidth;
    q16_t zeta;
    q16_t a[3];
    q16_t b[3];
    iirfiltsos_rrrq16 pll_filter;    // phase-locked loop filter
};

// 
// forward declaration of internal methods
//

// constrain phase/frequency to be in [-pi,pi)
void nco_crcq16_constrain_phase(nco_crcq16 _q);
void nco_crcq16_constrain_frequency(nco_crcq16 _q);

// compute trigonometric functions for nco/vco type
void nco_crcq16_compute_sincos_nco(nco_crcq16 _q);
void nco_crcq16_compute_sincos_vco(nco_crcq16 _q);

// reset internal phase-locked loop filter
void nco_crcq16_pll_reset(nco_crcq16 _q);

// create nco/vco object
nco_crcq16 nco_crcq16_create(liquid_ncotype _type)
{
    nco_crcq16 q = (nco_crcq16) malloc(sizeof(struct nco_crcq16_s));
    q->type = _type;

    // initialize sine table
    unsigned int i;
    for (i=0; i<256; i++)
        q->sintab[i] = q16_float_to_fixed(sinf(2.0f*M_PI*(float)(i)/256.0f));

    // set default pll bandwidth
    q->a[0] = q16_one;  q->b[0] = 0;
    q->a[1] = 0;        q->b[1] = 0;
    q->a[2] = 0;        q->b[2] = 0;
    q->pll_filter = iirfiltsos_rrrq16_create(q->b, q->a);
    nco_crcq16_reset(q);
    nco_crcq16_pll_set_bandwidth(q, NCO_PLL_BANDWIDTH_DEFAULT);

    // set internal method
    if (q->type == LIQUID_NCO) {
        q->compute_sincos = &nco_crcq16_compute_sincos_nco;
    } else if (q->type == LIQUID_VCO) {
        q->compute_sincos = &nco_crcq16_compute_sincos_vco;
    } else {
        fprintf(stderr,"error: nco_crcq16_create(), unknown type : %u\n", q->type);
        exit(1);
    }

    return q;
}

// destroy nco object
void nco_crcq16_destroy(nco_crcq16 _q)
{
    iirfiltsos_rrrq16_destroy(_q->pll_filter);
    free(_q);
}

// reset internal state of nco object
void nco_crcq16_reset(nco_crcq16 _q)
{
    _q->theta = 0;
    _q->d_theta = 0;

    // reset sine table index
    _q->index = 0;

    // set internal sine, cosine values
    _q->sine = 0;
    _q->cosine = 1;

    // reset pll filter state
    nco_crcq16_pll_reset(_q);
}

// set frequency of nco object
void nco_crcq16_set_frequency(nco_crcq16 _q,
                         q16_t _f)
{
    _q->d_theta = _f;
}

// adjust frequency of nco object
void nco_crcq16_adjust_frequency(nco_crcq16 _q,
                            q16_t _df)
{
    _q->d_theta += _df;
}

// set phase of nco object, constraining phase
void nco_crcq16_set_phase(nco_crcq16 _q, q16_t _phi)
{
    _q->theta = _phi;
    nco_crcq16_constrain_phase(_q);
}

// adjust phase of nco object, constraining phase
void nco_crcq16_adjust_phase(nco_crcq16 _q, q16_t _dphi)
{
    _q->theta += _dphi;
    nco_crcq16_constrain_phase(_q);
}

// increment internal phase of nco object
void nco_crcq16_step(nco_crcq16 _q)
{
    _q->theta += _q->d_theta;
    nco_crcq16_constrain_phase(_q);
}

// get phase
q16_t nco_crcq16_get_phase(nco_crcq16 _q)
{
    return _q->theta;
}

// ge frequency
q16_t nco_crcq16_get_frequency(nco_crcq16 _q)
{
    // return both internal NCO phase step as well
    // as PLL phase step
    return _q->d_theta;
}


// TODO : compute sine, cosine internally
q16_t nco_crcq16_sin(nco_crcq16 _q)
{
    // compute internal sin, cos
    _q->compute_sincos(_q);

    // return resulting cosine component
    return _q->sine;
}

q16_t nco_crcq16_cos(nco_crcq16 _q)
{
    // compute internal sin, cos
    _q->compute_sincos(_q);

    // return resulting cosine component
    return _q->cosine;
}

// compute sin, cos of internal phase
void nco_crcq16_sincos(nco_crcq16 _q, q16_t* _s, q16_t* _c)
{
    // compute sine, cosine internally, calling implementation-
    // specific function (nco, vco)
    _q->compute_sincos(_q);

    // return result
    *_s = _q->sine;
    *_c = _q->cosine;
}

// compute complex exponential of internal phase
void nco_crcq16_cexpf(nco_crcq16 _q,
                      cq16_t *  _y)
{
    // compute sine, cosine internally, calling implementation-
    // specific function (nco, vco)
    _q->compute_sincos(_q);

    // set _y[0] to [cos(theta) + _Complex_I*sin(theta)]
    _y->real = _q->cosine;
    _y->imag = _q->sine;
#if 0
    *_y = _q->cosine + _Complex_I*(_q->sine);
#endif
}

// pll methods

// reset pll state, retaining base frequency
void nco_crcq16_pll_reset(nco_crcq16 _q)
{
    // clear phase-locked loop filter
    iirfiltsos_rrrq16_clear(_q->pll_filter);
}

// set pll bandwidth
void nco_crcq16_pll_set_bandwidth(nco_crcq16 _q,
                             q16_t     _b)
{
    // validate input
    if (_b < 0.0f) {
        fprintf(stderr,"error: nco_pll_set_bandwidth(), bandwidth must be positive\n");
        exit(1);
    }

    _q->bandwidth = _b;
    _q->zeta = 1/sqrtf(2.0f);

    float K     = NCO_PLL_GAIN_DEFAULT; // gain
    float zeta  = 1.0f / sqrtf(2.0f);   // damping factor
    float wn    = _b;                   // natural frequency
    float t1    = K/(wn*wn);            // 
    float t2    = 2*zeta/wn - 1/K;      //

    // feed-forward coefficients
    _q->b[0] =  2*K*(1.+t2/2.0f);
    _q->b[1] =  2*K*2.;
    _q->b[2] =  2*K*(1.-t2/2.0f);

    // feed-back coefficients
    _q->a[0] =  1. + t1/2.0f;
    _q->a[1] = -1. + t1/2.0f;
    _q->a[2] =  0.0f;
    
    iirfiltsos_rrrq16_set_coefficients(_q->pll_filter, _q->b, _q->a);
}

// advance pll phase
//  _q      :   nco object
//  _dphi   :   phase error
void nco_crcq16_pll_step(nco_crcq16 _q,
                    q16_t     _dphi)
{
    // execute internal filter (direct form I)
    q16_t error_filtered = 0.0f;
    iirfiltsos_rrrq16_execute_df1(_q->pll_filter,
                                 _dphi,
                                 &error_filtered);

    // increase frequency proportional to error
    nco_crcq16_adjust_frequency(_q, error_filtered);

    // constrain frequency
    //nco_crcq16_constrain_frequency(_q);
}

// mixing functions

// Rotate input vector up by NCO angle, y = x exp{+j theta}
//  _q      :   nco object
//  _x      :   input sample
//  _y      :   output sample
void nco_crcq16_mix_up(nco_crcq16 _q,
                  cq16_t    _x,
                  cq16_t *  _y)
{
    // compute sine, cosine internally, calling implementation-
    // specific function (nco, vco)
    _q->compute_sincos(_q);

    // multiply _x by [cos(theta) + _Complex_I*sin(theta)]
    cq16_t v;
    v.real = _q->cosine;
    v.imag = _q->sine;
    *_y = cq16_mul(_x, v);
#if 0
    *_y = _x * (_q->cosine + _Complex_I*(_q->sine));
#endif
}

// Rotate input vector down by NCO angle, y = x exp{-j theta}
//  _q      :   nco object
//  _x      :   input sample
//  _y      :   output sample
void nco_crcq16_mix_down(nco_crcq16 _q,
                    cq16_t _x,
                    cq16_t *_y)
{
    // compute sine, cosine internally
    _q->compute_sincos(_q);

    // multiply _x by [cos(-theta) + _Complex_I*sin(-theta)]
    cq16_t v;
    v.real = _q->cosine;
    v.imag = -(_q->sine);
    *_y = cq16_mul(_x, v);
#if 0
    *_y = _x * (_q->cosine - _Complex_I*(_q->sine));
#endif
}


// Rotate input vector array up by NCO angle:
//      y(t) = x(t) exp{+j (f*t + theta)}
// TODO : implement NCO/VCO-specific versions
//  _q      :   nco object
//  _x      :   input array [size: _n x 1]
//  _y      :   output sample [size: _n x 1]
//  _n      :   number of input, output samples
void nco_crcq16_mix_block_up(nco_crcq16   _q,
                             cq16_t *     _x,
                             cq16_t *     _y,
                             unsigned int _n)
{
    unsigned int i;
    for (i=0; i<_n; i++)
        nco_crcq16_mix_up(_q, _x[i], &_y[i]);
#if 0
    q16_t theta =   _q->theta;
    q16_t d_theta = _q->d_theta;
    for (i=0; i<_n; i++) {
        // multiply _x[i] by [cos(theta) + _Complex_I*sin(theta)]
        _y[i] = _x[i] * liquid_cexpjf(theta);
        
        theta += d_theta;
    }

    nco_crcq16_set_phase(_q, theta);
#endif
}

// Rotate input vector array down by NCO angle:
//      y(t) = x(t) exp{-j (f*t + theta)}
// TODO : implement NCO/VCO-specific versions
//  _q      :   nco object
//  _x      :   input array [size: _n x 1]
//  _y      :   output sample [size: _n x 1]
//  _n      :   number of input, output samples
void nco_crcq16_mix_block_down(nco_crcq16   _q,
                               cq16_t *     _x,
                               cq16_t *     _y,
                               unsigned int _n)
{
    unsigned int i;
    for (i=0; i<_n; i++)
        nco_crcq16_mix_down(_q, _x[i], &_y[i]);
#if 0
    q16_t theta =   _q->theta;
    q16_t d_theta = _q->d_theta;
    for (i=0; i<_n; i++) {
        // multiply _x[i] by [cos(-theta) + _Complex_I*sin(-theta)]
        _y[i] = _x[i] * liquid_cexpjf(-theta);
        
        theta += d_theta;
    }

    nco_crcq16_set_phase(_q, theta);
#endif
}

//
// internal methods
//

// constrain frequency of NCO object to be in (-pi,pi)
void nco_crcq16_constrain_frequency(nco_crcq16 _q)
{
    if (_q->d_theta > q16_pi)
        _q->d_theta -= q16_2pi;
    else if (_q->d_theta < -q16_pi)
        _q->d_theta += q16_2pi;
}

// constrain phase of NCO object to be in (-pi,pi)
void nco_crcq16_constrain_phase(nco_crcq16 _q)
{
    if (_q->theta > q16_pi)
        _q->theta -= q16_2pi;
    else if (_q->theta < -q16_pi)
        _q->theta += q16_2pi;
}

// compute sin, cos of internal phase of nco
void nco_crcq16_compute_sincos_nco(nco_crcq16 _q)
{
    // assume phase is constrained to be in (-pi,pi)

    // compute index
    // NOTE : 40.743665 ~ 256 / (2*pi)
    // NOTE : add 512 to ensure positive value, add 0.5 for rounding precision
    // TODO : move away from floating-point specific code
    _q->index = ((unsigned int)((_q->theta)*40.743665f + 512.0f + 0.5f))&0xff;
    assert(_q->index < 256);
    
    _q->sine = _q->sintab[_q->index];
    _q->cosine = _q->sintab[(_q->index+64)&0xff];
}

// compute sin, cos of internal phase of vco
void nco_crcq16_compute_sincos_vco(nco_crcq16 _q)
{
    // TODO:...
    _q->sine   = q16_sin(_q->theta);
    _q->cosine = q16_cos(_q->theta);
}

