// BUG-3268：PCM 自动选语音源的不变量测试（voice_source_select.h）。
//
// 时间轴来自真机（シンフォニック=レイン，工画堂 Hy 引擎，x86，DirectSound，2026-10-10，
// 只读探针 fushi_voice_ring_probe --list-clips 从共享内存 dump 出的 VoiceClip 索引）：
//   * BGM 源：流式 buffer，每 1000ms 提交一段 1000ms（44100Hz/2ch/16bit，176400B）。
//   * 语音源：每句新建一个 buffer、一次灌满整句（44100Hz/1ch/16bit），提交时刻比文本
//     hook 早 15~190ms。
//   * 实测整段平均能量：语音 598，BGM 3221~3567——语音的**平均**能量远低于 BGM。
// 旧判据只看提交时刻：BGM 在 650ms 宽的「说话前」窗口里常常一段都没有（提交间隔 1000ms），
// 基线算 0，「跳升」等于 BGM 全部能量；语音提交在 -187ms 时又落在「文本时刻」窗口
// （-150 起）之外——两句实测台词都被选成了 BGM，用户听到 1 秒音乐。
//
// 能量函数按时间段给出（与 host 一样只对落进窗口的那一截求能量），用分段常数模拟
// 「语音句首有一小段静音，之后有声」与「BGM 能量平稳」。
//
// 不用 assert：Release 配置下 NDEBUG 会把它整条编掉（同 utterance_window_test 的说明）。
#undef NDEBUG

#include <cstdint>
#include <cstdio>
#include <vector>

#include "voice_source_select.h"

using fushi_voice_hook::SelectVoiceSourceByOnset;
using fushi_voice_hook::VoiceClipMsToByteRange;
using fushi_voice_hook::VoiceSelectClip;

namespace {

int g_failures = 0;

void ExpectSource(uint64_t got, uint64_t want, const char* what) {
  if (got == want) {
    return;
  }
  ++g_failures;
  printf("FAIL %s: got %llx want %llx\n", what,
         static_cast<unsigned long long>(got),
         static_cast<unsigned long long>(want));
}

void ExpectTrue(bool ok, const char* what) {
  if (ok) {
    return;
  }
  ++g_failures;
  printf("FAIL %s\n", what);
}

constexpr uint64_t kBgm = 0x163db770;
constexpr uint64_t kVoice = 0x163db540;

// 每段的能量模型：句首 [0, silent_ms) 为 silent_energy，其后为 body_energy。
struct ClipModel {
  VoiceSelectClip clip;
  int64_t silent_ms;
  double silent_energy;
  double body_energy;
};

double ModelEnergy(const ClipModel& m, int64_t from_ms, int64_t to_ms) {
  if (to_ms <= from_ms) {
    return m.body_energy;
  }
  const int64_t quiet_hi = to_ms < m.silent_ms ? to_ms : m.silent_ms;
  const int64_t quiet = quiet_hi > from_ms ? quiet_hi - from_ms : 0;
  const int64_t loud = (to_ms - from_ms) - quiet;
  return (m.silent_energy * static_cast<double>(quiet) +
          m.body_energy * static_cast<double>(loud)) /
         static_cast<double>(to_ms - from_ms);
}

// 流式 BGM：[first_ms, last_ms] 每 1000ms 一段 1000ms，能量平稳。
void AddBgm(std::vector<ClipModel>& out, int64_t text_ts, int64_t first_ms,
            int64_t last_ms, double energy) {
  for (int64_t d = first_ms; d <= last_ms; d += 1000) {
    out.push_back({{kBgm, text_ts + d, 1000}, 0, energy, energy});
  }
}

uint64_t Select(const std::vector<ClipModel>& models, int64_t text_ts,
                bool* any_energy = nullptr) {
  std::vector<VoiceSelectClip> clips;
  for (const ClipModel& m : models) {
    clips.push_back(m.clip);
  }
  bool any = false;
  const uint64_t selected = SelectVoiceSourceByOnset(
      clips.data(), clips.size(), text_ts,
      [&](size_t i, int64_t from_ms, int64_t to_ms) {
        return ModelEnergy(models[i], from_ms, to_ms);
      },
      &any);
  if (any_energy != nullptr) {
    *any_energy = any;
  }
  return selected;
}

}  // namespace

