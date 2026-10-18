// 最小 MPEG-TS 复用器实现 —— 见 ts_muxer.h 头注释（契约与线程前提）。
//
// 结构：emit_psi（PAT/PMT 单包，MPEG-2 CRC）→ emit_pes（PES 组包 → 整流逐包切分）
// → write_packet（唯一的 TS 打包函数：AF/stuffing/PCR/continuity 全在这里收口）。
// PCR pacing 与静音填充走 tick(now_90k)（细则①/②）。

#include "cast/airplay/ts_muxer.h"

#include "cast/airplay/audio_wrap.h"

#include <algorithm>
#include <cstring>

namespace fr::cast {

namespace {

constexpr uint16_t kPatPid = 0x0000;
constexpr uint16_t kPmtPid = 0x1000;
constexpr uint16_t kVideoPid = 0x0100;  // H.264（stream_type 0x1b；PCR 载体）
constexpr uint16_t kAudioPid = 0x0101;  // AAC-ELD 裸帧（stream_type 0x0f）
constexpr size_t kTsLen = 188;
constexpr size_t kPayload = kTsLen - 4;  // 184

// 音频帧间隔：spf=480 @44.1kHz ≈ 10.8847ms ≈ 979.6 tick（90kHz）→ 取 980。
constexpr uint64_t kAudioFrameTicks = 980;
// 细则②（d278）：轴落后实时时间 ≥ 1 帧就补静音帧。★不再有"缺口过大就不补"的上限——
// 缺口的正确表达是**静音**，而 mpv 对音频 PTS 跳变 ≥5s 会复位整个播放（见头文件）。
// 单次 tick 最多补 8 帧：轴由 tick 反复推进即可追上（缺口是几十秒量级，但 tick 每
// 10ms 或每帧都会跑；实测 8 帧/次 × ~100 次/s = 8.7s 音频/s，够追上）。
constexpr uint64_t kSilenceMinGap = kAudioFrameTicks;
constexpr int kSilenceMaxPerTick = 8;
// 真实帧 PTS 与轴的允许偏差：超出即认为远端钟不可信（客户端重开音频流会让
// 远端钟往回跳或跳出 17812s 这类伪值），一律改用轴 —— 轴永远单调且铺满。
constexpr uint64_t kAudioMaxLead = 8100;  // 90ms @ 90kHz
// ★d289：音频轴**回拉**阈值。d288 判决——真机流里 a−v 于视频帧 #600 单跳
//   +2.778s 后恒定 150s，根因之一是音频轴**只进退**：fill_silence() 只在
//   落后时补、feed_audio() 在 PTS 落后时沿用已超前的轴，代码里没有任何把轴
//   拉回基准的路径 ⇒ 一次扰动即永久冻结。轴超前基准超过本阈值时重锚。
//   ★为什么阈值取 100ms：既要能盖住网络抖动（局域网 <20ms），又不能太大 ——
//   mpv 对音频 PTS 单次跳变 ≥5s 直接 reset_playback_state()（d278），本阈值
//   远在安全区内（5s 的 2%）。
constexpr uint64_t kAxisCorrectionTh = 9000;  // 100ms @ 90kHz
// ★d289：视频帧落后音频轴超过本阈值即丢弃（契约 §5.3「视频跟随、赶不上就丢」）。
//   取 2 帧 ≈ 66ms：1 帧以内属正常抖动，不该丢；丢太多会让画面明显卡顿。
constexpr uint64_t kVideoDropTh = 1800;  // 20ms @ 90kHz（≈1.8 帧）
// 细则①：PCR 间隔 ≤100ms（DVB 推荐值；ISO 13818-1 上限 700ms）。
constexpr uint64_t kPcrInterval = 9000;     // 100ms @ 90kHz
// ★d282：PAT/PMT 重发周期 = 100ms @ 90kHz（对齐 FFmpeg mpegtsenc 的 pat_period
//   = 0.1s 默认值）。此前全流只在 start() 发一次：一旦中途丢分片（写超时丢片、
//   对端重连、mpv 内部复位），mpv 就永久失去 PMT 只能等 EOF 重来。
constexpr uint64_t kPsiPeriod = 9000;       // 100ms @ 90kHz

// d278：缺口填充用的**真静音** AAC-ELD 帧（18B，spf=480/44.1k/2ch，与本组件
// PMT 声明的 ASC `f8 e8 50 00` 匹配）。
// 来源 = 客户端在真机投屏时自己下发的静音帧（audio_eld.bin 里的 18B 帧），
// 判定方法**不是猜**：build/test_ts/eld_silence.cpp 用生产复用器把它复包成
// 纯音频 TS，交 libmpv 解出 PCM 实测 `max|s16| = 0 / RMS = 0.0`（数字静音；
// 对照组 135B 真实音频帧 RMS=4674）。重复发送该帧 = 静音，且不引入任何
// 时间轴跳变。16~19B 的一类帧都实测为数字静音，本常量取其中一枚。
constexpr uint8_t kEldSilence[18] = {0x8e, 0x8b, 0x0f, 0x92, 0xdc, 0x3c, 0xca, 0x1e,
                                     0x21, 0xf1, 0xc1, 0xf2, 0x5b, 0x87, 0x99, 0x43,
                                     0xc4, 0x3c};

// d275：构建 IOD 描述符（ISO 14496-1 in MPEG-2 TS）。层级 body 均 <128B，
// expandable length 取单字节（ffmpeg ff_mp4_read_descr_len 兼容）。
//   [0x1d][len][scope][label] + MP4 IOD(0x02){ ID,5×profile,
//     ES_Descr(0x03){ ES_ID,flags0, DecoderConfig(0x04){ 0x40,streamType,
//     bufDB3,max4,avg4, DecSpecificInfo(0x05)=ASC }, SLConfig(0x06)=2 } }
// FFmpeg pmt_cb 在 descriptor body 先跳 scope/label 两字节再进 mp4_read_iods
//（mpegts.c "get8 // scope; get8 // label"），两字节值不读——占位 0x01。
size_t build_iod(uint8_t* out, uint16_t es_id, const uint8_t* asc, size_t asc_len) {
  size_t n = 0;
  // DecSpecificInfo(0x05) + DecoderConfigDescriptor(0x04)
  uint8_t dc[32];
  size_t dn = 0;
  dc[dn++] = 0x04;
  dc[dn++] = (uint8_t)(13 + 2 + asc_len);
  dc[dn++] = 0x40;  // objectTypeIndication = MPEG-4 Audio → AV_CODEC_ID_AAC
  dc[dn++] = 0x15;  // streamType audio(5)<<3 | reserved 1
  dc[dn++] = 0x00; dc[dn++] = 0x00; dc[dn++] = 0x00;                   // bufferSizeDB
  dc[dn++] = 0x00; dc[dn++] = 0x01; dc[dn++] = 0xE2; dc[dn++] = 0x40;  // maxBitrate
  dc[dn++] = 0x00; dc[dn++] = 0x01; dc[dn++] = 0xE2; dc[dn++] = 0x40;  // avgBitrate
  dc[dn++] = 0x05;  // MP4DecSpecificDescrTag
  dc[dn++] = (uint8_t)asc_len;
  std::memcpy(dc + dn, asc, asc_len);
  dn += asc_len;
  // ES_Descriptor(0x03) = ES_ID(2) + flags(1) + DecConfig + SLConfig(0x06)
  uint8_t es[64];
  size_t en = 0;
  es[en++] = 0x03;
  es[en++] = (uint8_t)(3 + dn + 3);
  es[en++] = (uint8_t)(es_id >> 8);
  es[en++] = (uint8_t)(es_id & 0xFF);
  es[en++] = 0x00;  // flags：无 streamDependence/URL/OCR
  std::memcpy(es + en, dc, dn);
  en += dn;
  es[en++] = 0x06;  // MP4SLDescrTag
  es[en++] = 1;
  es[en++] = 0x02;  // predefined 2
  // IOD(0x02)：先拼 MP4 IOD 本体，再由实际字节数反推外层 0x1d 的 length
  // ★长度语义（FFmpeg pmt_cb 真值：先 tag/len，再跳 scope+label，len-=2 后
  //   整段交给 mp4_read_iods）→ 外层 length 必须覆盖
  //   scope(1)+label(1)+IOD总字节(tag+len+content)。d276 修：旧式 2+2+en
  //   漏算 IOD 自身 tag/len(2) 与 5 个 profile 字节，短 7 字节。
  uint8_t mp4[64];
  size_t mn = 0;
  mp4[mn++] = 0x02;                  // MP4IODescrTag
  mp4[mn++] = (uint8_t)(2 + 5 + en);  // ID(2) + 5×profile + ES_Descr
  mp4[mn++] = 0x00; mp4[mn++] = 0x01;  // objectDescriptorID = 1
  for (int i = 0; i < 5; i++) mp4[mn++] = 0xFF;  // OD/scene/audio/visual/graphics profile
  std::memcpy(mp4 + mn, es, en);
  mn += en;
  out[n++] = 0x1d;
  out[n++] = (uint8_t)(2 + mn);      // scope/label(2) + IOD 总字节
  out[n++] = 0x01;                   // scope（占位，FFmpeg 跳读）
  out[n++] = 0x01;                   // label（占位，FFmpeg 跳读）
  std::memcpy(out + n, mp4, mn);
  n += mn;
  return n;
}

// MPEG-2 PSI CRC-32（poly 0x04C11DB7，MSB-first，初/终 0xFFFFFFFF）
uint32_t mpeg_crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= (uint32_t)data[i] << 24;
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : (crc << 1);
  }
  return crc;
}

