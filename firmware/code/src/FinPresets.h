/** @file FinPresets.h
 *  @brief The finalizer's four presets (#345), kept in the master file (MasterSettings.h), not
 *  in the scenes: their SAVE / COPY / DELETE flow, the scenes' rules (SceneControls.h) on four
 *  slots. A mode key with key 15 held starts it (NormalPage.h), the presets on the keys of
 *  scenes 1-4; a preset picks (COPY: the source, then where to), the same one again
 *  deselects; SHIFT confirms. Unlike the scenes there's no blank one: SAVE takes any, COPY a
 *  stored source to any other, DELETE a stored one. No hardware here: test/controls.cpp runs it.
 */
#pragma once
#include "MasterSettings.h"
#include "SceneControls.h"

#pragma GCC push_options
#pragma GCC optimize("Os")

namespace chompi
{

class FinPresets
{
public:
    void Init(MasterSettings* master)
    {
        master_ = master;
        Cancel();
    }

    /** A mode key: the same one again cancels, another switches over */
    void ModePressed(SceneMode mode)
    {
        mode_ = mode_ == mode ? SceneMode::NONE : mode;
        sel_ = src_ = kNoScene;
    }
    void Cancel()
    {
        mode_ = SceneMode::NONE;
        sel_ = src_ = kNoScene;
    }

    inline bool Used(size_t p) const { return master_->fin_presets_used >> p & 1; }

    /** Whether preset p can be picked in the mode */
    bool Valid(size_t p) const
    {
        switch (mode_)
        {
        case SceneMode::SAVE:
            return p < kFinPresets;
        case SceneMode::COPY:
            return p < kFinPresets && (src_ == kNoScene ? Used(p) : static_cast<int>(p) != src_);
        case SceneMode::DELETE:
            return p < kFinPresets && Used(p);
        case SceneMode::NONE:
            break;
        }
        return false;
    }

    /** Preset p pressed in the mode: picked or deselected (true), or refused */
    bool Pressed(size_t p)
    {
        const int s = static_cast<int>(p);
        if (mode_ == SceneMode::COPY && s == src_)
        {
            if (sel_ != kNoScene)
                return false;
            src_ = kNoScene;
            return true;
        }
        if (!Valid(p))
            return false;
        if (mode_ == SceneMode::COPY && src_ == kNoScene)
            src_ = s;
        else
            sel_ = s == sel_ ? kNoScene : s;
        return true;
    }

    /** True while SHIFT would confirm: a mode with its preset picked */
    inline bool Armed() const { return mode_ != SceneMode::NONE && sel_ != kNoScene; }

    /** SHIFT: carries out the armed mode, SAVE taking params (the finalizer's now), and ends
     *  it. The preset it changed, kNoScene if it wasn't armed */
    int Confirm(const float* params)
    {
        if (!Armed())
            return kNoScene;
        const size_t p = static_cast<size_t>(sel_);
        const uint8_t bit = static_cast<uint8_t>(1u << p);
        switch (mode_)
        {
        case SceneMode::SAVE:
            for (size_t i = 0; i < kNumFinParams; i++)
                master_->fin_presets[p][i] = params[i];
            master_->fin_presets_used |= bit;
            break;
        case SceneMode::COPY:
            for (size_t i = 0; i < kNumFinParams; i++)
                master_->fin_presets[p][i] = master_->fin_presets[src_][i];
            master_->fin_presets_used |= bit;
            break;
        case SceneMode::DELETE:
            master_->fin_presets_used &= static_cast<uint8_t>(~bit);
            break;
        case SceneMode::NONE:
            break;
        }
        const int changed = sel_;
        Cancel();
        return changed;
    }

    inline SceneMode Mode() const { return mode_; }
    inline int Selected() const { return sel_; }
    inline int Source() const { return src_; }

private:
    MasterSettings* master_ = nullptr;
    SceneMode mode_ = SceneMode::NONE;
    int sel_ = kNoScene, src_ = kNoScene;
};

} // namespace chompi

#pragma GCC pop_options
