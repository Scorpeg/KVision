#ifndef PLAYCTRLAUDIO_H
#define PLAYCTRLAUDIO_H

// CMPManager::SetSkipType in this vendored libPlayCtrl.so loads the splitter
// with mov 0x20(%rax), %rax. That is how playback turns audio decoding on
// without PlayM4_PlaySound. A library that moves the field will not contain
// this instruction. Callers must stop there instead of reading a guessed offset.
constexpr int kPlayCtrlAudioSplitterOffset = 0x20;
constexpr int kPlayCtrlSetSkipScanBytes = 0x210;

inline bool playCtrlAudioSplitterLoadMatches(const unsigned char *code, int length)
{
    if (!code || length < 4)
        return false;
    const unsigned char pattern[4] = {0x48, 0x8b, 0x40, 0x20};
    const int last = length - 4;
    for (int i = 0; i <= last; ++i) {
        if (code[i] == pattern[0] && code[i + 1] == pattern[1]
                && code[i + 2] == pattern[2] && code[i + 3] == pattern[3])
            return true;
    }
    return false;
}

#endif