void put16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

// 33-bit PTS 编码：'0010' 前缀 + 三段（3/15/15）夹 marker 位（ISO 13818-1 §2.4.3.2）
void put_pts(uint8_t* p, uint64_t pts) {
  pts &= 0x1FFFFFFFFull;
  p[0] = (uint8_t)(0x10u | ((pts >> 29) & 0x0Eu) | 0x01u);
  const uint16_t mid = (uint16_t)((((pts >> 15) & 0x7FFFu) << 1) | 0x01u);
  p[1] = (uint8_t)(mid >> 8);
  p[2] = (uint8_t)mid;
  const uint16_t lo = (uint16_t)(((pts & 0x7FFFu) << 1) | 0x01u);
  p[3] = (uint8_t)(lo >> 8);
  p[4] = (uint8_t)lo;
}

}  // namespace

bool TsMuxer::start(Sink sink, bool has_video, bool has_audio, bool hevc_video) {
  if (!sink) return false;
  sink_ = std::move(sink);
  has_video_ = has_video;
  has_audio_ = has_audio;
  hevc_ = has_video && hevc_video;  // ★d292
  broken_ = false;
  // d278：CC 初值 15 ⇒ 每路第一包写出 0（对齐 mpegtsenc 的 `cc = 15` 初值）
  cc_[0] = cc_[1] = cc_[2] = cc_[3] = 15;
  pcr_seeded_ = false;
  last_pcr_at_ = 0;
  last_psi_at_ = 0;     // d282：PSI 周期锚
  psi_seeded_ = false;  //   未锚定 ⇒ 首个 tick 必发一次（start 的 PSI 可能早于对端就绪）
  // d278：声明了音频就立刻启用音频轴（从 0 起，tick 用静音帧铺到实时时间）——
  // 客户端可能比视频晚几十秒才开音频流，头段必须是**静音**而不是"没有字节"。
  have_audio_ = has_audio_;
  audio_axis_ = 0;
  last_audio_pts_ = 0;    // d289：单调基准随**新会话**归零（不在 stop，见 stop 注释）
  real_90k_ = 0;          // d289：实时基线随新会话归零
  real_seeded_ = false;
  dropped_video_ = 0;      // d289：诊断计数在 start 归零 —— stop 后仍需可读
  axis_corrections_ = 0;
  last_video_pts_ = 0;     // d311：视频单调基准随新会话归零（同 last_audio_pts_ 口径）
  emit_psi(true);   // PAT
  emit_psi(false);  // PMT —— 开局声明双 PID（细则③）
  // ★d286：开局先铺 3 帧数字静音，保证探测窗口里**一定**有一枚完整音频 PES。
  //   真机取证（22:35 会话 ts_dump.ts 逐包定位）：客户端下发顺序是「视频首帧
  //   （IDR 24235B）→ 音频协商」，复用器于是把 24KB 视频 PES 排在最前面，
  //   **第一枚音频 PES 落在第 26508 字节**。mpv 的 per-file 探测预算（d285 起
  //   =16384B）整窗只看得到视频 ⇒ lavf 报
  //   "Could not find codec parameters for stream 1 (Audio: aac…): unspecified
  //   sample format" ⇒ 音频轨没有 config、播放循环钉在 time-pos=0（真机 140s 不动）。
  //   同一文件离线重放能自愈（数据连续、探测后仍有整段可解析），真机实时流不能 ——
  //   所以这一枚静音帧是**起播门**，不是可有可无的填充。
  //   成本 ≈60B；kEldSilence 已由 build/test_ts/eld_silence.cpp 实测 RMS=0（d278），
  //   顺带把音频轴推离 0，后续 tick 的 fill_silence 无缝接管。
  if (has_audio_) {
    for (int i = 0; i < 3; i++) {
      if (!emit_pes(kAudioPid, kCcAudio, 0xC0, kEldSilence, sizeof(kEldSilence), audio_axis_))
        break;
      last_audio_pts_ = audio_axis_;  // d289：单调基准随之推进
      audio_axis_ += kAudioFrameTicks;
    }
  }
  return !broken_;
}

