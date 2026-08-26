#ifndef MEL_SPECTROGRAM_H
#define MEL_SPECTROGRAM_H

#include <cstdint>

struct MelSpectrogram {
    float* data;     // buffer lineare: n_mels * n_frames
    int n_mels;
    int n_frames;

    MelSpectrogram() : data(nullptr), n_mels(0), n_frames(0) {}
};

MelSpectrogram calculate_mel_spectrogram(const float* audio_input,
                                         int total_samples,
                                         int frame_size,
                                         int hop_size,
                                         int n_mels);

void free_mel_spectrogram(MelSpectrogram& spec);

#endif
