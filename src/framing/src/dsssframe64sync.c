/*
 * Copyright (c) 2007 - 2022 Joseph Gaeddert
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// basic frame synchronizer with 8 bytes header and 64 bytes payload

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <assert.h>

#include "liquid.internal.h"

// internal update
int dsssframe64sync_step(dsssframe64sync _q,
                         float complex * _buf,
                         unsigned int    _buf_len);

// internal decode received frame, update statistics, invoke callback
int dsssframe64sync_decode(dsssframe64sync _q);

// internal synchronization callback, return 0:continue, 1:reset
int dsssframe64sync_callback_internal(float complex * _buf,
                                      unsigned int    _buf_len,
                                      void *          _context)
{ return dsssframe64sync_step((dsssframe64sync) _context, _buf, _buf_len); }

// dsssframe64sync object structure
struct dsssframe64sync_s {
    // callback
    framesync_callback  callback;   // user-defined callback function
    void *              context;    // user-defined data structure
    unsigned int    m;                  // filter delay (symbols)
    float           beta;               // filter excess bandwidth factor

    framesyncstats_s    framesyncstats; // frame statistic object (synchronizer)
    framedatastats_s    framedatastats; // frame statistic object (packet statistics)

    // preamble
    float complex preamble_pn[1024];  // known 1024-symbol p/n sequence
    float complex preamble_rx[1024];  // received p/n symbols

    // payload decoder
    float complex payload_rx [630]; // received payload symbols with pilots
    float complex payload_sym[600]; // received payload symbols
    unsigned char payload_dec[ 72]; // decoded payload bytes

    qdsync_cccf     detector;       // frame detector
    msequence       ms;                 // spreading sequence generator
    float complex sym_despread;     // despread symbol

    qpacketmodem  dec;              // packet demodulator/decoder
    qpilotsync    pilotsync;        // pilot extraction, carrier recovery

    // status variables
    enum {
        dsssframe64sync_STATE_DETECTFRAME=0,    // detect frame (seek p/n sequence)
        dsssframe64sync_STATE_RXPREAMBLE,       // receive p/n sequence
        dsssframe64sync_STATE_RXPAYLOAD,        // receive payload data
    }            state;
    unsigned int preamble_counter;  // counter: num of p/n syms received
    unsigned int chip_counter;      // counter: num of payload chips received for a single symbol
    unsigned int payload_counter;   // counter: num of payload syms received
};

// create dsssframe64sync object
//  _callback       :   callback function invoked when frame is received
//  _context        :   user-defined data object passed to callback
dsssframe64sync dsssframe64sync_create(framesync_callback _callback,
                                       void *             _context)
{
    dsssframe64sync q = (dsssframe64sync) malloc(sizeof(struct dsssframe64sync_s));
    q->callback = _callback;
    q->context  = _context;
    q->m        = 15;   // filter delay (symbols)
    q->beta     = 0.20f;// excess bandwidth factor

    unsigned int i;

    // generate p/n sequence
    q->ms = msequence_create(11, 0x0805, 1);
    for (i=0; i<1024; i++) {
        q->preamble_pn[i]  = (msequence_advance(q->ms) ? M_SQRT1_2 : -M_SQRT1_2);
        q->preamble_pn[i] += (msequence_advance(q->ms) ? M_SQRT1_2 : -M_SQRT1_2)*_Complex_I;
    }

    // create frame detector
    unsigned int k = 2;    // samples/symbol
    q->detector = qdsync_cccf_create_linear(q->preamble_pn, 1024, LIQUID_FIRFILT_ARKAISER, k, q->m, q->beta,
        dsssframe64sync_callback_internal, (void*)q);
    qdsync_cccf_set_threshold(q->detector, 0.5f);

    // create payload demodulator/decoder object
    int check      = LIQUID_CRC_24;
    int fec0       = LIQUID_FEC_NONE;
    int fec1       = LIQUID_FEC_GOLAY2412;
    int mod_scheme = LIQUID_MODEM_QPSK;
    q->dec         = qpacketmodem_create();
    qpacketmodem_configure(q->dec, 72, check, fec0, fec1, mod_scheme);
    //qpacketmodem_print(q->dec);
    assert( qpacketmodem_get_frame_len(q->dec)==600 );

    // create pilot synchronizer
    q->pilotsync   = qpilotsync_create(600, 21);
    assert( qpilotsync_get_frame_len(q->pilotsync)==630);

    // reset global data counters
    dsssframe64sync_reset_framedatastats(q);

    // reset state and return
    dsssframe64sync_reset(q);
    return q;
}

// copy object
dsssframe64sync dsssframe64sync_copy(dsssframe64sync q_orig)
{
#if 0
    // validate input
    if (q_orig == NULL)
        return liquid_error_config("dsssframe64sync_copy(), object cannot be NULL");

    // allocate memory for new object
    dsssframe64sync q_copy = (dsssframe64sync) malloc(sizeof(struct dsssframe64sync_s));

    // copy entire memory space over and overwrite values as needed
    memmove(q_copy, q_orig, sizeof(struct dsssframe64sync_s));

    // set callback and context fields
    q_copy->callback = q_orig->callback;
    q_copy->context  = q_orig->context;

    // copy objects
    q_copy->detector = qdsync_cccf_copy(q_orig->detector);
    q_copy->dec      = qpacketmodem_copy  (q_orig->dec);
    q_copy->pilotsync= qpilotsync_copy    (q_orig->pilotsync);

    return q_copy;
#else
    return liquid_error_config("dsssframe64sync_copy(), method not yet implemented");
#endif
}

// destroy frame synchronizer object, freeing all internal memory
int dsssframe64sync_destroy(dsssframe64sync _q)
{
    // destroy synchronization objects
    msequence_destroy   (_q->ms);           //
    qdsync_cccf_destroy (_q->detector);     // frame detector
    qpacketmodem_destroy(_q->dec);          // payload demodulator
    qpilotsync_destroy  (_q->pilotsync);    // pilot synchronizer

    // free main object memory
    free(_q);
    return LIQUID_OK;
}

// print frame synchronizer object internals
int dsssframe64sync_print(dsssframe64sync _q)
{
    printf("<liquid.dsssframe64sync>\n");
    return LIQUID_OK;
}

// reset frame synchronizer object
int dsssframe64sync_reset(dsssframe64sync _q)
{
    // reset binary pre-demod synchronizer
    qdsync_cccf_reset(_q->detector);

    // reset state
    _q->state           = dsssframe64sync_STATE_DETECTFRAME;
    _q->preamble_counter= 0;
    _q->chip_counter    = 0;
    _q->payload_counter = 0;
    _q->sym_despread    = 0;

    // reset frame statistics
    _q->framesyncstats.evm = 0.0f;

    return LIQUID_OK;
}

// set the callback function
int dsssframe64sync_set_callback(dsssframe64sync    _q,
                                 framesync_callback _callback)
{
    _q->callback = _callback;
    return LIQUID_OK;
}

// set the user-defined data field (context)
int dsssframe64sync_set_context(dsssframe64sync _q,
                                void *          _context)
{
    _q->context= _context;
    return LIQUID_OK;
}

// execute frame synchronizer
//  _q     :   frame synchronizer object
//  _x      :   input sample array [size: _n x 1]
//  _n      :   number of input samples
int dsssframe64sync_execute(dsssframe64sync _q,
                            float complex * _buf,
                            unsigned int    _buf_len)
{
#if 0
    unsigned int i;
    for (i=0; i<_n; i++) {
        switch (_q->state) {
        case dsssframe64sync_STATE_DETECTFRAME:
            // detect frame (look for p/n sequence)
            dsssframe64sync_execute_seekpn(_q, _x[i]);
            break;
        case dsssframe64sync_STATE_RXPREAMBLE:
            // receive p/n sequence symbols
            dsssframe64sync_execute_rxpreamble(_q, _x[i]);
            break;
        case dsssframe64sync_STATE_RXPAYLOAD:
            // receive payload symbols
            dsssframe64sync_execute_rxpayload(_q, _x[i]);
            break;
        default:
            return liquid_error(LIQUID_EINT,"dsssframe64sync_exeucte(), unknown/unsupported state");
        }
    }
#else
    qdsync_cccf_execute(_q->detector, _buf, _buf_len);
#endif
    return LIQUID_OK;
}

// get detection threshold
float dsssframe64sync_get_threshold(dsssframe64sync _q)
{
    return qdsync_cccf_get_threshold(_q->detector);
}

// set detection threshold
int dsssframe64sync_set_threshold(dsssframe64sync _q,
                              float       _threshold)
{
    return qdsync_cccf_set_threshold(_q->detector, _threshold);
}

// reset frame data statistics
int dsssframe64sync_reset_framedatastats(dsssframe64sync _q)
{
    return framedatastats_reset(&_q->framedatastats);
}

// retrieve frame data statistics
framedatastats_s dsssframe64sync_get_framedatastats(dsssframe64sync _q)
{
    return _q->framedatastats;
}

// internal update
int dsssframe64sync_step(dsssframe64sync _q,
                         float complex * _buf,
                         unsigned int    _buf_len)
{
    // TODO: do this more efficiently?
    unsigned int i;
    for (i=0; i<_buf_len; i++) {
        // receive preamble
        if (_q->preamble_counter < 1024) {
            _q->preamble_rx[_q->preamble_counter++] = _buf[i];
            continue;
        }

        // generate pseudo-random symbol
        unsigned int  p = msequence_generate_symbol(_q->ms, 2);
        float complex s = cexpf(_Complex_I*2*M_PI*(float)p/(float)4);
        _q->sym_despread += _buf[i] * conjf(s);
        _q->chip_counter++;

        if (_q->chip_counter == 256) {
            _q->payload_rx[_q->payload_counter] = _q->sym_despread / 256.0f;
            _q->payload_counter++;
            _q->chip_counter = 0;
            if (_q->payload_counter == 630) {
                dsssframe64sync_decode(_q);
                return 1; // reset
            }
        }
    }

    return 0; // need more data
}

// internal decode received frame, update statistics, invoke callback
int dsssframe64sync_decode(dsssframe64sync _q)
{
    printf("dsssframe64sync: frame received!\n");

    // recover data symbols from pilots
    qpilotsync_execute(_q->pilotsync, _q->payload_rx, _q->payload_sym);
    
    // decode payload
    int crc_pass = qpacketmodem_decode(_q->dec, _q->payload_sym, _q->payload_dec);
    
    // update statistics
    _q->framedatastats.num_frames_detected++;
    _q->framedatastats.num_headers_valid  += crc_pass;
    _q->framedatastats.num_payloads_valid += crc_pass;
    _q->framedatastats.num_bytes_received += crc_pass ? 64 : 0;
    
    // invoke callback
    int rc = 0;
    if (_q->callback != NULL) {
        // set framesyncstats internals
        _q->framesyncstats.evm           = qpilotsync_get_evm(_q->pilotsync);
        _q->framesyncstats.rssi          = 0; //20*log10f(qdsync_cccf_get_gamma(_q->detector));
        _q->framesyncstats.cfo           = 0; //qdsync_cccf_get_frequency(_q->detector);
        _q->framesyncstats.framesyms     = _q->payload_sym;
        _q->framesyncstats.num_framesyms = 600;
        _q->framesyncstats.mod_scheme    = LIQUID_MODEM_QPSK;
        _q->framesyncstats.mod_bps       = 2;
        _q->framesyncstats.check         = LIQUID_CRC_24;
        _q->framesyncstats.fec0          = LIQUID_FEC_NONE;
        _q->framesyncstats.fec1          = LIQUID_FEC_GOLAY2412;
    
        // invoke callback method
        rc = _q->callback(&_q->payload_dec[0],   // header is first 8 bytes
                     crc_pass,
                     &_q->payload_dec[8],   // payload is last 64 bytes
                     64,
                     crc_pass,
                     _q->framesyncstats,
                     _q->context);
    }
    
    // reset frame synchronizer and return
    dsssframe64sync_reset(_q);
    return rc;
}

