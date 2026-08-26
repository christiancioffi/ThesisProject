#ifndef FILTER_MODULE_H
#define FILTER_MODULE_H

#include <stddef.h>
#include "biquad_coeffs.h" 

bool apply_sosfiltfilt(const float* input_data, float* output_data, int n_samples, const BiquadCoeffs* sos_coeffs, int num_sections);


#endif