void TsMuxer::stop() {
  sink_ = nullptr;
  has_video_ = has_audio_ = false;
  have_audio_ = false;
  audio_axis_ = 0;
  last_audio_pts_ = 0;    // d289：单调基准随会话复位
  last_video_pts_ = 0;    // d311：视频单调基准随会话复位
  // d289：诊断计数器**故意不在此清零** —— stop() 之后调用方还要读它们做收尾统计
  //（airplay_stream 的 mux_.reset() 紧随其后，清了就永远读不到）。归零点前移到
  // start()：那是"新会话开始"的正确语义。
  pes_buf_.clear();
  broken_ = false;
  last_psi_at_ = 0;          // d282：复位 PSI 周期锚
  psi_seeded_ = false;
  broken_reason_ = nullptr;  // d273：复位时一并清诊断原因
}

// —— 唯一的 TS 打包口 ——
// body_len==0 && with_pcr → PCR-only 包（afc=0b10，CC 不递增——无 payload）；
// body_len==184 → 纯载荷包（无 AF，afc=0b01）；
// 其余 → AF（flags [+ PCR] + 0xFF stuffing）承载对齐（afc=0b11）。
bool TsMuxer::write_packet(uint16_t pid, int cc_slot, bool pusi, const uint8_t* body,
                           size_t body_len, bool with_pcr, uint64_t pcr_90k) {
  if (broken_) return false;
  if (body_len > kPayload) {
    broken_reason_ = "body_len>184";  // d273
    broken_ = true;
    return false;
  }
  const bool payload_present = body_len > 0;
  const bool need_af = payload_present ? (body_len < kPayload || with_pcr) : true;
  const uint8_t afc =
      payload_present ? (need_af ? 0x3u : 0x1u) : 0x2u;  // 11=AF+载荷 01=纯载荷 10=纯 AF
  uint8_t pkt[kTsLen];
  std::memset(pkt, 0, sizeof(pkt));  // d269：防御性清零（ stuffing bug 曾让包尾带栈垃圾）
  pkt[0] = 0x47;
  pkt[1] = (uint8_t)((pusi ? 0x40u : 0x00u) | ((pid >> 8) & 0x1Fu));
  pkt[2] = (uint8_t)(pid & 0xFFu);
  // ★★d278 铁律：CC = 「上次写出去的值」，有载荷包**先自增后写**、无载荷包原值写出。
  //   逐字对齐 FFmpeg 参考实现 mpegtsenc.c：
  //     有载荷：`ts_st->cc = (ts_st->cc + 1) & 0xf; *q++ = 0x10 | ts_st->cc;`
  //     纯 AF（PCR-only）：`mpegts_insert_pcr_only_pid(s, pid, ts_st->cc, …)` 里
  //       写 `0x20 | (cc & 0xf)`，**不自增**（原注释："Continuity Count field does
  //       not increment (see 13818-1 section 2.4.3.3)"）。
  //   消费侧 mpegts.c:3233 handle_packet：
  //     `expected_cc = has_payload ? (last_cc + 1) & 0xf : last_cc;`
  //     `tss->last_cc = cc;`   ← afc=2 的包也照样更新 last_cc
  //   所以 afc=2 包的 CC **必须等于上一包写出去的值**。
  //   旧实现"先写后自增"在纯载荷流上看着没事（相邻载荷包差值仍为 1），只在插入了
  //   PCR-only 包的 PID（= 视频 PID，PCR 载体）上暴露：每次插入 2 处失配 →
  //   `pc->flags |= AV_PKT_FLAG_CORRUPT` 落到当前正在组装的 PES（new_pes_packet
  //   取 pes->flags）→ 实测 9321 个视频 AU 里 3137 个（33.7%）被判 CORRUPT →
  //   mpv 丢参考帧（"number of reference frames ... discarding one"）→ **画面冻结**。
  if (payload_present) cc_[cc_slot] = (uint8_t)((cc_[cc_slot] + 1) & 0x0F);
  pkt[3] = (uint8_t)((afc << 4) | (cc_[cc_slot] & 0x0Fu));
  size_t off = 4;
  if (need_af) {
    // AF：length(1) + flags(1) [+ PCR(6)] + stuffing(0xFF×n)
    // af_len 值 = flags 起的字节数（不含 length 字段自身）
    size_t af_len = 1 + (with_pcr ? 6 : 0);
    size_t stuff = 0;
    if (payload_present && body_len < kPayload) {
      // stuffing 计入 AF：4 头 + 1 length + af_len + body_len = 188，
      // 其中 af_len = 1(flags) + stuffing —— 故 stuffing = af_len - 1。
      // ★d269 修复：原式 stuff = kPayload - body_len - 1 - af_len 恒为 0，
      //   af_len 声称的 stuffing 从未写入 → 包尾 157B 栈垃圾随包发出，
      //   PSI/PES 尾包全坏（lavf 跳 AF 后 payload 错位）——真机表现为
      //   mpv probe 拿不到合法 PAT/PMT，loadfile 卡 TRANSITIONING 15s 强停。
      af_len = kPayload - body_len - 1;
      if (af_len == 0) {
        // ★d272 防御兜底：body_len==183 时 AF 路径差 1 字节对不齐（AF 最小
        //   2 字节），af_len-1 下溢曾致栈数组越界写穿（fastfail 硬崩无 dump）。
        //   正常数据由 emit_pes 重切规避，这里只挡未来新调用点踩坑。
        broken_reason_ = "af_len==0(body_len=183 未重切)";  // d273
        broken_ = true;
        return false;
      }
      stuff = af_len - 1;
      if (with_pcr && af_len < 8) {
        // PCR 需 8 字节 AF 下限；本组件调用形态（PES 整流切块 / PSI 单包 /
        // PCR-only）不会触发，防御性判坏。
        broken_reason_ = "af_len<8 with pcr";  // d273
        broken_ = true;
        return false;
      }
    }
    pkt[off++] = (uint8_t)af_len;
    pkt[off++] = with_pcr ? 0x10 : 0x00;  // PCR_flag
    if (with_pcr) {
      // 42-bit PCR = base(33b)*300 + ext(9b)；线序 = base‖reserved(6)‖ext(9)
      const uint64_t v = ((uint64_t)(pcr_90k & 0x1FFFFFFFFull) << 15) | (0x3Full << 9);
      for (int i = 0; i < 6; i++) pkt[off + i] = (uint8_t)((v >> (8 * (5 - i))) & 0xFFu);
      off += 6;
    }
    for (size_t i = 0; i < stuff; i++) pkt[off++] = 0xFF;
  }
  if (off + body_len > kTsLen) {
    broken_reason_ = "off+body_len>188";  // d273
    broken_ = true;
    return false;
  }
  if (payload_present) std::memcpy(pkt + off, body, body_len);
  return sink_ && sink_(pkt, kTsLen);
}

