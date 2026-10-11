/** @file EnvFollower.h
 *  @brief A simple asymmetric peak/envelope follower for the meters: VOLUME's VU meter
 *  (PassthroughEngine::GetVUSample, read by NormalPage) and the FX keys' levels. It runs per
 *  sample in the audio callback, but only measures: nothing it computes reaches the outputs.
 */
#pragma once
#include "daisysp.h"

namespace chompi {
class EnvFollower
{
    public:

        EnvFollower() {}
        ~EnvFollower() {}

        void Init()
        {
            last_samp_ = 0.f;
        }

        void Process(float samp)
        {
            samp = fabsf(samp);
            samp = daisysp::fclamp(samp, 0.f, 1.f);

            const float b = samp > last_samp_ ? kUp : kDown;
            const float g = 1.f - b;

            last_samp_ = samp * g + last_samp_ * b;
        }

        // also kicks up the gain a bunch and then clips
        inline float GetLastSamp() const
        { 
            float vu_sample = last_samp_;
            vu_sample *= 5.f;
            vu_sample = vu_sample > 1.f ? 1.f : vu_sample;

            return vu_sample;
        }

    private:
        // the share of the last value kept: fast up, slow down
        static constexpr float kUp = .5f, kDown = .9993f;
        float last_samp_;
};
} // namespace chompi