/**
 * @file tests/unit/test_nvhttp_codec_modes.cpp
 * @brief Test the codec mode flags advertised to clients in /serverinfo.
 */

#include "../tests_common.h"

// standard includes
#include <array>
#include <cstdint>
#include <tuple>

// local includes
#include <src/nvhttp.h>
#include <src/video.h>

namespace {
  constexpr uint32_t scm_pyrowave = 0x00800000;  ///< PyroWave 4:2:0 8-bit.
  constexpr uint32_t scm_pyrowave_444 = 0x01000000;  ///< PyroWave 4:4:4 8-bit.
  constexpr uint32_t scm_pyrowave_10bit = 0x02000000;  ///< PyroWave 4:2:0 10-bit.
  constexpr uint32_t scm_pyrowave_10bit_444 = 0x04000000;  ///< PyroWave 4:4:4 10-bit.
  constexpr uint32_t scm_mask_pyrowave = scm_pyrowave | scm_pyrowave_444 | scm_pyrowave_10bit | scm_pyrowave_10bit_444;  ///< All PyroWave bits.
}  // namespace

/**
 * @brief Restores the probed encoder modes that the codec mode flags are built from.
 */
class PyrowaveCodecModeTest: public testing::TestWithParam<std::tuple<int, int, uint32_t>> {
protected:
  /**
   * @brief Save the probed modes and turn off every codec but PyroWave.
   */
  void SetUp() override {
    saved_hevc_mode = video::active_hevc_mode;
    saved_av1_mode = video::active_av1_mode;
    saved_pyrowave_mode = video::active_pyrowave_mode;
    saved_pyrowave_yuv444_mode = video::active_pyrowave_yuv444_mode;
    saved_yuv444 = video::last_encoder_probe_supported_yuv444_for_codec;

    // Only PyroWave is under test
    video::active_hevc_mode = 1;
    video::active_av1_mode = 1;
    video::last_encoder_probe_supported_yuv444_for_codec = {};
  }

  /**
   * @brief Restore the probed modes.
   */
  void TearDown() override {
    video::active_hevc_mode = saved_hevc_mode;
    video::active_av1_mode = saved_av1_mode;
    video::active_pyrowave_mode = saved_pyrowave_mode;
    video::active_pyrowave_yuv444_mode = saved_pyrowave_yuv444_mode;
    video::last_encoder_probe_supported_yuv444_for_codec = saved_yuv444;
  }

private:
  int saved_hevc_mode = 0;  ///< Saved HEVC mode.
  int saved_av1_mode = 0;  ///< Saved AV1 mode.
  int saved_pyrowave_mode = 0;  ///< Saved PyroWave mode.
  int saved_pyrowave_yuv444_mode = 0;  ///< Saved PyroWave 4:4:4 mode.
  std::array<bool, 3> saved_yuv444 {};  ///< Saved per-codec YUV444 probe results.
};

TEST_P(PyrowaveCodecModeTest, AdvertisesTheProbedModes) {
  const auto &[pyrowave_mode, pyrowave_yuv444_mode, expected_flags] = GetParam();
  video::active_pyrowave_mode = pyrowave_mode;
  video::active_pyrowave_yuv444_mode = pyrowave_yuv444_mode;

  EXPECT_EQ(nvhttp::test_support::codec_mode_flags() & scm_mask_pyrowave, expected_flags);
}

INSTANTIATE_TEST_SUITE_P(
  PyrowaveCodecModes,
  PyrowaveCodecModeTest,
  testing::Values(
    std::make_tuple(1, 1, 0u),
    std::make_tuple(2, 1, scm_pyrowave),
    std::make_tuple(3, 1, scm_pyrowave | scm_pyrowave_10bit),
    std::make_tuple(2, 2, scm_pyrowave | scm_pyrowave_444),
    std::make_tuple(3, 2, scm_pyrowave | scm_pyrowave_10bit | scm_pyrowave_444),
    std::make_tuple(3, 3, scm_mask_pyrowave)
  )
);
