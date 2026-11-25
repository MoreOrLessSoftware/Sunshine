/**
 * @file tests/unit/platform/windows/test_display_base.cpp
 * @brief Test the encoder reenumeration check in src/platform/windows/display_base.cpp.
 */

// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <optional>
  #include <vector>

  // local includes
  #include <src/platform/common.h>
  #include <src/platform/windows/display.h>

namespace {
  using platf::dxgi::adapter_identity_t;

  /**
   * @brief Build an adapter identity for a GPU that drives the desktop.
   *
   * @param luid_low Low part of the adapter LUID.
   * @param driver_version User-mode driver version.
   * @return The adapter identity.
   */
  adapter_identity_t gpu(DWORD luid_low, LONGLONG driver_version) {
    return {
      .luid_high = 0,
      .luid_low = luid_low,
      .vendor_id = 0x10DE,
      .device_id = 0x2684,
      .sub_sys_id = 0x1,
      .revision = 0xA1,
      .driver_version = driver_version,
      .drives_desktop = true,
    };
  }
}  // namespace

TEST(WindowsEncoderReenumerationTest, ProbesWhenNoAdaptersWereRecorded) {
  EXPECT_TRUE(platf::dxgi::adapters_need_reenumeration(std::nullopt, {gpu(1, 100)}));
}

TEST(WindowsEncoderReenumerationTest, KeepsTheProbeWhenTheAdaptersAreUnchanged) {
  const std::vector adapters {gpu(1, 100)};
  EXPECT_FALSE(platf::dxgi::adapters_need_reenumeration(adapters, adapters));
}

TEST(WindowsEncoderReenumerationTest, ProbesWhenNoAdapterIsFound) {
  EXPECT_TRUE(platf::dxgi::adapters_need_reenumeration(std::vector<adapter_identity_t> {}, {}));
}

TEST(WindowsEncoderReenumerationTest, ProbesWhenTheDriverRestarts) {
  EXPECT_TRUE(platf::dxgi::adapters_need_reenumeration(std::vector {gpu(1, 100)}, {gpu(2, 100)}));
}

TEST(WindowsEncoderReenumerationTest, ProbesWhenTheDriverIsUpdated) {
  EXPECT_TRUE(platf::dxgi::adapters_need_reenumeration(std::vector {gpu(1, 100)}, {gpu(1, 101)}));
}

TEST(WindowsEncoderReenumerationTest, ProbesWhenAnAdapterAppears) {
  EXPECT_TRUE(platf::dxgi::adapters_need_reenumeration(std::vector {gpu(1, 100)}, {gpu(1, 100), gpu(2, 200)}));
}

TEST(WindowsEncoderReenumerationTest, ProbesWhenTheDesktopMovesToAnotherAdapter) {
  auto before = std::vector {gpu(1, 100), gpu(2, 200)};
  before[1].drives_desktop = false;
  auto after = before;
  after[0].drives_desktop = false;
  after[1].drives_desktop = true;
  EXPECT_TRUE(platf::dxgi::adapters_need_reenumeration(before, after));
}

TEST(WindowsEncoderReenumerationTest, NoAdaptersWithoutAFactory) {
  EXPECT_TRUE(platf::dxgi::adapter_identities(nullptr).empty());
}

TEST(WindowsEncoderReenumerationTest, AdapterIdentitiesAreStable) {
  platf::dxgi::factory1_t factory;
  ASSERT_TRUE(SUCCEEDED(CreateDXGIFactory1(IID_IDXGIFactory1, (void **) &factory)));

  const auto first = platf::dxgi::adapter_identities(factory.get());
  ASSERT_FALSE(first.empty());  // DXGI always lists at least the Microsoft Basic Render Driver
  EXPECT_EQ(first, platf::dxgi::adapter_identities(factory.get()));
}

TEST(WindowsEncoderReenumerationTest, NoReprobeWithoutAChange) {
  // The first call may or may not ask for a probe, depending on what ran before; the next one has nothing new to see.
  platf::needs_encoder_reenumeration();
  EXPECT_FALSE(platf::needs_encoder_reenumeration());
}
#endif
