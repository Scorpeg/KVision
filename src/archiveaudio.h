#ifndef ARCHIVEAUDIO_H
#define ARCHIVEAUDIO_H

// Sample rate for one decoded Hikvision audio frame.
// A header rate (nFrameRate, or nWidth when that field holds the rate) wins.
// A 640-byte packet is 8 kHz or 16 kHz, so the size is used only when the header
// does not say. readStamp is true when the stamp fallback ran, so the caller
// stores this frame's stamp for the next one.
struct ArchiveAudioDecision {
    int sampleRate = 0;
    bool readStamp = false;
};

inline int archiveAudioRateFromGap(double dynamicRate)
{
    if (dynamicRate > 7000 && dynamicRate < 9000)
        return 8000;
    if (dynamicRate > 10000 && dynamicRate < 12000)
        return 11025;
    if (dynamicRate > 14000 && dynamicRate < 18000)
        return 16000;
    if (dynamicRate > 20000 && dynamicRate < 24000)
        return 22050;
    if (dynamicRate > 28000 && dynamicRate < 36000)
        return 32000;
    if (dynamicRate > 40000 && dynamicRate < 46000)
        return 44100;
    if (dynamicRate > 46000 && dynamicRate < 50000)
        return 48000;
    return 0;
}

inline ArchiveAudioDecision archiveAudioDecision(int frameRate, int width, int sizeBytes, bool pcm16,
                                                 long stamp, long lastStamp)
{
    ArchiveAudioDecision decision;
    if (frameRate >= 4000 && frameRate <= 192000) {
        decision.sampleRate = frameRate;
        return decision;
    }
    if (width >= 4000 && width <= 192000) {
        decision.sampleRate = width;
        return decision;
    }
    if (pcm16) {
        if (sizeBytes == 640 || sizeBytes == 320)
            decision.sampleRate = 8000;
        else if (sizeBytes == 2048 || sizeBytes == 1024)
            decision.sampleRate = 16000;
    } else if (sizeBytes == 320 || sizeBytes == 160) {
        decision.sampleRate = 8000;
    } else if (sizeBytes == 1024 || sizeBytes == 512) {
        decision.sampleRate = 16000;
    }
    if (decision.sampleRate > 0)
        return decision;

    decision.readStamp = true;
    if (lastStamp > 0 && stamp > lastStamp) {
        const long deltaStamp = stamp - lastStamp;
        if (deltaStamp > 0 && deltaStamp < 200) {
            const double scale = pcm16 ? 500.0 : 1000.0;
            decision.sampleRate = archiveAudioRateFromGap(static_cast<double>(sizeBytes) * scale / deltaStamp);
        }
    }
    return decision;
}

#endif
