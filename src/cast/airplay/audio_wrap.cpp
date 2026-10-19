// audio_wrap 实现 —— 见 audio_wrap.h（§5.2 包装器：ELD→LOAS/LATM、ALAC→CAF、LC→ADTS）。
// 纯字节/位操作，无日志无锁；所有常量出处见头文件注记。

#include "cast/airplay/audio_wrap.h"

#include <cstring>
#include <vector>

namespace fr::cast {

namespace {

// —— 位写入器（MSB-first）——
class BitWriter {
 public:
  void put(uint32_t val, int bits) {
    for (int i = bits - 1; i >= 0; i--) {
      if (bit_pos_ == 0) buf_.push_back(0);
      const uint8_t b = (uint8_t)((val >> i) & 1u);
      buf_.back() |= (uint8_t)(b << (7 - bit_pos_));
      bit_pos_ = (uint8_t)((bit_pos_ + 1) & 7);
    }
  }
  void byte_align() {
    if (bit_pos_ != 0) bit_pos_ = 0;  // 当前字节就此截断（后续 put 从新字节开始）
  }
  std::vector<uint8_t> take() {
    return std::move(buf_);
  }

 private:
  std::vector<uint8_t> buf_;
  uint8_t bit_pos_ = 0;
};

// MuxSlotLengthBytes：8 位值链，0xFF = 续传（>255B 帧的转义序列）
void put_mux_slot_len(BitWriter& bw, size_t bytes) {
  while (bytes >= 255) {
    bw.put(0xFF, 8);
    bytes -= 255;
  }
  bw.put((uint32_t)bytes, 8);
}

}  // namespace

size_t latm_wrap(const uint8_t* frame, size_t len, uint8_t* out, size_t out_cap) {
  if (!frame || len == 0 || !out) return 0;
  // audioMuxElement 位流（d275 修正版，对照 FFmpeg n7.1 aacdec_latm.h 逐位核过）：
  //   useSameStreamMux(1)=0, audioMuxVersion(1)=0, allStreamsSameTimeFraming(1)=1,
  //   numSubFrames(6)=0, numProgram(4)=0, numLayer(3)=0 → ASC(★30 位，FFmpeg ELD
  //   路径恰好消费 30 位) → frameLengthType(3)=000 → latmBufferFullness(8)=0xFF
  //   → otherDataPresent(1)=0 → crcCheckPresent(1)=0 → MuxSlotLengthBytes 链
  //   → 载荷（★不字节对齐，紧跟链尾按当前位相位写入）。
  // ★otherDataPresent/crcCheckPresent 是 ISO 14496-3 StreamMuxConfig 的语法位，
  //   此前漏写：FFmpeg 在 BufferFullness 后读 otherDataPresent=链首字节最高位
  //   （载荷≥255B 时 0xFF→1），把我们的长度链当 other-data/长度交叉解析，声明
  //   载荷长度错成杂凑值 → "frame length mismatch" 每帧必拒（d275 模拟器三对
  //   错误值与真机日志逐位一致）。ASC 写 30 位而非 32：FFmpeg 的 m4ac(19)+ELD
  //   SpecificConfig(11) 恰好 30 位，多写的 2 位会整体后移所有后续字段。
  // ★numSubFrames 是 **6 位**（ISO/IEC 14496-3 §4.4.2.4），不是 4 位（d249 实测
  //   写 4 位会错位出 "Multiple layers is not implemented"）。
  BitWriter bw;
  bw.put(0, 1);   // useSameStreamMux = 0（每帧重申 —— 对流式场景最稳，解码端免状态）
  bw.put(0, 1);   // audioMuxVersion = 0
  bw.put(1, 1);   // allStreamsSameTimeFraming = 1
  bw.put(0, 6);   // numSubFrames = 0（1 个子帧）
  bw.put(0, 4);   // numProgram = 0（1 个 program）
  bw.put(0, 3);   // numLayer = 0（1 层）
  bw.put(kEldAscBits30, 30);  // AudioSpecificConfig（ELD，FFmpeg 消费恰 30 位）
  bw.put(0, 3);   // frameLengthType = 0（BufferFullness 模式）
  bw.put(0xFF, 8);  // latmBufferFullness
  bw.put(0, 1);   // otherDataPresent = 0（★d275 补写）
  bw.put(0, 1);   // crcCheckPresent = 0（★d275 补写）
  put_mux_slot_len(bw, len);
  // 载荷：不做 byte_align——FFmpeg 读完长度链后从当前位直接续读 AAC 位流
  //（载荷起点 ≡ 3 (mod 8)，对齐反而错位 3~5 bit）。逐位写入保持比特序不变。
  for (size_t i = 0; i < len; i++) bw.put(frame[i], 8);
  std::vector<uint8_t> body = bw.take();  // 长度=ceil(总位数/8)，尾部自然补零
  // AudioSyncStream 帧长域（13 bits，单位字节）= audioMuxElement 字节数
  const size_t element_len = body.size();
  if (3 + element_len > out_cap) return 0;
  out[0] = 0x56;  // syncword 0x2B7（11 位）的高 8 位
  out[1] = (uint8_t)(0xE0u | ((element_len >> 8) & 0x1Fu));  // syncword 低 3 位=0b111 + 长度高 5 位
  out[2] = (uint8_t)(element_len & 0xFFu);
  std::memcpy(out + 3, body.data(), body.size());
  return 3 + element_len;
}

size_t adts_wrap(const uint8_t* frame, size_t len, uint8_t* out, size_t out_cap) {
  if (!frame || len == 0 || !out) return 0;
  if (7 + len > out_cap) return 0;
  // ADTS：sync FFF + ID0 + layer00 + protection_absent1 + profile(LC=01)
  //      + sf_index 0100(44100) + private 0 + channel 010 + ...
  out[0] = 0xFF;
  out[1] = 0xF1;                 // sync FFF + ID=0 + layer=00 + protection_absent=1
  out[2] = (uint8_t)(0x40u | ((len >> 11) & 0x03u));  // profile=01(LC) + freq=0100 + ch 高 1 位=0
  out[3] = (uint8_t)(((len >> 3) & 0x1Fu) << 5);      // frame_length 高 5 位（ch/边位=0）
  out[4] = (uint8_t)(((len & 0x07u) << 5) | 0x1Fu);   // 低 3 位 + buffer_fullness 高位(0x7FF 全值)
  out[5] = 0xFC;                 // buffer_fullness 尾位 + num_frames-1 = 0
  out[6] = 0x00;                 // CRC absent
  std::memcpy(out + 7, frame, len);
  return 7 + len;
}

size_t CafWriter::write_header(uint8_t* out, size_t out_cap) {
  if (!out || out_cap < 108) return 0;
  size_t o = 0;
  auto put4 = [&](const char* tag) {
    for (int i = 0; i < 4; i++) out[o++] = (uint8_t)tag[i];
  };
  auto putu32 = [&](uint32_t v) { out[o++] = (uint8_t)(v >> 24); out[o++] = (uint8_t)(v >> 16);
                                  out[o++] = (uint8_t)(v >> 8);  out[o++] = (uint8_t)v; };
  // CAF 文件头（8B）：'caff' + version(1) + flags(2)
  put4("caff");
  out[o++] = 0x00; out[o++] = 0x01;  // version 1, flags 0 → 2 字节
  out[o++] = 0x00;
  // AudioDescription chunk（40B：头 8 + 32 体）：44100 / alac / 352 frames / 2ch / 16bit
  put4("desc");
  putu32(32);
  // sampleRate：double 44100（0x40E5888000000000）
  out[o++] = 0x40; out[o++] = 0xE5; out[o++] = 0x88; out[o++] = 0x80;
  out[o++] = 0x00; out[o++] = 0x00; out[o++] = 0x00; out[o++] = 0x00;
  put4("alac");              // formatID
  putu32(0);                 // formatFlags
  putu32(0);                 // bytesPerPacket = 0（VBR）
  putu32(352);               // framesPerPacket = spf 352（ALAC 44.1k）
  putu32(2);                 // channelsPerFrame
  putu32(16);                // bitsPerChannel
  // kuki chunk（8 + 36）：ALAC magic cookie，原样取自 UxPlay audio_renderer.c:61-62
  put4("kuki");
  putu32(36);
  static const uint8_t alac_cookie[36] = {
      0x00, 0x00, 0x00, 0x24, 0x61, 0x6c, 0x61, 0x63, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x01, 0x60, 0x00, 0x10, 0x28, 0x0a, 0x0e, 0x02, 0x00, 0xff,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xac, 0x44};
  std::memcpy(out + o, alac_cookie, 36);
  o += 36;
  // data chunk（12B）：'data' + size = -1（未定长流，lavf caf 支持）+ edit count 0
  put4("data");
  putu32(0xFFFFFFFF);
  putu32(0xFFFFFFFF);
  putu32(0);
  return o;  // 8 + 40 + 44 + 12 = 104? —— 实算：caff8 + desc(8+32=40) + kuki(8+36=44) + data(12) = 108
}

size_t CafWriter::wrap(const uint8_t* frame, size_t len, uint8_t* out, size_t out_cap) {
  if (!frame || len == 0 || !out || len > out_cap) return 0;
  std::memcpy(out, frame, len);
  return len;
}

}  // namespace fr::cast
