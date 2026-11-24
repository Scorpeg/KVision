#include "archiveaudio.h"
#include "playctrlaudio.h"
#include "stallwatch.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

TEST(StallWatch, OneTileDoesNotChangeAnother)
{
    const qint64 now = 200000;
    const qint64 assigned = now - 20000;
    EXPECT_EQ(evaluateStall(true, 6, now - 8000, assigned, now, 0, 0), 5000);
    EXPECT_EQ(evaluateStall(true, 6, now - 1000, assigned, now, 0, 0), 0);
    EXPECT_EQ(evaluateStall(true, kStallStatusLoading, now - 60000, assigned, now, 0, 0), 0);
}

TEST(ArchiveAudio, HeaderRateBeatsAnAmbiguousPacket)
{
    const ArchiveAudioDecision rate = archiveAudioDecision(16000, 0, 640, true, 1000, 0);
    EXPECT_EQ(rate.sampleRate, 16000);
    EXPECT_FALSE(rate.readStamp);
}

TEST(ArchiveAudio, WidthCarriesTheRateWhenTheFrameRateIsNotOne)
{
    const ArchiveAudioDecision rate = archiveAudioDecision(25, 8000, 640, true, 1000, 0);
    EXPECT_EQ(rate.sampleRate, 8000);
    EXPECT_FALSE(rate.readStamp);
}

TEST(ArchiveAudio, PacketSizeIsTheFallback)
{
    const ArchiveAudioDecision pcm16 = archiveAudioDecision(0, 0, 640, true, 1000, 0);
    EXPECT_EQ(pcm16.sampleRate, 8000);
    EXPECT_FALSE(pcm16.readStamp);

    const ArchiveAudioDecision pcm8 = archiveAudioDecision(0, 0, 160, false, 1000, 0);
    EXPECT_EQ(pcm8.sampleRate, 8000);
    EXPECT_FALSE(pcm8.readStamp);
}

TEST(ArchiveAudio, StampGapFillsAnUnknownPacket)
{
    // 800 bytes over 50 ms is 8 kHz of 16-bit audio, and it is not a known packet size.
    const ArchiveAudioDecision pcm16 = archiveAudioDecision(0, 0, 800, true, 1050, 1000);
    EXPECT_EQ(pcm16.sampleRate, 8000);
    EXPECT_TRUE(pcm16.readStamp);

    const ArchiveAudioDecision pcm8 = archiveAudioDecision(0, 0, 400, false, 1050, 1000);
    EXPECT_EQ(pcm8.sampleRate, 8000);
    EXPECT_TRUE(pcm8.readStamp);

    const ArchiveAudioDecision unknown = archiveAudioDecision(0, 0, 100, true, 1000, 0);
    EXPECT_EQ(unknown.sampleRate, 0);
    EXPECT_TRUE(unknown.readStamp);
}

TEST(PlayCtrlAudio, RejectsALibraryThatMovedTheSplitter)
{
    unsigned char code[8] = {};
    EXPECT_FALSE(playCtrlAudioSplitterLoadMatches(nullptr, 4));
    EXPECT_FALSE(playCtrlAudioSplitterLoadMatches(code, 8));
    code[2] = 0x48;
    code[3] = 0x8b;
    code[4] = 0x40;
    code[5] = 0x20;
    EXPECT_TRUE(playCtrlAudioSplitterLoadMatches(code, 8));
}