// —— PSI（PAT/PMT）：单 section 单包（pointer_field + section + AF stuffing）——
bool TsMuxer::emit_psi(bool pat) {
  // ★d276 铁律：section_length 一律由「实际写出的字节数」回填，禁止手算。
  //   旧实现手算 PAT 漏 CRC（-4）、PMT 多算 2（ES_info_length 重复计入）→
  //   声明长度与实写长度不符 ⇒ FFmpeg pmt_cb 判段损坏、整段不顾
  //   （trace 里表现为没有 pcr_pid= 行，退化成按 PID 盲探 → 音频误判 mp3）。
  //   布局：sec[0]=table_id，sec[1..2]=长度（回填），sec[3..]=正文。
  uint8_t sec[kPayload];
  size_t n = 0;
  if (pat) {
    sec[0] = 0x00;  // table_id = PAT
    put16(sec + 3, 0x0001);  // transport_stream_id = 1
    sec[5] = 0xC1;           // reserved(11)=111, version=0, current_next=1
    sec[6] = 0x00;           // section_number
    sec[7] = 0x00;           // last_section_number
    put16(sec + 8, 0x0001);  // program_number = 1
    put16(sec + 10, 0xE000u | kPmtPid);
    n = 12;
    // 长度必须先回填再算 CRC（CRC 覆盖长度字段本身）
    sec[1] = (uint8_t)(0xB0u | (((n + 4 - 3) >> 8) & 0x0Fu));
    sec[2] = (uint8_t)((n + 4 - 3) & 0xFFu);
    put32(sec + n, mpeg_crc32(sec, n));  // ★CRC 必写（漏则整段作废）
    n += 4;
  } else {
    const uint16_t pcr_pid = has_video_ ? kVideoPid : kAudioPid;
    // d275：音频走「裸 ELD 帧 + PMT 携带 MPEG-4 描述符」。
    // ★为什么（d275 定案）：FFmpeg 的 aac/aac_latm 解码器对 ER(ELD) 帧的输出
    //   sample_rate 取自 avctx->sample_rate，而该值只随容器 codecpar 来；
    //   TS/LATM 通道没有任何环节会设它（上游缺口，n7.1/master 均未修）→
    //   输出帧恒被判 invalid（"An invalid frame was output by a decoder"）
    //   → 音频全灭、视频卡第一帧。解法：PMT 带描述符让 lavf 把
    //   codec=AAC / sample_rate=44100 / extradata=ASC 直接写进 codecpar，
    //   解码器走 plain aac + ER 路径（mpv_probe 实测 [ELD] 2ch 44100Hz 全绿）。
    //   裸帧无重同步机制 → 音频必须一帧一 PES（feed_audio 契约）。
    uint8_t iod[64];
    const size_t iod_n = has_audio_ ? build_iod(iod, kAudioPid, kEldAsc, sizeof(kEldAsc)) : 0;
    // 音频 ES_info：SL_DESCRIPTOR(0x1e) = ES_ID 关联（触发 dec_config_descr 应用）
    const uint8_t sl_desc[4] = {0x1e, 2, (uint8_t)(kAudioPid >> 8), (uint8_t)(kAudioPid & 0xFF)};
    const size_t sl_n = has_audio_ ? sizeof(sl_desc) : 0;

    // es_n 仅用于计长度； Intellectual 布局<｜hy_place▁holder▁no▁813｜>正文后再回填下列长度字段。
    const int es_n = (has_video_ ? 1 : 0) + (has_audio_ ? 1 : 0);
    (void)es_n;
    const uint16_t pil = (uint16_t)iod_n;
    uint8_t* p = sec;
    *p++ = 0x02;  // table_id = PMT
    *p++ = 0x00;  // section_length 占位（回填）
    *p++ = 0x00;
    put16(p, 0x0001);  // program_number = 1
    p += 2;
    *p++ = 0xC1;  // version/current_next
    *p++ = 0x00;  // section_number
    *p++ = 0x00;  // last_section_number
    put16(p, 0xE000u | pcr_pid);  // reserved(3)=111 + PCR_PID(13)
    p += 2;
    put16(p, 0xF000u | pil);  // program_info_length
    p += 2;
    if (has_audio_) {
      std::memcpy(p, iod, iod_n);
      p += iod_n;
    }
    if (has_video_) {
      // ★d292/d294：stream_type 应如实反映客户端送来的编码（AVC=0x1b / HEVC=0x24）。
      //   本行原先硬编码 0x1b。**注意**：d294 已撤回「15:56 那次是 HEVC、0x1b 是病灶」的判词 ——
      //   真实流一直是 **H.264**（日志三次 `video-format` 全为 h264），我那次是**解析器错误**
      //   （payload 偏移手算差 1 字节 + 套HEVC 三字节NAL 规则读 H.264 四字节流）。
      //   ⇒ 这个分支**目前不会走到**，保留它是为了「若真送来 HEVC 则如实声明」——
      //   **声明错编码会让下游用错解析器，比不声明更糟**（这是保留的理由，不是当前病灶）。
      //   待验：真送 HEVC 时 mpv 侧能否硬解（本机AMD HD 6570存疑）、探测窗是否够
      //   （HEVC 首帧含 VPS+SPS+PPS+IDR，比 H.264 更大，同一个 16384 字节门可能不够）。
      *p++ = hevc_ ? 0x24 : 0x1b;
      put16(p, 0xE000u | kVideoPid);
      p += 2;
      put16(p, 0xF000u);  // ES_info_length = 0
      p += 2;
    }
    if (has_audio_) {
      // stream_type 0x0f = AAC。★不是 0x11（LATM）：载荷已改裸 ELD 帧，
      // 且 SL 描述符让 lavf 设 need_parsing=0，ADTS 解析器不会运行——
      // 0x0f 是此刻语义上诚实的声明（AAC with ASC extradata）。
      *p++ = 0x0f;
      put16(p, 0xE000u | kAudioPid);
      p += 2;
      put16(p, (uint16_t)(0xF000u | sl_n));  // ES_info_length = SL_DESCRIPTOR
      p += 2;
      std::memcpy(p, sl_desc, sl_n);
      p += sl_n;
    }
    // 先回填 section_length（= 含 CRC 的实写总字节 - 3），再算 CRC
    {
      const size_t body_n = (size_t)(p - sec) + 4;  // 含即将写入的 CRC
      const size_t slen = body_n - 3;
      sec[1] = (uint8_t)(0xB0u | ((slen >> 8) & 0x0Fu));
      sec[2] = (uint8_t)(slen & 0xFFu);
    }
    put32(p, mpeg_crc32(sec, (size_t)(p - sec)));
    p += 4;
    n = (size_t)(p - sec);
  }
  uint8_t payload[kPayload];
  payload[0] = 0x00;  // pointer_field = 0（section 紧随）
  std::memcpy(payload + 1, sec, n);
  return write_packet(pat ? kPatPid : kPmtPid, pat ? kCcPat : kCcPmt, true, payload,
                      n + 1, false, 0);
}

