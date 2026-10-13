/**
 * @file src/pyrowave_records.h
 * @brief Declarations for laying out PyroWave frames in record framing.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

/**
 * @brief PyroWave record framing, as the nonary (Vibepollo) host defines it.
 *
 * @details A PyroWave frame is an 8 byte sequence header followed by self-delimiting block
 *          records, each a whole number of 32-bit words. In record framing the frame may also
 *          hold padding records, `0xFFFFFFFF`, a word count N, then N zero words, which the
 *          client strips. The host uses them to line records up with the packets the frame is
 *          sent in, so that a client that lost packets can still decode the rest of the frame:
 *
 *          1. The sequence header, then the coarsest wavelet level, without which nothing can be
 *             decoded. The packets holding them are the frame's critical packets, whose count
 *             goes in the frame header, and which get all of the session's FEC
 *             (critical_fec_blocks()).
 *          2. Every other record. Records too large to share a packet with a padding record
 *             come first and span packets; the rest are packed first-fit so that none crosses a
 *             packet boundary, and padding fills a packet only when no record fits.
 *
 *          Packets that start with a record are flagged, so a client that lost a record's header
 *          resumes parsing at the next of them. Blocks may be in any order, since PyroWave places
 *          them by index. Clients ask for record framing with `x-ss-video[0].pyrowaveFeatures`
 *          bit 0x1. See docs/pyrowave-protocol.md in Vibepollo, where the layout comes from.
 */
namespace pyrowave::records {
  constexpr std::uint32_t feature_record_framing = 0x1;  ///< `x-ss-video[0].pyrowaveFeatures` bit asking for record framing.
  constexpr std::uint8_t extra_flag_record_start = 0x80;  ///< `NV_VIDEO_PACKET::extraFlags` bit of a packet whose frame data starts with a record.
  constexpr std::size_t frame_header_bytes = 8;  ///< The short frame header before the frame in its first packet.
  constexpr std::size_t packet_overhead_bytes = 16;  ///< Bytes of a packet's `packetSize` taken by the video packet header.
  constexpr std::size_t header_bytes = 8;  ///< Size of a sequence header, block header or empty padding record.
  constexpr std::uint32_t padding_magic = 0xFFFFFFFFu;  ///< First word of a padding record.

  /**
   * @brief Work out how much of the frame each packet carries, for laying records out on them.
   *
   * @param packetsize The negotiated `packetSize`.
   * @return Frame bytes per packet, or 0 if records can't be lined up with packets of this size
   *         (not a multiple of four bytes, or too small), in which case the frame is sent as encoded.
   */
  inline std::size_t packet_payload_bytes(int packetsize) {
    if (packetsize <= (int) packet_overhead_bytes) {
      return 0;
    }
    const auto payload = std::size_t(packetsize) - packet_overhead_bytes;
    if (payload < frame_header_bytes + 2 * header_bytes || payload % 4 != 0) {
      return 0;
    }
    return payload;
  }

  /**
   * @brief Count the blocks of PyroWave's coarsest wavelet level, which it indexes first.
   *
   * @param width Frame width in pixels.
   * @param height Frame height in pixels.
   * @return Blocks with an index below this are in the coarsest level.
   */
  inline std::uint32_t coarse_block_count(std::uint32_t width, std::uint32_t height) {
    // Each dimension is padded to 32 pixels (at least 128), then halved by five wavelet levels.
    // The coarsest level has four bands of three components.
    const auto blocks = [](std::uint32_t extent) {
      const std::uint32_t aligned = std::max<std::uint32_t>((extent + 31) / 32 * 32, 128);
      return ((aligned >> 5) + 31) / 32;
    };
    return blocks(width) * blocks(height) * 4 * 3;
  }

  /**
   * @brief Count the packets a frame's first bytes are sent in.
   *
   * @param bytes Frame bytes, from its start.
   * @param payload Frame bytes per packet, from packet_payload_bytes().
   * @return Packets that hold them, the first also holding the frame header.
   */
  inline std::size_t packets_for(std::size_t bytes, std::size_t payload) {
    return (frame_header_bytes + bytes + payload - 1) / payload;
  }

  namespace detail {
    inline std::uint32_t read_u32(const std::uint8_t *p) {
      return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
    }

