#include <cstring>

#include "k230/inspector/decoder.hpp"
#include "k230/log.hpp"

namespace k230::inspector {

bool RawPcmAudioDecoder::open(CodecId codec) {
  clock_anchor_us_ = -1;
  clock_frames_ = 0;
  return codec == CodecId::Raw;
}

bool RawPcmAudioDecoder::decode(const MediaPacket& packet, std::vector<PcmChunk>& out) {
  if (packet.is_config) return true;
  if (packet.pts_us < 0 || packet.data.empty() || packet.data.size() % 4 != 0) return false;
  PcmChunk chunk;
  chunk.pts_us = packet.pts_us;
  chunk.sample_rate = 48000;
  chunk.channels = 2;
  chunk.samples.resize(packet.data.size() / 2);
  constexpr std::int64_t jitter_us = 2000;
  const auto expected_pts = clock_anchor_us_ + clock_frames_ * 1'000'000 / chunk.sample_rate;
  const auto delta = packet.pts_us - expected_pts;
  if (clock_anchor_us_ < 0 || packet.seq != previous_sequence_ + 1 ||
      delta < -jitter_us || delta > jitter_us) {
    chunk.timing_discontinuity = clock_anchor_us_ >= 0;
    clock_anchor_us_ = packet.pts_us;
    clock_frames_ = 0;
  }
  chunk.sample_clock_pts_us = clock_anchor_us_ + clock_frames_ * 1'000'000 / chunk.sample_rate;
  clock_frames_ += static_cast<std::int64_t>(chunk.frames());
  previous_sequence_ = packet.seq;
  for (std::size_t i = 0; i < chunk.samples.size(); ++i) {
    const std::uint16_t value = static_cast<std::uint16_t>(packet.data[2 * i]) |
                                static_cast<std::uint16_t>(packet.data[2 * i + 1]) << 8;
    chunk.samples[i] = static_cast<std::int16_t>(value < 0x8000 ? value : static_cast<std::int32_t>(value) - 0x10000);
  }
  out.push_back(std::move(chunk));
  return true;
}

std::unique_ptr<AudioDecoder> make_default_audio_decoder(CodecId codec) {
  if (codec == CodecId::Raw) return std::make_unique<RawPcmAudioDecoder>();
#ifdef K230_HAS_FFMPEG
  return make_ffmpeg_audio_decoder();
#else
  K230_LOG_ERROR("decoder") << "no decoder for audio codec " << to_string(codec)
                            << " on this target; run scrcpy with audio_codec=raw";
  return nullptr;
#endif
}

std::unique_ptr<VideoDecoder> make_default_video_decoder() {
#ifdef K230_HAS_FFMPEG
  return make_ffmpeg_video_decoder();
#else
  return make_vdec_video_decoder();
#endif
}

}  // namespace k230::inspector