// —— PES 组包 + 整流逐包切分 ——
bool TsMuxer::emit_pes(uint16_t pid, int cc_slot, uint8_t stream_id, const uint8_t* payload,
                       size_t len, uint64_t pts_90k) {
  if (broken_ || !sink_) return false;
  if (len == 0) return true;  // 空帧防御
  const bool video = (stream_id & 0xF0u) == 0xE0u;
  // PES：prefix(3) + stream_id(1) + PES_packet_length(2) + flags(2) + header_len(1) + PTS(5)
  const uint32_t pes_len = 3 + 5 + (uint32_t)len;  // length 字段覆盖范围（不含前 6B）
  const uint16_t len16 = (video && pes_len > 0xFFFFu) ? 0x0000u : (uint16_t)pes_len;
  pes_buf_.clear();
  pes_buf_.reserve((size_t)len + 14);
  pes_buf_.insert(pes_buf_.end(), {0x00, 0x00, 0x01});
  pes_buf_.push_back(stream_id);
  pes_buf_.push_back((uint8_t)(len16 >> 8));  // PES_packet_length（高位在前）
  pes_buf_.push_back((uint8_t)(len16 & 0xFF));
  pes_buf_.push_back(0x84);  // '10' + scrambling 00 + priority 0 + data_alignment=1
  pes_buf_.push_back(0x80);  // PTS_DTS_flags='10'（PTS only）
  pes_buf_.push_back(5);     // header_data_length = PTS(5)
  {
    uint8_t ts[5];
    put_pts(ts, pts_90k);
    pes_buf_.insert(pes_buf_.end(), ts, ts + 5);
  }
  pes_buf_.insert(pes_buf_.end(), payload, payload + len);
  // 切包：首包 PUSI=1；中间包满载荷；末包 stuffing。
  // ★d272：n==kPayload-1（183B）必须重切为 182——TS 打包死角：AF 最小占
  //   2 字节（length+flags），183B 载荷 + AF = 186，差 1 字节凑不满 188，
  //   AF 路径 af_len 会算出 0 → stuff = af_len-1 下溢 → 栈数组越界写穿
  //   （/GS fastfail 硬崩，无 dump——d271 版真机 23:04 死亡即此）。
  size_t off = 0;
  bool first = true;
  while (off < pes_buf_.size()) {
    size_t n = std::min(kPayload, pes_buf_.size() - off);
    if (n == kPayload - 1) n = kPayload - 2;  // 183B 无法对齐，重切（余量进后续包）
    if (!write_packet(pid, cc_slot, first, pes_buf_.data() + off, n, false, 0)) return false;
    off += n;
    first = false;
  }
  return true;
}

