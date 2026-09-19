#pragma once
#include <cstdint>

namespace simcore {

struct TransformerComponent {
  uint8_t inputTier;  // input voltage tier (e.g. 2=MV for mv_hv)
  uint8_t outputTier; // output voltage tier (e.g. 3=HV for mv_hv)
  bool stepUp;        // true: low→high, false: high→low
  int32_t buffer;     // internal energy buffer
  int32_t maxInput;   // max EU/t input
  int32_t maxOutput;  // max EU/t output

  TransformerComponent() = default;

  TransformerComponent(uint8_t inTier, uint8_t outTier, bool up, int32_t buf,
                       int32_t maxIn, int32_t maxOut)
      : inputTier(inTier), outputTier(outTier), stepUp(up), buffer(buf),
        maxInput(maxIn), maxOutput(maxOut) {}
};

} // namespace simcore