int main() {
  // ① 真机「その方が楽だもん」(text_ts=191681765)：BGM 在 -2969/-1969/-969/+31/...，
  //    语音在 -187 一次灌入 2778ms。旧判据选中 BGM。
  {
    const int64_t ts = 191681765;
    std::vector<ClipModel> m;
    AddBgm(m, ts, -2969, 5031, 3567.0);
    m.push_back({{kVoice, ts - 187, 2778}, 120, 30.0, 1400.0});
    ExpectSource(Select(m, ts), kVoice, "real short line picks the voice");
  }

  // ② 真机长句「声楽科はソロ…」(text_ts=190653890)：BGM 在 -2094/-1094/-94/+906/...，
  //    语音在 -15 一次灌入 12409ms。
  {
    const int64_t ts = 190653890;
    std::vector<ClipModel> m;
    AddBgm(m, ts, -2094, 11906, 3221.0);
    m.push_back({{kVoice, ts - 15, 12409}, 150, 30.0, 1100.0});
    ExpectSource(Select(m, ts), kVoice, "real long line picks the voice");
  }

  // ③ 「有的句子对、有的不对」的根：BGM 相位每句都不同。整个 1000ms 相位扫一遍，
  //    结论必须与相位无关。
  for (int64_t phase = 0; phase < 1000; phase += 25) {
    const int64_t ts = 500000000;
    std::vector<ClipModel> m;
    AddBgm(m, ts, -3000 + phase, 6000, 3500.0);
    m.push_back({{kVoice, ts - 187, 2778}, 120, 30.0, 1400.0});
    if (Select(m, ts) != kVoice) {
      ++g_failures;
      printf("FAIL bgm phase %lld picked the bgm\n",
             static_cast<long long>(phase));
    }
  }

  // ④ 语音提早到跨过两个窗口（-400 提交，句首 300ms 静音）：只有覆盖到的那一截算能量，
  //    整段平均值不会让两个窗口互相抵消。
  {
    const int64_t ts = 600000000;
    std::vector<ClipModel> m;
    AddBgm(m, ts, -2500, 5500, 3500.0);
    m.push_back({{kVoice, ts - 400, 3000}, 300, 20.0, 1500.0});
    ExpectSource(Select(m, ts), kVoice, "voice spanning both windows");
  }

  // ⑤ 「从静音跳到有声」本来就成立的旧场景不变：持续流静音的语音源（KiriKiri 式小段
  //    流式）在文本时刻起声，BGM 平稳。
  {
    const int64_t ts = 700000000;
    std::vector<ClipModel> m;
    AddBgm(m, ts, -3000, 5000, 3000.0);
    for (int64_t d = -1500; d <= 2000; d += 125) {
      const double e = d < -100 ? 15.0 : 1300.0;
      m.push_back({{kVoice, ts + d, 125}, 0, e, e});
    }
    ExpectSource(Select(m, ts), kVoice, "streamed voice onset");
  }

  // ⑥ 能量全都算不出（位深不认识）：any_energy 必须如实报 false，交调用方走字节兜底。
  {
    const int64_t ts = 800000000;
    std::vector<VoiceSelectClip> clips{{kBgm, ts - 94, 1000},
                                       {kVoice, ts - 15, 2000}};
    bool any = true;
    const uint64_t selected = SelectVoiceSourceByOnset(
        clips.data(), clips.size(), ts,
        [](size_t, int64_t, int64_t) { return -1.0; }, &any);
    ExpectTrue(!any, "unknown format reports no energy");
    ExpectSource(selected, 0, "unknown format selects nothing");
  }

  // ⑦ 窗口外的段不参与：只有远离文本时刻的段时选不出源。
  {
    const int64_t ts = 900000000;
    std::vector<ClipModel> m{{{kVoice, ts - 5000, 1000}, 0, 900.0, 900.0},
                             {{kBgm, ts + 2000, 1000}, 0, 900.0, 900.0}};
    ExpectSource(Select(m, ts), 0, "clips outside both windows");
  }

  // ⑧ 毫秒 → 字节区间：按帧对齐、夹在 clip 之内。
  {
    uint32_t off = 0;
    uint32_t len = 0;
    // 44100Hz/1ch/16bit，2778ms ≈ 245040B（真机那句语音）。
    ExpectTrue(VoiceClipMsToByteRange(245040, 2, 44100, 37, 637, &off, &len),
               "range inside clip");
    ExpectTrue(off == 1631 * 2 && len == (28091 - 1631) * 2,
               "range offset/length in frames");
    ExpectTrue(VoiceClipMsToByteRange(245040, 2, 44100, 2700, 9999, &off, &len),
               "range clamped to clip end");
    ExpectTrue(off + len == 245040, "clamped range ends at clip end");
    ExpectTrue(!VoiceClipMsToByteRange(245040, 2, 44100, 300, 300, &off, &len),
               "empty range");
    ExpectTrue(!VoiceClipMsToByteRange(245040, 0, 44100, 0, 100, &off, &len),
               "unknown block align");
  }

  if (g_failures != 0) {
    printf("voice_source_select_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("voice_source_select_test: ok\n");
  return 0;
}