// ★d289：视频**向音频对齐**（契约 §5.3/§D1「音频为基准流，视频跟随、赶不上就丢」）。
//   此前本函数原样写出调用方给的 pts_90k，而调用方给的是**视频帧自己的到达钟**——
//   于是视频成了时间轴主锚、音频反过来被 fill_silence 拖着走（d288 判决：与契约相反）。
//   现在：pts 取音频轴现值（轴由音频帧推进，见 feed_audio）。
//   落后超过 kVideoDropTh ⇒ **丢这一帧视频**并计数：视频丢几帧用户无感，
//   音频丢一帧就直接报废（用户原话：「视频丢几帧很难感受到，但音频丢失无法正常播放」）。
bool TsMuxer::feed_video(const uint8_t* annexb_au, size_t len, uint64_t pts_90k) {
  if (broken_ || !sink_) return false;
  if (!has_video_ || !annexb_au || len == 0) return len == 0;
  // ★d289b修复（真机投屏黑屏无声的直接原因，d289 首版引入）：
  //   d289 首版把「pts > 到达钟 + kVideoDropTh(20ms) 就丢帧」当对齐手段，
  //   而 kVideoDropTh **比一帧（30fps = 3000 tick = 33ms）还小** ⇒ 真机
  //   「视频丢帧+387→+537→+687…每 5s 正好 +150」与「mux消费+150」一一对应
  //   = **视频帧 100% 被丢光**，TS 里只剩音频 PES + PSI + PCR（输出包数从
  //   ~21000/5s 掉到 ~1100/5s），mpv 无视频轨 ⇒ time-pos恒 0、vfps=null、
  //   有声无画。教训：**「对齐」不能用「超阈值就丢」实现** —— 阈值小于帧间隔时
  //   丢的不是落后帧而是全部帧。
  //
  //   正确语义（契约 §5.3「视频跟随音频」）：视频 PTS **直接取音频轴**，不做丢弃。
  //   音频轴单调、已铺满（d278），视频挂上去即天然同域；视频帧率与音频帧率
  //   各自的抖动由 mpv 的 video-sync=audio 在显示层吸收（§5.3 分工：复用器管
  //   相位、mpv 管速率）。**复用器不丢视频帧** —— 丢帧留给 mpv（帧级丢弃，
  //   丢的是"显示"不是"PES"，不会破坏参考链）。
  //   保留一个**真正的异常兜底**：音频尚未起轴时（have_audio_ 为 false）用自身到达钟。
  //
  //   ★PTS 取 `last_audio_pts_`（已发出的最后一枚音频 PTS）而不是 `audio_axis_`
  //   （下一个要发的）：后者比前者恒领先一帧（980 tick），挂上去会让每一帧视频
  //   都比音频早10.9ms，一帧接一帧累积 ⇒ 视频相对音频**系统性超前**。
  //   last_audio_pts_ 才是「当前媒体时间上音频所在的位置」。
  //   ★不再保留相对偏移：视频自身到达钟与音频轴的差值含网络抖动（±几十 ms），
  //   写进 PTS 只会把抖动固化成音画偏移；交给 mpv 的 video-sync=audio 去吸收。
  //   （d288 那恒定 2.778s 正说明这类差值不可信 —— 它由客户端钟的基准偏造成。）
  //
  //   ★★d311：PTS 重复修复（d297 定案、d298 撤回后的**同基准重做**）。
  //   病灶：`use = last_audio_pts_` 让「同一音频 tick 内到达的多帧视频」拿到
  //   **完全相同的 PTS** —— 音频 92fps（10.9ms/帧）vs 视频 24fps（42ms/帧），
  //   视频帧到达有网络突发，同 tick 双帧就撞车。真机实测：mpv 判零增量
  //   `Invalid video timestamp` 每轮 1816 条，vfps 29.9→3.7、time-pos 冻 3 秒
  //   后跳帧追上 ⇒ 用户感知的「延迟约 5 秒」（冻结期间延迟每秒涨 1 秒）。
  //   修法：**锚不动、只补步进** —— use ≤ last_video_pts_ 时只加亚帧级最小步进
  //   kVideoPtsMinStep（=1 tick，11µs @90kHz）。★步进取值的判责（21:15 复盘）：
  //   首版取 90 tick（1ms），真机实测视频 TCP 突发可把 **83 帧**挤进同一音频 tick
  //   （队列「音频优先」但 aq_ 瞬时为空时 vq_ 被连续排空 ⇒ 83 帧同 PTS 是 d297
  //   缺陷的随机放大形态，21:08 会话 `Invalid video timestamp` 124.556433 一值
  //   连刷 83 次）⇒ 83×1ms=83ms 视频超前音频，**超过 40ms 守门线**（d298 纪律）。
  //   改 1 tick：mpv 触发条件是 PTS **相等**（零增量），严格递增即可；83 帧突发
  //   最大扰动 0.92ms、千帧病态突发也 <1ms，守门线内余量百倍。突发帧在显示层
  //   "尽快连播"正是突发的正确语义（帧时距 11µs ⇒ 立即显示，随后回锚音频）。
  static constexpr uint64_t kVideoPtsMinStep = 1;  // 1 tick = 11µs @90kHz（亚帧级）
  uint64_t use = have_audio_ ? last_audio_pts_ : pts_90k;
  if (use <= last_video_pts_) {
    use = last_video_pts_ + kVideoPtsMinStep;
    video_pts_bumps_++;  // 正信号：突发/同 tick 多帧时 >0；恒 0 = 没撞过车
  }
  last_video_pts_ = use;
  return emit_pes(kVideoPid, kCcVideo, 0xE0, annexb_au, len, use);
}