// The function bytes in the vendored file. Section offsets are file positions.
// Only the symbol value is a load address, and a PT_LOAD header turns it back
// into a file position. The test never loads the library.
bool vendoredSetSkipMatches(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    const std::vector<unsigned char> file((std::istreambuf_iterator<char>(in)),
                                           std::istreambuf_iterator<char>());
    if (file.size() < 64 || file[0] != 0x7f || file[1] != 'E' || file[2] != 'L' || file[3] != 'F'
            || file[4] != 2 || file[5] != 1)
        return false;

    auto need = [&](size_t at, size_t n) {
        return n <= file.size() && at <= file.size() - n;
    };
    auto u16 = [&](size_t at) -> uint16_t {
        if (!need(at, 2))
            return 0;
        return static_cast<uint16_t>(file[at] | (file[at + 1] << 8));
    };
    auto u32 = [&](size_t at) -> uint32_t {
        if (!need(at, 4))
            return 0;
        return static_cast<uint32_t>(file[at] | (file[at + 1] << 8)
                                      | (file[at + 2] << 16) | (file[at + 3] << 24));
    };
    auto u64 = [&](size_t at) -> uint64_t {
        if (!need(at, 8))
            return 0;
        uint64_t value = 0;
        for (int i = 0; i < 8; ++i)
            value |= static_cast<uint64_t>(file[at + i]) << (8 * i);
        return value;
    };
    auto cstr = [&](size_t at) -> std::string {
        if (at >= file.size())
            return {};
        size_t end = at;
        while (end < file.size() && file[end] != 0)
            ++end;
        return std::string(reinterpret_cast<const char *>(file.data() + at), end - at);
    };

    const uint64_t phoff = u64(32);
    const uint16_t phentsize = u16(54);
    const uint16_t phnum = u16(56);
    if (phentsize < 56 || phnum == 0 || !need(static_cast<size_t>(phoff), static_cast<size_t>(phnum) * phentsize))
        return false;
    auto fileOff = [&](uint64_t vaddr) -> int64_t {
        for (uint16_t i = 0; i < phnum; ++i) {
            const size_t ph = static_cast<size_t>(phoff) + static_cast<size_t>(i) * phentsize;
            if (u32(ph) != 1)
                continue;
            const uint64_t offset = u64(ph + 8);
            const uint64_t start = u64(ph + 16);
            const uint64_t filesz = u64(ph + 32);
            if (vaddr >= start && vaddr - start < filesz)
                return static_cast<int64_t>(offset + (vaddr - start));
        }
        return -1;
    };

    const uint64_t shoff = u64(40);
    const uint16_t shentsize = u16(58);
    const uint16_t shnum = u16(60);
    const uint16_t shstrndx = u16(62);
    if (shentsize < 64 || shstrndx >= shnum
            || !need(static_cast<size_t>(shoff), static_cast<size_t>(shnum) * shentsize))
        return false;
    const size_t shstr = static_cast<size_t>(shoff) + static_cast<size_t>(shstrndx) * shentsize;
    const uint64_t shstrFile = u64(shstr + 24);
    if (!need(static_cast<size_t>(shstrFile), 1))
        return false;

    int64_t dynsymOff = -1;
    int64_t dynstrOff = -1;
    uint64_t dynsymSize = 0;
    uint64_t dynsymEnt = 24;
    for (uint16_t i = 0; i < shnum; ++i) {
        const size_t sh = static_cast<size_t>(shoff) + static_cast<size_t>(i) * shentsize;
        const std::string section = cstr(static_cast<size_t>(shstrFile) + u32(sh));
        const uint32_t type = u32(sh + 4);
        if (section == ".dynsym" && type == 11) {
            dynsymOff = static_cast<int64_t>(u64(sh + 24));
            dynsymSize = u64(sh + 32);
            const uint64_t entsize = u64(sh + 56);
            if (entsize >= 24)
                dynsymEnt = entsize;
        } else if (section == ".dynstr" && type == 3) {
            dynstrOff = static_cast<int64_t>(u64(sh + 24));
        }
    }
    if (dynsymOff < 0 || dynstrOff < 0 || dynsymSize < dynsymEnt)
        return false;
    if (!need(static_cast<size_t>(dynsymOff), static_cast<size_t>(dynsymSize))
            || !need(static_cast<size_t>(dynstrOff), 1))
        return false;

    const std::string wanted = "_ZN10CMPManager11SetSkipTypeE14_MP_SKIP_TYPE_ii";
    for (uint64_t at = 0; at + dynsymEnt <= dynsymSize; at += dynsymEnt) {
        const size_t sym = static_cast<size_t>(dynsymOff) + static_cast<size_t>(at);
        if (cstr(static_cast<size_t>(dynstrOff) + u32(sym)) != wanted)
            continue;
        const int64_t codeAt = fileOff(u64(sym + 8));
        if (codeAt < 0 || !need(static_cast<size_t>(codeAt), static_cast<size_t>(kPlayCtrlSetSkipScanBytes)))
            return false;
        return playCtrlAudioSplitterLoadMatches(file.data() + codeAt, kPlayCtrlSetSkipScanBytes);
    }
    return false;
}

TEST(PlayCtrlAudio, VendoredLibraryStillMatchesTheHook)
{
    EXPECT_TRUE(vendoredSetSkipMatches(KVISION_PLAYCTRL_SO));
}
