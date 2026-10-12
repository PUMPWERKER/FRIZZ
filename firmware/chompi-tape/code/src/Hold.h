#pragma GCC push_options
#pragma GCC optimize ("Os")
#pragma once

#include <stdint.h>

namespace chompi
{
    // A switch that tells a click from a hold: Held fires once while it's down, Release is a click unless it fired.
    class Hold
    {
      public:
        void Init()
        {
            at_ = 0;
            down_ = fired_ = false;
        }

        void Press(uint32_t now)
        {
            at_ = now;
            down_ = true;
            fired_ = false;
        }

        // true for a click
        bool Release()
        {
            const bool click = down_ && !fired_;
            down_ = false;
            return click;
        }

        // true once, when it's been down long enough
        bool Held(uint32_t now)
        {
            if(!down_ || fired_ || int32_t(now - at_) < int32_t(kTime)) // signed: now may be from before the press
                return false;
            fired_ = true;
            return true;
        }

        // the press went to something else: no hold, no click
        void Cancel() { fired_ = down_; }

        // the release went elsewhere
        void Lost() { down_ = false; }

        bool Down() const { return down_; }

      private:
        static constexpr uint32_t kTime = 500; // ms

        uint32_t at_;
        bool down_, fired_;
    };
}

#pragma GCC pop_options