// d275：payload = 单个裸 AAC-ELD 帧（不再 LOAS 包装，供参走 PMT 描述符）。
// 一帧一 PES 是硬契约：裸帧无重同步，帧跨 PES 边界即碎。
// d278：本帧落在**单调音频轴**上 —— 与轴偏差 ≤90ms 才采用传入 PTS（保留客户端
// 细粒度节奏）；偏差更大（客户端重开音频流 → 远端钟往回跳 / 跳出伪值）一律改用
// 轴，保证 emit 出去的 PTS 序列永不倒挂、永无 ≥5s 跳变（否则 mpv 复位整个播放）。
// ★d289：音频是**基准流**（契约 §5.3/§D1）—— 轴由本函数推进（每帧 +980），
//   视频/PCR/静音填充全部以它为准。此前轴是被fill_silence 按视频 PTS 拖着走的。
bool TsMuxer::feed_audio(const uint8_t* eld_frame, size_t len, uint64_t pts_90k) {
  if (broken_ || !sink_) return false;
  if (!has_audio_ || !eld_frame || len == 0) return len == 0;
  // ★d289 音频轴回拉（防 d288 判决的一次性永久错位）—— 回到契约 §5.3「音频是基准流」。
  //
  //   ★★判据为什么必须用「实时基线 real_90k_」而不是调用方给的 pts_90k
  //   （第一版就是这么写的，定向回归场景 B 判为**假绿灯**抓出来的）：
  //   d288 病灶的机理是**轴整体超前于真实媒体时间**（轴当时跑在视频 PTS 上，
  //   而视频 PTS 相对实时基线偏快）。若拿"轴 vs 传入 pts"比较——而调用方传的
  //   pts_90k 本身也是从同一条被污染的链推出来的——两者会一起超前、差值恒为 0，
  //   纠偏条件永不成立（实测场景 B：纠偏 0 次）。
  //   实时基线来自 tick(now_90k)，它由**单调到达钟**独立算出，是"真实媒体时间"
  //   的唯一客观参照 ⇒ 拿它当基准，纠偏才抓得住真正的超前。
  //
  //   ★为什么可以拉：① 回拉幅度按阈值分档、单次远小于 mpv 的 5s 复位线（d278）；
  //   ② 新轴必须 > last_audio_pts_（已发出的最后一帧）⇒ 轴始终严格单调不倒挂；
  //   ③ 绝不让轴后退到已发出 PTS 之后 —— d278 的红线是"单调"，不是"精确"。
  if (real_seeded_ && audio_axis_ > real_90k_ + kAxisCorrectionTh &&
      real_90k_ + kAudioFrameTicks > last_audio_pts_) {
    audio_axis_ = real_90k_ + kAudioFrameTicks;
    axis_corrections_++;
  }
  uint64_t use = audio_axis_;
  // 调用方 pts 仅用于"客户端细粒度节奏"的小幅贴合（≤90ms 才采信，d278 原有语义）。
  if (pts_90k > audio_axis_ && pts_90k - audio_axis_ <= kAudioMaxLead) use = pts_90k;
  // 兜底：任何路径都不得写出 ≤ 已发出末帧的 PTS（单调是 mpv 的硬要求）。
  if (use <= last_audio_pts_) use = last_audio_pts_ + kAudioFrameTicks;
  if (!emit_pes(kAudioPid, kCcAudio, 0xC0, eld_frame, len, use)) return false;
  last_audio_pts_ = use;
  audio_axis_ = use + kAudioFrameTicks;
  return true;
}



