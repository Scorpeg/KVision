#include "vcaparse.h"

#include <algorithm>
#include <cstring>

QVector<VcaSpan> ruleBodySpans(const unsigned char *p, int len, int gap)
{
    QVector<VcaSpan> spans;
    if (!p || len < 24 || gap < 1)
        return spans;
    const int ruleCount = p[22];
    int pos = 24;
    for (int k = 0; k < ruleCount && pos < len; ++k) {
        int end = pos;
        while (end + gap <= len) {
            bool zeros = true;
            for (int z = 0; z < gap; ++z) {
                if (p[end + z] != 0) {
                    zeros = false;
                    break;
                }
            }
            if (zeros)
                break;
            ++end;
        }
        if (end + gap > len)
            break;
        VcaSpan span;
        span.begin = pos;
        span.end = end;
        spans.append(span);
        pos = end + gap;
    }
    return spans;
}

QVector<int> g3RuleStarts(const unsigned char *p, int len)
{
    QVector<int> starts;
    if (!p)
        return starts;
    for (int i = 24; i + 1 < len; ++i) {
        const bool line = p[i] == 0x00 && p[i + 1] == 0x10;
        const bool zone = p[i] == 0x40 && (p[i + 1] == 0x20 || p[i + 1] == 0x30 || p[i + 1] == 0x38);
        if (line || zone)
            starts.append(i);
    }
    return starts;
}

bool decodeG3Targets(const unsigned char *p, int len, QVector<VcaTargetBox> *out)
{
    if (!out)
        return false;
    out->clear();
    if (!p || len < 36 + 28 || p[0] != 0x00 || p[1] != 0x14)
        return false;
    const int count = std::min<int>(p[26], 30);
    int pos = 36;
    for (int k = 0; k < count && pos + 28 <= len; ++k, pos += 32) {
        quint32 tid = 0;
        float x = 0.f;
        float y = 0.f;
        float w = 0.f;
        float h = 0.f;
        std::memcpy(&tid, p + pos, 4);
        std::memcpy(&x, p + pos + 8, 4);
        std::memcpy(&y, p + pos + 12, 4);
        std::memcpy(&w, p + pos + 16, 4);
        std::memcpy(&h, p + pos + 20, 4);
        if (tid == 0 || !(w > 0.002f && h > 0.002f && x >= 0.f && y >= 0.f && x + w <= 1.01f && y + h <= 1.01f))
            continue;
        VcaTargetBox box;
        box.id = tid;
        box.x = x;
        box.y = y;
        box.w = w;
        box.h = h;
        out->append(box);
    }
    return true;
}

VcaAlarmIds readVcaAlarm(const unsigned char *p, int len)
{
    VcaAlarmIds alarm;
    if (!p || len < 40 || p[0] != 0x00 || p[1] != 0x03 || p[5] != 2)
        return alarm;
    if (p[14] == 0x30) {
        const unsigned char *t = p + len - 16;
        if (!std::all_of(t - 6, t, [](unsigned char b) { return b == 0; }))
            return alarm;
        alarm.layout = VcaAlarmIds::G3;
        alarm.target = (((t[0] & 0x0f) << 11) | (static_cast<quint32>(t[1]) << 3) | (t[2] >> 5)) & 0x7fff;
        return alarm;
    }
    const unsigned char *tail = p + len;
    const bool zeros = std::all_of(tail - 20, tail - 13, [](unsigned char b) { return b == 0; });
    if (!zeros || !(tail[-13] & 0x80))
        return alarm;
    alarm.layout = VcaAlarmIds::Legacy;
    alarm.rule = tail[-21];
    alarm.target = ((static_cast<quint32>(tail[-13]) << 8) | tail[-12]) & 0x7fff;
    return alarm;
}

bool mergeKeptRules(QVector<VcaRule> *rules, const QVector<VcaRule> &got)
{
    if (!rules)
        return false;
    bool changed = false;
    for (const VcaRule &n : got) {
        bool found = false;
        for (VcaRule &old : *rules) {
            if (old.id != n.id)
                continue;
            if (old.polygon != n.polygon || old.points != n.points) {
                old = n;
                changed = true;
            }
            found = true;
            break;
        }
        if (!found) {
            rules->append(n);
            changed = true;
        }
    }
    return changed;
}
