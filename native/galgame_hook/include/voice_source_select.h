#ifndef FUSHI_VOICE_SOURCE_SELECT_H_
#define FUSHI_VOICE_SOURCE_SELECT_H_

#include <cstddef>
#include <cstdint>
#include <map>

// PCM 捕获的**自动选语音源**判据（BUG-3268）。host 的 `GrabUtterance` 与只读诊断工具
// `ring_probe` 共用这一份实现：诊断工具与真实取音判据不一致，拿它排障只会把人带偏。
//
// 判据不变：每个源在「说话前」窗口 [ts-900, ts-250] 与「文本时刻」窗口 [ts-150, ts+450]
// 的平均能量之差最大者，就是「从静音跳到有声」的语音源。
//
// 改的是**一段 clip 算不算在某个窗口里**。旧实现只看 clip 的**提交时刻**落在哪个窗口，
// 而一段 clip 实际承载的是 [提交时刻, 提交时刻 + 时长) 这一整段音频。真机（シンフォニック=
// レイン，工画堂 Hy 引擎，DirectSound）上这让两边同时出错：
//   * 流式 BGM 每 1000ms 提交一段 1000ms。「说话前」窗口只有 650ms 宽，比 BGM 的提交间隔
//     还窄，于是 BGM 的提交时刻有约 1/3 的概率一段都不落在里面，基线被当成 0，「能量跳升」
//     就等于 BGM 的全部能量——BGM 被选成语音，每句只取到一两秒音乐。BGM 的相位每句都不同，
//     所以表现为「有的句子对、有的句子不对」。
//   * 这台引擎每句语音新建一个 buffer、一次灌满整句，提交时刻比文本 hook 早 ~190ms，落在
//     「文本时刻」窗口左边界（-150）之外，真语音连候选都进不去。
// 改成按**覆盖区间与窗口的重叠**归属，并且只取重叠那一部分音频算能量、按重叠时长加权：
// BGM 在两个窗口里都有真实能量，差值趋近 0；语音只在文本时刻窗口里有声，差值最大。
// 一整段跨过两个窗口的 clip 也不会两边算成同一个平均值而互相抵消。
//
// 时间轴假设与旧实现相同：clip 的音频从提交时刻开始播放。提前灌入的流式引擎会让这个
// 映射偏早，窗口本身的宽度吸收这点偏差（与旧实现同口径，不是本次引入的新假设）。
namespace fushi_voice_hook {

constexpr int64_t kVoiceSelectBeforeBeginMs = -900;
constexpr int64_t kVoiceSelectBeforeEndMs = -250;
constexpr int64_t kVoiceSelectAtBeginMs = -150;
constexpr int64_t kVoiceSelectAtEndMs = 450;

struct VoiceSelectClip {
  uint64_t source;       // VoiceClip::source_ptr
  int64_t timestamp_ms;  // 提交时刻（VoiceClip::timestamp_ms）
  int64_t duration_ms;   // 该段承载的音频时长；<= 0 视为 1ms 的瞬时段
};

// 把 clip 内 [from_ms, to_ms)（相对 clip 起点）换算成字节区间，按 [block_align] 对齐。
// 返回 false 表示区间为空或格式不可换算。
inline bool VoiceClipMsToByteRange(uint32_t byte_len, uint32_t block_align,
                                   uint32_t sample_rate, int64_t from_ms,
                                   int64_t to_ms, uint32_t* offset,
                                   uint32_t* length) {
  if (block_align == 0 || sample_rate == 0 || to_ms <= from_ms ||
      byte_len < block_align) {
    return false;
  }
  const uint64_t frames = byte_len / block_align;
  auto to_frame = [&](int64_t ms) -> uint64_t {
    if (ms <= 0) return 0;
    const uint64_t f = static_cast<uint64_t>(ms) * sample_rate / 1000u;
    return f < frames ? f : frames;
  };
  const uint64_t begin = to_frame(from_ms);
  const uint64_t end = to_frame(to_ms);
  if (end <= begin) {
    return false;
  }
  *offset = static_cast<uint32_t>(begin * block_align);
  *length = static_cast<uint32_t>((end - begin) * block_align);
  return true;
}

// [clips, clips+count)：参与自动选源的段（调用方已剔除用户排除的源）。
// [energy_over](index, from_ms, to_ms)：第 index 段在其自身 [from_ms, to_ms)（相对该段起点）
// 这部分音频的平均绝对幅值（16-bit 标度）；位深不认识返回 < 0。
// 返回选中的 source；选不出返回 0。[any_energy] 报告是否有任一重叠段算得出能量——全都算不
// 出时调用方走与格式无关的兜底判据。
template <typename EnergyOver>
uint64_t SelectVoiceSourceByOnset(const VoiceSelectClip* clips, size_t count,
                                  int64_t text_ts_ms, EnergyOver energy_over,
                                  bool* any_energy) {
  struct Acc {
    double weighted = 0.0;
    int64_t ms = 0;
  };
  std::map<uint64_t, Acc> before;
  std::map<uint64_t, Acc> at;
  bool energy_seen = false;
  auto accumulate = [&](size_t i, int64_t win_begin, int64_t win_end,
                        std::map<uint64_t, Acc>& into) {
    const VoiceSelectClip& c = clips[i];
    const int64_t dur = c.duration_ms > 0 ? c.duration_ms : 1;
    const int64_t clip_begin = c.timestamp_ms;
    const int64_t clip_end = c.timestamp_ms + dur;
    const int64_t lo = clip_begin > win_begin ? clip_begin : win_begin;
    const int64_t hi = clip_end < win_end ? clip_end : win_end;
    if (hi <= lo) {
      return;
    }
    const double e = energy_over(i, lo - clip_begin, hi - clip_begin);
    if (e < 0) {
      return;
    }
    energy_seen = true;
    Acc& acc = into[c.source];
    acc.weighted += e * static_cast<double>(hi - lo);
    acc.ms += hi - lo;
  };
  for (size_t i = 0; i < count; ++i) {
    accumulate(i, text_ts_ms + kVoiceSelectBeforeBeginMs,
               text_ts_ms + kVoiceSelectBeforeEndMs, before);
    accumulate(i, text_ts_ms + kVoiceSelectAtBeginMs,
               text_ts_ms + kVoiceSelectAtEndMs, at);
  }
  if (any_energy != nullptr) {
    *any_energy = energy_seen;
  }
  uint64_t selected = 0;
  double best_delta = -1e18;
  for (const auto& kv : at) {
    if (kv.second.ms <= 0 || kv.second.weighted <= 0) {
      continue;
    }
    const double at_avg = kv.second.weighted / static_cast<double>(kv.second.ms);
    const auto b = before.find(kv.first);
    const double before_avg =
        (b != before.end() && b->second.ms > 0)
            ? b->second.weighted / static_cast<double>(b->second.ms)
            : 0.0;
    const double delta = at_avg - before_avg;
    if (delta > best_delta) {
      best_delta = delta;
      selected = kv.first;
    }
  }
  return selected;
}

}  // namespace fushi_voice_hook

#endif  // FUSHI_VOICE_SOURCE_SELECT_H_