// 细则②（d278 重写）：把音频轴补到实时时间 —— 缺口一律用**真静音 ELD 帧**填平，
// 无上限（旧实现对 >250ms 的缺口直接放弃填充，于是在 TS 里留下 5s/10s/104s 的空洞，
// mpv 每次遇到就 `pts_reset` 复位播放，表现为"没有声音"）。
bool TsMuxer::fill_silence(uint64_t now_90k) {
  if (!has_audio_ || !have_audio_) return true;
  for (int i = 0; i < kSilenceMaxPerTick; i++) {
    if (now_90k < audio_axis_ + kSilenceMinGap) return true;  // 已跟上实时时间
    if (!emit_pes(kAudioPid, kCcAudio, 0xC0, kEldSilence, sizeof(kEldSilence), audio_axis_))
      return false;
    last_audio_pts_ = audio_axis_;  // d289：与 feed_audio 共用单调基准
    audio_axis_ += kAudioFrameTicks;
  }
  return true;
}

// ★d429：PSI 立即重发请求（见 ts_muxer.h 注释）。一次性：置位后由最近一轮 tick
//   消费（tick 里的 PSI 段 exchange 清零），期间多次置位幂等。
void TsMuxer::request_psi_resync() {
  psi_resync_.store(true, std::memory_order_release);
}

void TsMuxer::tick(uint64_t now_90k) {
  if (broken_ || !sink_) return;
  // d272 防御：契约要求 now 单调递增（mux 线程的 mono_sec 保证）。若调用方
  // 违约传了回退值，now_90k - last_pcr_at_ 无符号下溢会让 PCR while 变永真
  // 循环（定向回归探针实锤）。静默忽略回退 tick，不烧线程。
  if (pcr_seeded_ && now_90k < last_pcr_at_) return;
  // ★d289：记下实时基线 —— feed_audio 的轴回拉判据要用它（见该函数注释：
  //   为什么不能用调用方 pts_90k：那条链可能与轴一起被污染，判据会永不成立）。
  real_90k_ = now_90k;
  real_seeded_ = true;
  if (!pcr_seeded_) {
    last_pcr_at_ = now_90k;  // 轴基准 = 首个 tick（调用方在流起播时刻开 tick）
    pcr_seeded_ = true;
  }
  // ★d282：PAT/PMT 周期重发（见 kPsiPeriod 说明）。周期锚在**轴**上：轴不走 =
  //   没有数据在发，不必补 PSI；一旦恢复（轴跳跃），下一轮立刻补发 ⇒ 丢片/停滞
  //   之后 mpv 拿到完整 PSI 即可重新同步，不必等 EOF。
  //   ★d429：request_psi_resync() 置位的一次性请求同样在此消费 —— 回环重连后
  //   新 demuxer 的 16384B 探测窗内必须出现 PAT/PMT（否则 PMT 弃用、音频盲探成
  //   mp3、视频轨缺失），不能赌 100ms 周期恰好落在窗内。
  if (psi_resync_.exchange(false, std::memory_order_acq_rel) ||
      !psi_seeded_ || now_90k - last_psi_at_ >= kPsiPeriod) {
    psi_seeded_ = true;
    last_psi_at_ = now_90k;
    if (!emit_psi(true)) return;   // PAT
    if (!emit_psi(false)) return;  // PMT
  }
  // 细则①：PCR 严格按 last_pcr_at_ + 间隔 推进（单调，与帧到达解耦）
  while (now_90k - last_pcr_at_ >= kPcrInterval) {
    const uint64_t pcr_t = last_pcr_at_ + kPcrInterval;
    if (!write_packet(has_video_ ? kVideoPid : kAudioPid, has_video_ ? kCcVideo : kCcAudio,
                      false, nullptr, 0, true, pcr_t))
      return;
    last_pcr_at_ = pcr_t;
  }
  fill_silence(now_90k);
}

}  // namespace fr::cast