    inline void append_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
      out.push_back(std::uint8_t(value));
      out.push_back(std::uint8_t(value >> 8));
      out.push_back(std::uint8_t(value >> 16));
      out.push_back(std::uint8_t(value >> 24));
    }

    /**
     * @brief Finds the earliest remaining record no larger than a size.
     *
     * @details Records are bucketed by size in words (12 bits in their header), each bucket
     *          holding its records in frame order. A min segment tree over the buckets keeps each
     *          one's earliest remaining record, so the earliest record of at most N words is a
     *          prefix minimum. Records only leave the front of their bucket, as both lookups take
     *          a bucket's earliest.
     */
    class fit_index_t {
    public:
      static constexpr std::size_t none = std::numeric_limits<std::size_t>::max();  ///< No record found.

      /**
       * @brief Index records by size.
       *
       * @param words Each record's size in words, in frame order.
       */
      explicit fit_index_t(const std::vector<std::uint32_t> &words):
          heads(leaves + 1, 0),
          ends(leaves, 0),
          order(words.size()),
          tree(2 * leaves, none) {
        for (const auto w : words) {
          ++heads[w + 1];
        }
        for (std::size_t b = 0; b < leaves; b++) {
          heads[b + 1] += heads[b];
          ends[b] = heads[b + 1];
        }
        std::vector<std::size_t> fill(heads.begin(), heads.end() - 1);
        for (std::size_t i = 0; i < words.size(); i++) {
          order[fill[words[i]]++] = i;
        }
        for (std::size_t b = 0; b < leaves; b++) {
          tree[leaves + b] = heads[b] < ends[b] ? order[heads[b]] : none;
        }
        for (std::size_t n = leaves - 1; n > 0; n--) {
          tree[n] = std::min(tree[2 * n], tree[2 * n + 1]);
        }
      }

      /**
       * @brief Find the earliest remaining record of at most a size.
       *
       * @param max_words Largest size in words.
       * @return Its index, or none.
       */
      std::size_t earliest_fitting(std::size_t max_words) const {
        if (max_words == 0) {
          return none;
        }
        std::size_t best = none;
        std::size_t lo = leaves;
        std::size_t hi = leaves + std::min(max_words, leaves - 1) + 1;
        while (lo < hi) {
          if (lo & 1) {
            best = std::min(best, tree[lo++]);
          }
          if (hi & 1) {
            best = std::min(best, tree[--hi]);
          }
          lo >>= 1;
          hi >>= 1;
        }
        return best;
      }

      /**
       * @brief Find the earliest remaining record of exactly a size.
       *
       * @param words Size in words.
       * @return Its index, or none.
       */
      std::size_t earliest_exact(std::size_t words) const {
        return words < leaves ? tree[leaves + words] : none;
      }

      /**
       * @brief Remove the earliest remaining record of a size.
       *
       * @param words Size in words.
       */
      void remove(std::uint32_t words) {
        const auto head = ++heads[words];
        std::size_t n = leaves + words;
        tree[n] = head < ends[words] ? order[head] : none;
        for (n >>= 1; n > 0; n >>= 1) {
          tree[n] = std::min(tree[2 * n], tree[2 * n + 1]);
        }
      }

    private:
      static constexpr std::size_t leaves = 4096;  ///< Record sizes in words: 12 bits.

