/*
 *  Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <memory>

#include "webrtc/base/checks.h"
#include "webrtc/common_types.h"
#include "webrtc/modules/audio_coding/audio_network_adaptor/mock/mock_audio_network_adaptor.h"
#include "webrtc/modules/audio_coding/codecs/opus/audio_encoder_opus.h"
#include "webrtc/test/field_trial.h"
#include "webrtc/test/gmock.h"
#include "webrtc/test/gtest.h"
#include "webrtc/system_wrappers/include/clock.h"

namespace webrtc {
using ::testing::NiceMock;
using ::testing::Return;

namespace {

const CodecInst kDefaultOpusSettings = {105, "opus", 48000, 960, 1, 32000};
constexpr int64_t kInitialTimeUs = 12345678;

AudioEncoderOpus::Config CreateConfig(const CodecInst& codec_inst) {
  AudioEncoderOpus::Config config;
  config.frame_size_ms = rtc::CheckedDivExact(codec_inst.pacsize, 48);
  config.num_channels = codec_inst.channels;
  config.bitrate_bps = rtc::Optional<int>(codec_inst.rate);
  config.payload_type = codec_inst.pltype;
  config.application = config.num_channels == 1 ? AudioEncoderOpus::kVoip
                                                : AudioEncoderOpus::kAudio;
  config.supported_frame_lengths_ms.push_back(config.frame_size_ms);
  return config;
}

struct AudioEncoderOpusStates {
  std::shared_ptr<MockAudioNetworkAdaptor*> mock_audio_network_adaptor;
  std::unique_ptr<AudioEncoderOpus> encoder;
  std::unique_ptr<SimulatedClock> simulated_clock;
};

AudioEncoderOpusStates CreateCodec(size_t num_channels) {
  AudioEncoderOpusStates states;
  states.mock_audio_network_adaptor =
      std::make_shared<MockAudioNetworkAdaptor*>(nullptr);

  std::weak_ptr<MockAudioNetworkAdaptor*> mock_ptr(
      states.mock_audio_network_adaptor);
  AudioEncoderOpus::AudioNetworkAdaptorCreator creator = [mock_ptr](
      const std::string&, const Clock*) {
    std::unique_ptr<MockAudioNetworkAdaptor> adaptor(
        new NiceMock<MockAudioNetworkAdaptor>());
    EXPECT_CALL(*adaptor, Die());
    if (auto sp = mock_ptr.lock()) {
      *sp = adaptor.get();
    } else {
      RTC_NOTREACHED();
    }
    return adaptor;
  };

  CodecInst codec_inst = kDefaultOpusSettings;
  codec_inst.channels = num_channels;
  auto config = CreateConfig(codec_inst);
  states.simulated_clock.reset(new SimulatedClock(kInitialTimeUs));
  config.clock = states.simulated_clock.get();

  states.encoder.reset(new AudioEncoderOpus(config, std::move(creator)));
  return states;
}

AudioNetworkAdaptor::EncoderRuntimeConfig CreateEncoderRuntimeConfig() {
  constexpr int kBitrate = 40000;
  constexpr int kFrameLength = 60;
  constexpr bool kEnableFec = true;
  constexpr bool kEnableDtx = false;
  constexpr size_t kNumChannels = 1;
  constexpr float kPacketLossFraction = 0.1f;
  AudioNetworkAdaptor::EncoderRuntimeConfig config;
  config.bitrate_bps = rtc::Optional<int>(kBitrate);
  config.frame_length_ms = rtc::Optional<int>(kFrameLength);
  config.enable_fec = rtc::Optional<bool>(kEnableFec);
  config.enable_dtx = rtc::Optional<bool>(kEnableDtx);
  config.num_channels = rtc::Optional<size_t>(kNumChannels);
  config.uplink_packet_loss_fraction =
      rtc::Optional<float>(kPacketLossFraction);
  return config;
}

void CheckEncoderRuntimeConfig(
    const AudioEncoderOpus* encoder,
    const AudioNetworkAdaptor::EncoderRuntimeConfig& config) {
  EXPECT_EQ(*config.bitrate_bps, encoder->GetTargetBitrate());
  EXPECT_EQ(*config.frame_length_ms, encoder->next_frame_length_ms());
  EXPECT_EQ(*config.enable_fec, encoder->fec_enabled());
  EXPECT_EQ(*config.enable_dtx, encoder->GetDtx());
  EXPECT_EQ(*config.num_channels, encoder->num_channels_to_encode());
}

}  // namespace

TEST(AudioEncoderOpusTest, DefaultApplicationModeMono) {
  auto states = CreateCodec(1);
  EXPECT_EQ(AudioEncoderOpus::kVoip, states.encoder->application());
}

TEST(AudioEncoderOpusTest, DefaultApplicationModeStereo) {
  auto states = CreateCodec(2);
  EXPECT_EQ(AudioEncoderOpus::kAudio, states.encoder->application());
}

TEST(AudioEncoderOpusTest, ChangeApplicationMode) {
  auto states = CreateCodec(2);
  EXPECT_TRUE(
      states.encoder->SetApplication(AudioEncoder::Application::kSpeech));
  EXPECT_EQ(AudioEncoderOpus::kVoip, states.encoder->application());
}

TEST(AudioEncoderOpusTest, ResetWontChangeApplicationMode) {
  auto states = CreateCodec(2);

  // Trigger a reset.
  states.encoder->Reset();
  // Verify that the mode is still kAudio.
  EXPECT_EQ(AudioEncoderOpus::kAudio, states.encoder->application());

  // Now change to kVoip.
  EXPECT_TRUE(
      states.encoder->SetApplication(AudioEncoder::Application::kSpeech));
  EXPECT_EQ(AudioEncoderOpus::kVoip, states.encoder->application());

  // Trigger a reset again.
  states.encoder->Reset();
  // Verify that the mode is still kVoip.
  EXPECT_EQ(AudioEncoderOpus::kVoip, states.encoder->application());
}

TEST(AudioEncoderOpusTest, ToggleDtx) {
  auto states = CreateCodec(2);
  // Enable DTX
  EXPECT_TRUE(states.encoder->SetDtx(true));
  // Verify that the mode is still kAudio.
  EXPECT_EQ(AudioEncoderOpus::kAudio, states.encoder->application());
  // Turn off DTX.
  EXPECT_TRUE(states.encoder->SetDtx(false));
}

TEST(AudioEncoderOpusTest,
     OnReceivedTargetAudioBitrateWithoutAudioNetworkAdaptor) {
  auto states = CreateCodec(1);
  // Constants are replicated from audio_states.encoderopus.cc.
  const int kMinBitrateBps = 500;
  const int kMaxBitrateBps = 512000;
  // Set a too low bitrate.
  states.encoder->OnReceivedTargetAudioBitrate(kMinBitrateBps - 1);
  EXPECT_EQ(kMinBitrateBps, states.encoder->GetTargetBitrate());
  // Set a too high bitrate.
  states.encoder->OnReceivedTargetAudioBitrate(kMaxBitrateBps + 1);
  EXPECT_EQ(kMaxBitrateBps, states.encoder->GetTargetBitrate());
  // Set the minimum rate.
  states.encoder->OnReceivedTargetAudioBitrate(kMinBitrateBps);
  EXPECT_EQ(kMinBitrateBps, states.encoder->GetTargetBitrate());
  // Set the maximum rate.
  states.encoder->OnReceivedTargetAudioBitrate(kMaxBitrateBps);
  EXPECT_EQ(kMaxBitrateBps, states.encoder->GetTargetBitrate());
  // Set rates from 1000 up to 32000 bps.
  for (int rate = 1000; rate <= 32000; rate += 1000) {
    states.encoder->OnReceivedTargetAudioBitrate(rate);
    EXPECT_EQ(rate, states.encoder->GetTargetBitrate());
  }
}

namespace {

// Returns a vector with the n evenly-spaced numbers a, a + (b - a)/(n - 1),
// ..., b.
std::vector<float> IntervalSteps(float a, float b, size_t n) {
  RTC_DCHECK_GT(n, 1u);
  const float step = (b - a) / (n - 1);
  std::vector<float> points;
  points.push_back(a);
  for (size_t i = 1; i < n - 1; ++i)
    points.push_back(a + i * step);
  points.push_back(b);
  return points;
}

// Sets the packet loss rate to each number in the vector in turn, and verifies
// that the loss rate as reported by the encoder is |expected_return| for all
// of them.
void TestSetPacketLossRate(AudioEncoderOpusStates* states,
                           const std::vector<float>& losses,
                           float expected_return) {
  // |kSampleIntervalMs| is chosen to ease the calculation since
  // 0.9999 ^ 184198 = 1e-8. Which minimizes the effect of
  // PacketLossFractionSmoother used in AudioEncoderOpus.
  constexpr int64_t kSampleIntervalMs = 184198;
  for (float loss : losses) {
    states->encoder->OnReceivedUplinkPacketLossFraction(loss);
    states->simulated_clock->AdvanceTimeMilliseconds(kSampleIntervalMs);
    EXPECT_FLOAT_EQ(expected_return, states->encoder->packet_loss_rate());
  }
}

}  // namespace

TEST(AudioEncoderOpusTest, PacketLossRateOptimized) {
  auto states = CreateCodec(1);
  auto I = [](float a, float b) { return IntervalSteps(a, b, 10); };
  constexpr float eps = 1e-8f;

  // Note that the order of the following calls is critical.

  // clang-format off
  TestSetPacketLossRate(&states, I(0.00f      , 0.01f - eps), 0.00f);
  TestSetPacketLossRate(&states, I(0.01f + eps, 0.06f - eps), 0.01f);
  TestSetPacketLossRate(&states, I(0.06f + eps, 0.11f - eps), 0.05f);
  TestSetPacketLossRate(&states, I(0.11f + eps, 0.22f - eps), 0.10f);
  TestSetPacketLossRate(&states, I(0.22f + eps, 1.00f      ), 0.20f);

  TestSetPacketLossRate(&states, I(1.00f      , 0.18f + eps), 0.20f);
  TestSetPacketLossRate(&states, I(0.18f - eps, 0.09f + eps), 0.10f);
  TestSetPacketLossRate(&states, I(0.09f - eps, 0.04f + eps), 0.05f);
  TestSetPacketLossRate(&states, I(0.04f - eps, 0.01f + eps), 0.01f);
  TestSetPacketLossRate(&states, I(0.01f - eps, 0.00f      ), 0.00f);
  // clang-format on
}

TEST(AudioEncoderOpusTest, SetReceiverFrameLengthRange) {
  auto states = CreateCodec(2);
  // Before calling to |SetReceiverFrameLengthRange|,
  // |supported_frame_lengths_ms| should contain only the frame length being
  // used.
  using ::testing::ElementsAre;
  EXPECT_THAT(states.encoder->supported_frame_lengths_ms(),
              ElementsAre(states.encoder->next_frame_length_ms()));
  states.encoder->SetReceiverFrameLengthRange(0, 12345);
  EXPECT_THAT(states.encoder->supported_frame_lengths_ms(),
              ElementsAre(20, 60));
  states.encoder->SetReceiverFrameLengthRange(21, 60);
  EXPECT_THAT(states.encoder->supported_frame_lengths_ms(), ElementsAre(60));
  states.encoder->SetReceiverFrameLengthRange(20, 59);
  EXPECT_THAT(states.encoder->supported_frame_lengths_ms(), ElementsAre(20));
}

TEST(AudioEncoderOpusTest, InvokeAudioNetworkAdaptorOnReceivedUplinkBandwidth) {
  auto states = CreateCodec(2);
  states.encoder->EnableAudioNetworkAdaptor("", nullptr);

  auto config = CreateEncoderRuntimeConfig();
  EXPECT_CALL(**states.mock_audio_network_adaptor, GetEncoderRuntimeConfig())
      .WillOnce(Return(config));

  // Since using mock audio network adaptor, any bandwidth value is fine.
  constexpr int kUplinkBandwidth = 50000;
  EXPECT_CALL(**states.mock_audio_network_adaptor,
              SetUplinkBandwidth(kUplinkBandwidth));
  states.encoder->OnReceivedUplinkBandwidth(kUplinkBandwidth);

  CheckEncoderRuntimeConfig(states.encoder.get(), config);
}

TEST(AudioEncoderOpusTest,
     InvokeAudioNetworkAdaptorOnReceivedUplinkPacketLossFraction) {
  auto states = CreateCodec(2);
  states.encoder->EnableAudioNetworkAdaptor("", nullptr);

  auto config = CreateEncoderRuntimeConfig();
  EXPECT_CALL(**states.mock_audio_network_adaptor, GetEncoderRuntimeConfig())
      .WillOnce(Return(config));

  // Since using mock audio network adaptor, any packet loss fraction is fine.
  constexpr float kUplinkPacketLoss = 0.1f;
  EXPECT_CALL(**states.mock_audio_network_adaptor,
              SetUplinkPacketLossFraction(kUplinkPacketLoss));
  states.encoder->OnReceivedUplinkPacketLossFraction(kUplinkPacketLoss);

  CheckEncoderRuntimeConfig(states.encoder.get(), config);
}

TEST(AudioEncoderOpusTest,
     InvokeAudioNetworkAdaptorOnReceivedTargetAudioBitrate) {
  auto states = CreateCodec(2);
  states.encoder->EnableAudioNetworkAdaptor("", nullptr);

  auto config = CreateEncoderRuntimeConfig();
  EXPECT_CALL(**states.mock_audio_network_adaptor, GetEncoderRuntimeConfig())
      .WillOnce(Return(config));

  // Since using mock audio network adaptor, any target audio bitrate is fine.
  constexpr int kTargetAudioBitrate = 30000;
  EXPECT_CALL(**states.mock_audio_network_adaptor,
              SetTargetAudioBitrate(kTargetAudioBitrate));
  states.encoder->OnReceivedTargetAudioBitrate(kTargetAudioBitrate);

  CheckEncoderRuntimeConfig(states.encoder.get(), config);
}

TEST(AudioEncoderOpusTest, InvokeAudioNetworkAdaptorOnReceivedRtt) {
  auto states = CreateCodec(2);
  states.encoder->EnableAudioNetworkAdaptor("", nullptr);

  auto config = CreateEncoderRuntimeConfig();
  EXPECT_CALL(**states.mock_audio_network_adaptor, GetEncoderRuntimeConfig())
      .WillOnce(Return(config));

  // Since using mock audio network adaptor, any rtt is fine.
  constexpr int kRtt = 30;
  EXPECT_CALL(**states.mock_audio_network_adaptor, SetRtt(kRtt));
  states.encoder->OnReceivedRtt(kRtt);

  CheckEncoderRuntimeConfig(states.encoder.get(), config);
}

TEST(AudioEncoderOpusTest, InvokeAudioNetworkAdaptorOnReceivedOverhead) {
  auto states = CreateCodec(2);
  states.encoder->EnableAudioNetworkAdaptor("", nullptr);

  auto config = CreateEncoderRuntimeConfig();
  EXPECT_CALL(**states.mock_audio_network_adaptor, GetEncoderRuntimeConfig())
      .WillOnce(Return(config));

  // Since using mock audio network adaptor, any overhead is fine.
  constexpr size_t kOverhead = 64;
  EXPECT_CALL(**states.mock_audio_network_adaptor, SetOverhead(kOverhead));
  states.encoder->OnReceivedOverhead(kOverhead);

  CheckEncoderRuntimeConfig(states.encoder.get(), config);
}

TEST(AudioEncoderOpusTest,
     PacketLossFractionSmoothedOnSetUplinkPacketLossFraction) {
  auto states = CreateCodec(2);

  // The values are carefully chosen so that if no smoothing is made, the test
  // will fail.
  constexpr float kPacketLossFraction_1 = 0.02f;
  constexpr float kPacketLossFraction_2 = 0.198f;
  // |kSecondSampleTimeMs| is chosen to ease the calculation since
  // 0.9999 ^ 6931 = 0.5.
  constexpr int64_t kSecondSampleTimeMs = 6931;

  // First time, no filtering.
  states.encoder->OnReceivedUplinkPacketLossFraction(kPacketLossFraction_1);
  EXPECT_FLOAT_EQ(0.01f, states.encoder->packet_loss_rate());

  states.simulated_clock->AdvanceTimeMilliseconds(kSecondSampleTimeMs);
  states.encoder->OnReceivedUplinkPacketLossFraction(kPacketLossFraction_2);

  // Now the output of packet loss fraction smoother should be
  // (0.02 + 0.198) / 2 = 0.109, which reach the threshold for the optimized
  // packet loss rate to increase to 0.05. If no smoothing has been made, the
  // optimized packet loss rate should have been increase to 0.1.
  EXPECT_FLOAT_EQ(0.05f, states.encoder->packet_loss_rate());
}

TEST(AudioEncoderOpusTest, DoNotInvokeSetTargetBitrateIfOverheadUnknown) {
  test::ScopedFieldTrials override_field_trials(
      "WebRTC-SendSideBwe-WithOverhead/Enabled/");

  auto states = CreateCodec(2);

  states.encoder->OnReceivedTargetAudioBitrate(kDefaultOpusSettings.rate * 2);

  // Since |OnReceivedOverhead| has not been called, the codec bitrate should
  // not change.
  EXPECT_EQ(kDefaultOpusSettings.rate, states.encoder->GetTargetBitrate());
}

TEST(AudioEncoderOpusTest, OverheadRemovedFromTargetAudioBitrate) {
  test::ScopedFieldTrials override_field_trials(
      "WebRTC-SendSideBwe-WithOverhead/Enabled/");

  auto states = CreateCodec(2);

  constexpr size_t kOverheadBytesPerPacket = 64;
  states.encoder->OnReceivedOverhead(kOverheadBytesPerPacket);

  constexpr int kTargetBitrateBps = 40000;
  states.encoder->OnReceivedTargetAudioBitrate(kTargetBitrateBps);

  int packet_rate = rtc::CheckedDivExact(48000, kDefaultOpusSettings.pacsize);
  EXPECT_EQ(kTargetBitrateBps -
                8 * static_cast<int>(kOverheadBytesPerPacket) * packet_rate,
            states.encoder->GetTargetBitrate());
}

TEST(AudioEncoderOpusTest, BitrateBounded) {
  test::ScopedFieldTrials override_field_trials(
      "WebRTC-SendSideBwe-WithOverhead/Enabled/");

  constexpr int kMinBitrateBps = 500;
  constexpr int kMaxBitrateBps = 512000;

  auto states = CreateCodec(2);

  constexpr size_t kOverheadBytesPerPacket = 64;
  states.encoder->OnReceivedOverhead(kOverheadBytesPerPacket);

  int packet_rate = rtc::CheckedDivExact(48000, kDefaultOpusSettings.pacsize);

  // Set a target rate that is smaller than |kMinBitrateBps| when overhead is
  // subtracted. The eventual codec rate should be bounded by |kMinBitrateBps|.
  int target_bitrate =
      kOverheadBytesPerPacket * 8 * packet_rate + kMinBitrateBps - 1;
  states.encoder->OnReceivedTargetAudioBitrate(target_bitrate);
  EXPECT_EQ(kMinBitrateBps, states.encoder->GetTargetBitrate());

  // Set a target rate that is greater than |kMaxBitrateBps| when overhead is
  // subtracted. The eventual codec rate should be bounded by |kMaxBitrateBps|.
  target_bitrate =
      kOverheadBytesPerPacket * 8 * packet_rate + kMaxBitrateBps + 1;
  states.encoder->OnReceivedTargetAudioBitrate(target_bitrate);
  EXPECT_EQ(kMaxBitrateBps, states.encoder->GetTargetBitrate());
}

}  // namespace webrtc
