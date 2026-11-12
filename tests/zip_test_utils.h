/**
 * @file tests/zip_test_utils.h
 * @brief Builds zip files for tests of plugins imported from a URL.
 */
#pragma once

// standard includes
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

// lib includes
#include <zlib.h>

// local includes
#include <src/crypto.h>

namespace test_utils::zip {
  /**
   * @brief An entry of a zip file built by `make_zip()`.
   */
  struct zip_entry_t {
    std::string name;  ///< Name stored in the zip.
    std::string data;  ///< Contents.
    bool deflate = true;  ///< Whether to compress it rather than store it.
    std::uint16_t made_by = 20;  ///< "Version made by", whose high byte is the host system.
    std::uint32_t attributes = 0;  ///< External attributes.
    std::uint16_t flags = 0;  ///< General purpose flags.
    std::optional<std::uint16_t> method;  ///< Compression method to record instead of the real one.
    std::optional<std::uint32_t> size;  ///< Uncompressed size to record instead of the real one.
    std::optional<std::uint32_t> crc;  ///< CRC to record instead of the real one.
  };

  /**
   * @brief Build a zip file.
   *
   * @param entries Entries to put in it.
   * @param count Number of entries to record instead of the real one.
   * @return Contents of the zip file.
   */
  inline std::string make_zip(const std::vector<zip_entry_t> &entries, std::optional<std::uint16_t> count = std::nullopt) {
    const auto le16 = [](std::string &out, std::uint32_t value) {
      out += static_cast<char>(value & 0xFF);
      out += static_cast<char>((value >> 8) & 0xFF);
    };
    const auto le32 = [&le16](std::string &out, std::uint32_t value) {
      le16(out, value & 0xFFFF);
      le16(out, value >> 16);
    };

    std::string zip;
    std::string directory;
    for (const auto &entry : entries) {
      std::string stored = entry.data;
      if (entry.deflate) {
        z_stream stream {};
        deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        stored.assign(deflateBound(&stream, static_cast<uLong>(entry.data.size())), '\0');
        stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(entry.data.data()));
        stream.avail_in = static_cast<uInt>(entry.data.size());
        stream.next_out = reinterpret_cast<Bytef *>(stored.data());
        stream.avail_out = static_cast<uInt>(stored.size());
        deflate(&stream, Z_FINISH);
        stored.resize(stream.total_out);
        deflateEnd(&stream);
      }
      const auto method = entry.method.value_or(entry.deflate ? Z_DEFLATED : 0);
      const auto crc = entry.crc.value_or(static_cast<std::uint32_t>(crc32(0, reinterpret_cast<const Bytef *>(entry.data.data()), static_cast<uInt>(entry.data.size()))));
      const auto size = entry.size.value_or(static_cast<std::uint32_t>(entry.data.size()));
      const auto offset = static_cast<std::uint32_t>(zip.size());

      le32(zip, 0x04034b50);
      le16(zip, 20);
      le16(zip, entry.flags);
      le16(zip, method);
      le32(zip, 0);  // time and date
      le32(zip, crc);
      le32(zip, static_cast<std::uint32_t>(stored.size()));
      le32(zip, size);
      le16(zip, static_cast<std::uint32_t>(entry.name.size()));
      le16(zip, 0);
      zip += entry.name;
      zip += stored;

      le32(directory, 0x02014b50);
      le16(directory, entry.made_by);
      le16(directory, 20);
      le16(directory, entry.flags);
      le16(directory, method);
      le32(directory, 0);
      le32(directory, crc);
      le32(directory, static_cast<std::uint32_t>(stored.size()));
      le32(directory, size);
      le16(directory, static_cast<std::uint32_t>(entry.name.size()));
      le16(directory, 0);  // extra field
      le16(directory, 0);  // comment
      le16(directory, 0);  // disk
      le16(directory, 0);  // internal attributes
      le32(directory, entry.attributes);
      le32(directory, offset);
      directory += entry.name;
    }

    const auto directory_offset = static_cast<std::uint32_t>(zip.size());
    zip += directory;
    le32(zip, 0x06054b50);
    le16(zip, 0);
    le16(zip, 0);
    le16(zip, count.value_or(static_cast<std::uint16_t>(entries.size())));
    le16(zip, count.value_or(static_cast<std::uint16_t>(entries.size())));
    le32(zip, static_cast<std::uint32_t>(directory.size()));
    le32(zip, directory_offset);
    le16(zip, 7);  // comment length
    zip += "comment";
    return zip;
  }

  /**
   * @brief Format the SHA-256 of some data as hex.
   *
   * @param data Data to hash.
   * @return 64 lowercase hex digits.
   */
  inline std::string sha256_of(const std::string &data) {
    std::string hex;
    for (const auto byte : crypto::hash(data)) {
      hex += std::format("{:02x}", byte);
    }
    return hex;
  }
}  // namespace test_utils::zip