      std::vector<std::size_t> heads;  ///< Per size, where its earliest remaining record is in order.
      std::vector<std::size_t> ends;  ///< Per size, where its records end in order.
      std::vector<std::size_t> order;  ///< Records grouped by size, in frame order.
      std::vector<std::size_t> tree;  ///< Earliest remaining record per range of sizes.
    };
  }  // namespace detail

  /**
   * @brief What laying out a frame did.
   */
  struct layout_t {
    std::size_t critical_bytes = 0;  ///< Frame bytes through the last record of the coarsest level.
    std::size_t padding_bytes = 0;  ///< Bytes of padding records added.
  };

  /**
   * @brief Lay out an encoded frame in record framing.
   *
   * @param bitstream The frame as PyroWave encoded it: the sequence header, then every block.
   * @param payload Frame bytes per packet, from packet_payload_bytes(). Not 0.
   * @param out Receives the laid out frame, reusing its storage.
   * @param layout Receives what the layout did.
   * @return False if bitstream isn't a PyroWave frame, leaving out unspecified.
   */
  inline bool write_record_frame(std::span<const std::uint8_t> bitstream, std::size_t payload, std::vector<std::uint8_t> &out, layout_t &layout) {
    using detail::read_u32;

    struct record_t {
      std::size_t offset;
      std::size_t size;
      std::uint32_t block_index;
    };

    if (bitstream.size() < header_bytes || (read_u32(bitstream.data()) >> 31) == 0) {
      return false;
    }
    std::vector<record_t> records;
    for (std::size_t position = header_bytes; position < bitstream.size();) {
      if (bitstream.size() - position < header_bytes) {
        return false;
      }
      const auto word0 = read_u32(bitstream.data() + position);
      const std::size_t size = std::size_t((word0 >> 16) & 0xfff) * 4;
      if ((word0 >> 31) != 0 || size < header_bytes || size > bitstream.size() - position) {
        return false;
      }
      records.push_back({position, size, read_u32(bitstream.data() + position + 4) >> 8});
      position += size;
    }

    layout = {};
    out.clear();
    out.reserve(bitstream.size() + bitstream.size() / 64);
    out.insert(out.end(), bitstream.begin(), bitstream.begin() + header_bytes);

    // Bytes left in the packet the next byte goes in (never 0)
    const auto room = [&]() {
      return payload - (out.size() + frame_header_bytes) % payload;
    };
    const auto place = [&](const record_t &record) {
      out.insert(out.end(), bitstream.begin() + record.offset, bitstream.begin() + record.offset + record.size);
    };
    const auto pad = [&](std::size_t bytes) {
      detail::append_u32(out, padding_magic);
      detail::append_u32(out, std::uint32_t(bytes / 4 - 2));
      out.resize(out.size() + bytes - header_bytes, 0);
      layout.padding_bytes += bytes;
    };
    // Too large to share a packet with a padding record. One a packet less 4 bytes long would
    // always leave 4 bytes, too few for a padding record, so it counts too.
    const auto oversized = [&](const record_t &record) {
      return record.size + header_bytes > payload;
    };

    const auto sequence = read_u32(bitstream.data());
    const auto coarse_blocks = coarse_block_count((sequence & 0x3fff) + 1, ((sequence >> 14) & 0x3fff) + 1);
    std::vector<std::size_t> coarse;
    std::vector<std::size_t> fine;
    for (std::size_t i = 0; i < records.size(); i++) {
      (records[i].block_index < coarse_blocks ? coarse : fine).push_back(i);
    }

    for (const auto *group : {&coarse, &fine}) {
      // Oversized records first, wherever they start. One that would end 4 bytes before a
      // packet boundary is moved along by an empty padding record, as nothing could fill them.
      std::vector<std::size_t> ordinary;
      for (const auto i : *group) {
        if (oversized(records[i])) {
          if (records[i].size % payload == room() - 4) {
            pad(header_bytes);
          }
          place(records[i]);
        } else {
          ordinary.push_back(i);
        }
      }

      // Then the rest, first-fit: what's left of a packet takes the earliest record that fits
      std::vector<std::uint32_t> words;
      words.reserve(ordinary.size());
      for (const auto i : ordinary) {
        words.push_back(std::uint32_t(records[i].size / 4));
      }
      detail::fit_index_t fit {words};
      for (std::size_t left = ordinary.size(); left != 0;) {
        const auto bytes = room();
        auto chosen = fit.earliest_fitting(bytes / 4);
        if (chosen != detail::fit_index_t::none && bytes - std::size_t(words[chosen]) * 4 == 4) {
          // That would leave 4 bytes. Take one that fills the packet, or leaves room for padding.
          const auto exact = fit.earliest_exact(bytes / 4);
          const auto roomy = bytes >= 16 ? fit.earliest_fitting((bytes - header_bytes) / 4) : detail::fit_index_t::none;
          chosen = std::min(exact, roomy);
        }
        if (chosen == detail::fit_index_t::none) {
          // Never 4 bytes: nothing above leaves that
          pad(bytes);
          continue;
        }
        fit.remove(words[chosen]);
        --left;
        place(records[ordinary[chosen]]);
      }

      if (group == &coarse) {
        layout.critical_bytes = out.size();
      }
    }
    return true;
  }

  constexpr std::size_t max_fec_blocks = 4;  ///< FEC blocks a frame can be sent in (2 bits in the video packet header).
  constexpr std::size_t max_block_packets = 1023;  ///< Data packets a FEC block can have (10 bits in the video packet header).
  constexpr std::size_t max_reed_solomon_shards = 255;  ///< Data and parity packets a FEC block with parity can have.

  /**
   * @brief One FEC block of a frame.
   */
  struct fec_block_t {
    std::size_t packets;  ///< Data packets.
    std::size_t fec_percentage;  ///< Parity, as a percentage of them.
  };

  /**
   * @brief Plan a frame's FEC blocks so only its critical packets get parity.
   *
   * @details Losing a critical packet costs the whole frame, while losing a later one only blurs
   *          part of it for that frame, so the session's FEC goes to the critical packets alone:
   *          they are the first block, with parity at the session's percentage and at least the
   *          client's minimum of parity packets. The rest of the frame follows in up to three blocks
   *          without parity, each kept to a Reed-Solomon block's size where it can be, so a client
   *          that lost packets of one can give up on it without waiting for a huge one.
   *
   * @param packets Data packets of the frame.
   * @param critical_packets Its critical packets, from the start.
   * @param fec_percentage The session's FEC percentage.
   * @param min_parity_packets The fewest parity packets the client wants in a block with parity.
   * @return The blocks in order, or empty if the frame should be sent with FEC as for other
   *         codecs: FEC is off, nothing is critical, the critical packets and their parity don't fit
   *         one Reed-Solomon block, or the rest of the frame doesn't fit three blocks.
   */
  inline std::vector<fec_block_t> critical_fec_blocks(std::size_t packets, std::size_t critical_packets, std::size_t fec_percentage, std::size_t min_parity_packets) {
    if (critical_packets == 0 || fec_percentage == 0 || critical_packets > packets) {
      return {};
    }
    // As stream.cpp's FEC encoder works it out
    const auto parity = std::max((critical_packets * fec_percentage + 99) / 100, min_parity_packets);
    if (critical_packets + parity > max_reed_solomon_shards) {
      return {};
    }

    std::vector<fec_block_t> blocks {{critical_packets, fec_percentage}};
    const auto rest = packets - critical_packets;
    if (rest == 0) {
      return blocks;
    }
    const auto count = std::clamp<std::size_t>((rest + max_reed_solomon_shards - 1) / max_reed_solomon_shards, 1, max_fec_blocks - 1);
    const auto per_block = (rest + count - 1) / count;
    if (per_block > max_block_packets) {
      return {};
    }
    for (auto left = rest; left != 0;) {
      const auto block = std::min(per_block, left);
      blocks.push_back({block, 0});
      left -= block;
    }
    return blocks;
  }

  /**
   * @brief Find which packets of a frame in record framing start with a record.
   *
   * @param frame The frame, as write_record_frame() laid it out.
   * @param payload Frame bytes per packet.
   * @return One entry per packet, or empty if the frame can't be walked. The first packet always
   *         starts with one, the sequence header.
   */
  inline std::vector<bool> record_start_packets(std::span<const std::uint8_t> frame, std::size_t payload) {
    if (payload == 0 || frame.empty()) {
      return {};
    }
    std::vector<bool> starts(packets_for(frame.size(), payload), false);
    for (std::size_t position = 0; position < frame.size();) {
      if (frame.size() - position < header_bytes) {
        return {};
      }
      if (position == 0 || (position + frame_header_bytes) % payload == 0) {
        starts[(position + frame_header_bytes) / payload] = true;
      }
      const auto word0 = detail::read_u32(frame.data() + position);
      std::size_t size;
      if (word0 == padding_magic) {
        const auto words = detail::read_u32(frame.data() + position + 4);
        if (words > (frame.size() - position - header_bytes) / 4) {
          return {};
        }
        size = header_bytes + std::size_t(words) * 4;
      } else if ((word0 >> 31) != 0) {
        size = header_bytes;
      } else {
        size = std::size_t((word0 >> 16) & 0xfff) * 4;
        if (size < header_bytes || size > frame.size() - position) {
          return {};
        }
      }
      position += size;
    }
    return starts;
  }
}  // namespace pyrowave::records
