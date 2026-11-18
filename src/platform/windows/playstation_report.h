/**
 * @file src/platform/windows/playstation_report.h
 * @brief Conversions shared by the virtual PlayStation controllers (ViGEmBus DualShock 4, xgpad DualSense).
 *
 * Kept free of Windows APIs so the conversions can be unit tested on their own.
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>

namespace platf::playstation {

  constexpr float earth_g = 9.80665f;  ///< Meters per second squared in one g.
  constexpr int accel_counts_per_g = 8192;  ///< Accelerometer counts per g, uncalibrated, on DualShock 4 and DualSense.
  constexpr int gyro_counts_per_deg_s = 16;  ///< Gyro counts per degree per second, uncalibrated, on DualShock 4 and DualSense.
  constexpr std::uint8_t max_touch_points = 2;  ///< Fingers a DualShock 4 or DualSense touchpad tracks.

  /**
   * @brief One motion sample in sensor counts, before clamping.
   */
  struct motion_counts_t {
    std::int32_t x;  ///< X axis.
    std::int32_t y;  ///< Y axis.
    std::int32_t z;  ///< Z axis.

    /**
     * @brief Compare two samples.
     * @param other Sample to compare with.
     * @return True when all three axes match.
     */
    bool operator==(const motion_counts_t &other) const = default;
  };

  /**
   * @brief Convert an acceleration from Moonlight's m/s^2 to accelerometer counts.
   * @param x X acceleration in m/s^2.
   * @param y Y acceleration in m/s^2.
   * @param z Z acceleration in m/s^2.
   * @return Counts at accel_counts_per_g, truncated towards zero and not clamped.
   */
  constexpr motion_counts_t accel_counts(const float x, const float y, const float z) {
    const auto convert = [](const float value) {
      return static_cast<std::int32_t>(value / earth_g * accel_counts_per_g);
    };
    return {convert(x), convert(y), convert(z)};
  }

  /**
   * @brief Convert an angular velocity from Moonlight's deg/s to gyro counts.
   * @param x Rotation about X in deg/s.
   * @param y Rotation about Y in deg/s.
   * @param z Rotation about Z in deg/s.
   * @return Counts at gyro_counts_per_deg_s, truncated towards zero and not clamped.
   */
  constexpr motion_counts_t gyro_counts(const float x, const float y, const float z) {
    const auto convert = [](const float value) {
      return static_cast<std::int32_t>(value * gyro_counts_per_deg_s);
    };
    return {convert(x), convert(y), convert(z)};
  }

  /**
   * @brief Clamp a sensor value to the signed 16-bit range of a report field.
   * @param value Sensor counts.
   * @return Value limited to -32768..32767.
   */
  constexpr std::int16_t clamp_i16(const std::int32_t value) {
    return static_cast<std::int16_t>(std::clamp<std::int32_t>(value, INT16_MIN, INT16_MAX));
  }

  /**
   * @brief Convert a normalized touch coordinate to touchpad units.
   * @param value Coordinate from 0.0 to 1.0; values outside are clamped.
   * @param extent Touchpad width or height in its own units.
   * @return Coordinate from 0 to extent - 1.
   */
  constexpr std::uint16_t touch_coordinate(const float value, const std::uint16_t extent) {
    if (!(value > 0.0f)) {
      return 0;  // also catches NaN
    }
    const auto scaled = static_cast<std::uint32_t>(value * extent);
    return static_cast<std::uint16_t>(std::min<std::uint32_t>(scaled, extent - 1u));
  }

  /**
   * @brief Pack a touch position into the three bytes DualShock 4 and DualSense reports use.
   * @param x X in touchpad units (12 bits).
   * @param y Y in touchpad units (12 bits).
   * @return X bits 0..7; X bits 8..11 and Y bits 0..3; Y bits 4..11.
   */
  constexpr std::array<std::uint8_t, 3> pack_touch(const std::uint16_t x, const std::uint16_t y) {
    return {
      static_cast<std::uint8_t>(x & 0xFF),
      static_cast<std::uint8_t>(((x >> 8) & 0x0F) | ((y & 0x0F) << 4)),
      static_cast<std::uint8_t>((y >> 4) & 0xFF),
    };
  }

  /**
   * @brief Assigns Moonlight's touch pointer IDs to the touchpad's two touch point slots.
   */
  class touch_slots_t {
  public:
    /**
     * @brief Assign a free slot to a new touch.
     *
     * A pointer that already has a slot keeps it.
     *
     * @param pointer_id Moonlight pointer ID.
     * @return Slot index, or empty when both slots are taken.
     */
    std::optional<std::uint8_t> down(const std::uint32_t pointer_id) {
      if (const auto slot = find(pointer_id)) {
        return slot;
      }
      for (std::uint8_t slot = 0; slot < max_touch_points; ++slot) {
        if (available & (1u << slot)) {
          available &= static_cast<std::uint8_t>(~(1u << slot));
          slots[pointer_id] = slot;
          return slot;
        }
      }
      return std::nullopt;
    }

    /**
     * @brief Look up the slot of a touch.
     * @param pointer_id Moonlight pointer ID.
     * @return Slot index, or empty when the pointer has no slot.
     */
    std::optional<std::uint8_t> find(const std::uint32_t pointer_id) const {
      const auto it = slots.find(pointer_id);
      if (it == slots.end()) {
        return std::nullopt;
      }
      return it->second;
    }

    /**
     * @brief Free the slot of a touch that ended.
     * @param pointer_id Moonlight pointer ID.
     * @return Slot index that was freed, or empty when the pointer had no slot.
     */
    std::optional<std::uint8_t> up(const std::uint32_t pointer_id) {
      const auto it = slots.find(pointer_id);
      if (it == slots.end()) {
        return std::nullopt;
      }
      const auto slot = it->second;
      slots.erase(it);
      available |= static_cast<std::uint8_t>(1u << slot);
      return slot;
    }

    /**
     * @brief Free both slots.
     */
    void clear() {
      slots.clear();
      available = all_slots;
    }

  private:
    static constexpr std::uint8_t all_slots = (1u << max_touch_points) - 1;  ///< Bit mask with every slot free.

    std::map<std::uint32_t, std::uint8_t> slots;  ///< Slot of each active pointer ID.
    std::uint8_t available = all_slots;  ///< Bit mask of free slots.
  };

}  // namespace platf::playstation
