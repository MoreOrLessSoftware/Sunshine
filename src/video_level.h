/**
 * @file src/video_level.h
 * @brief Declarations for reading the level an encoder chose from its parameter sets.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

/**
 * @brief Reading the level and tier an encoder signals in H.264 and HEVC parameter sets.
 *
 * @details An encoder left to pick the level picks the lowest one the stream fits. Raising the
 *          bitrate while streaming can then need a higher level, which can't change without a
 *          new sequence. Reading the level back lets it be fixed, so later changes stay within it.
 */
namespace video::level {
  /**
   * @brief Level and tier of a stream.
   */
  struct level_t {
    std::uint32_t level;  ///< Level as coded: level_idc for H.264, general_level_idc (30 times the level) for HEVC.
    std::uint32_t tier;  ///< HEVC general_tier_flag: 0 for Main, 1 for High. Always 0 for H.264.
  };

  /**
   * @brief Split an Annex B byte stream into NAL units.
   *
   * @param stream Bytes with 3 or 4 byte start codes before each NAL unit.
   * @return Each NAL unit without its start code.
   */
  inline std::vector<std::span<const std::uint8_t>> nal_units(std::span<const std::uint8_t> stream) {
    std::vector<std::span<const std::uint8_t>> units;

    // Finds the byte after the next start code, from `from`
    const auto next_start = [&](std::size_t from) {
      for (auto i = from; i + 2 < stream.size(); ++i) {
        if (stream[i] == 0 && stream[i + 1] == 0 && stream[i + 2] == 1) {
          return i + 3;
        }
      }
      return stream.size();
    };

    auto begin = next_start(0);
    while (begin < stream.size()) {
      auto next = next_start(begin);
      auto end = next == stream.size() ? stream.size() : next - 3;

      // The leading zero of a 4 byte start code belongs to the next start code
      while (end > begin && stream[end - 1] == 0 && next != stream.size()) {
        --end;
      }

      units.push_back(stream.subspan(begin, end - begin));
      begin = next;
    }

    return units;
  }

  /**
   * @brief Remove emulation prevention bytes, the 0x03 that follows two zero bytes.
   *
   * @param nal NAL unit as it appears in the stream.
   * @return The NAL unit's raw bytes.
   */
  inline std::vector<std::uint8_t> unescape(std::span<const std::uint8_t> nal) {
    std::vector<std::uint8_t> raw;
    raw.reserve(nal.size());

    int zeros = 0;
    for (const auto byte : nal) {
      if (zeros >= 2 && byte == 3) {
        zeros = 0;
        continue;
      }
      zeros = byte == 0 ? zeros + 1 : 0;
      raw.push_back(byte);
    }

    return raw;
  }

  /**
   * @brief Read the level from an H.264 sequence parameter set.
   *
   * @param stream Annex B parameter sets, as the encoder emits them.
   * @return The level, or nothing if there is no complete SPS.
   */
  inline std::optional<level_t> h264_level(std::span<const std::uint8_t> stream) {
    for (const auto nal : nal_units(stream)) {
      // nal_unit_type 7 is an SPS. After the header come profile_idc, the constraint flags
      // and level_idc.
      if (nal.empty() || (nal[0] & 0x1F) != 7) {
        continue;
      }
      const auto raw = unescape(nal);
      if (raw.size() < 4) {
        return std::nullopt;
      }
      return level_t {raw[3], 0};
    }
    return std::nullopt;
  }

  /**
   * @brief Read the level and tier from an HEVC sequence parameter set.
   *
   * @param stream Annex B parameter sets, as the encoder emits them.
   * @return The level and tier, or nothing if there is no complete SPS.
   */
  inline std::optional<level_t> hevc_level(std::span<const std::uint8_t> stream) {
    for (const auto nal : nal_units(stream)) {
      // nal_unit_type 33 is an SPS. After the 2 byte header, one byte of layer fields, then
      // profile_tier_level: a byte with the tier flag, 4 bytes of compatibility flags, 6 of
      // constraint flags, then general_level_idc.
      if (nal.empty() || ((nal[0] >> 1) & 0x3F) != 33) {
        continue;
      }
      const auto raw = unescape(nal);
      if (raw.size() < 15) {
        return std::nullopt;
      }
      return level_t {raw[14], static_cast<std::uint32_t>((raw[3] >> 5) & 1)};
    }
    return std::nullopt;
  }
}  // namespace video::level
