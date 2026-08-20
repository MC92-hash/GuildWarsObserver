#pragma once

// Where a weapon model attaches to a character skeleton.
//
// GW's BB9/FA1 bone records carry an index, a hierarchy depth and a bind position - no names and
// no stable ids - so there is nothing to look a "hand bone" up *by*. Bone counts also differ per
// rig (76..109 across the rigs GW Observer loads), so no constant index exists either.
//
// The hand is instead found from the topology, which is unmistakable: a wrist carrying five
// three-bone finger chains. Validated against every rig in animation_cache.ini - exactly two hits
// on each of the 12 humanoid rigs, zero on the 3 non-humanoid ones. Section 4 of the
// weapon-rendering design notes (gwobserver-private/docs/WeaponModelRendering.md) carries the
// measurements, and the FA1 attachment-block route that was tried first and abandoned.
//
// Header-only and free of ImGui/D3D, matching the rest of Animation/ and Parsers/.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "AnimationClip.h"

namespace GW::Animation
{
    // The two hands are mirror images in bind pose, so which one holds the main-hand weapon
    // cannot be decided from the data. Both are stored by bind-pose side and the caller picks,
    // which keeps that open question a render-time toggle rather than something baked into the
    // cache and re-resolved whenever it flips.
    struct WeaponSocket
    {
        int32_t negativeXBone = -1;  // indices into AnimationClip::boneTracks
        int32_t positiveXBone = -1;
        bool    resolved      = false; // false => rig has no hands; render no weapon

        int32_t MainHand(bool weaponHandIsPositiveX) const
        {
            return weaponHandIsPositiveX ? positiveXBone : negativeXBone;
        }

        int32_t OffHand(bool weaponHandIsPositiveX) const
        {
            return weaponHandIsPositiveX ? negativeXBone : positiveXBone;
        }

        bool IsValidFor(size_t boneCount) const
        {
            return resolved
                && negativeXBone >= 0 && static_cast<size_t>(negativeXBone) < boneCount
                && positiveXBone >= 0 && static_cast<size_t>(positiveXBone) < boneCount;
        }
    };

    namespace detail
    {
        // A hand has five digits, each a chain of at least three bones, and sits out on the arm.
        // The three clauses are all load-bearing: without the exact-five test and the lateral
        // offset the pelvis/spine root (8-10 children, 6-10 qualifying chains, x ~ 0) matches on
        // several rigs.
        inline constexpr int   kDigitCount      = 5;
        inline constexpr int   kMaxHandChildren = 7;   // five digits plus one or two leaf helpers
        inline constexpr int   kMinDigitDepth   = 2;   // knuckle -> mid -> tip
        inline constexpr float kMinLateralOffset = 5.f;
    }

    // Finds the two hand bones of a character rig, or leaves the socket unresolved when the rig
    // is not humanoid.
    inline WeaponSocket ResolveWeaponSocket(const AnimationClip& clip)
    {
        WeaponSocket socket;

        const size_t boneCount = clip.boneTracks.size();
        if (boneCount == 0 || boneCount != clip.boneParents.size())
            return socket;

        std::vector<std::vector<int32_t>> children(boneCount);
        for (size_t i = 0; i < boneCount; i++)
        {
            const int32_t parent = clip.boneParents[i];
            if (parent >= 0 && static_cast<size_t>(parent) < boneCount)
                children[parent].push_back(static_cast<int32_t>(i));
        }

        // Subtree depth. A single reverse pass suffices when children always outrank their
        // parent, which holds for every rig sampled, but FA1's parent encodings are decoded from
        // a byte stream rather than guaranteed by construction - so relax to a fixpoint instead of
        // trusting the ordering. Converges in one extra pass on real data.
        std::vector<int> subtreeDepth(boneCount, 0);
        for (size_t pass = 0; pass < boneCount; pass++)
        {
            bool changed = false;
            for (size_t i = boneCount; i-- > 0; )
            {
                int deepest = 0;
                for (int32_t child : children[i])
                    if (static_cast<size_t>(child) < boneCount)
                        deepest = std::max(deepest, subtreeDepth[child] + 1);
                if (deepest != subtreeDepth[i]) { subtreeDepth[i] = deepest; changed = true; }
            }
            if (!changed) break;
        }

        int32_t negativeX = -1, positiveX = -1;
        int hits = 0;

        for (size_t i = 0; i < boneCount; i++)
        {
            if (children[i].size() > detail::kMaxHandChildren)
                continue;

            int digits = 0;
            for (int32_t child : children[i])
                if (static_cast<size_t>(child) < boneCount && subtreeDepth[child] >= detail::kMinDigitDepth)
                    digits++;
            if (digits != detail::kDigitCount)
                continue;

            const float x = clip.boneTracks[i].basePosition.x;
            if (std::abs(x) <= detail::kMinLateralOffset)
                continue;

            hits++;
            if (x < 0.f) { if (negativeX < 0) negativeX = static_cast<int32_t>(i); }
            else         { if (positiveX < 0) positiveX = static_cast<int32_t>(i); }
        }

        // Anything other than one hand per side means the rule did not recognise this rig. Leave
        // the socket unresolved rather than attaching a weapon to a guess.
        if (hits != 2 || negativeX < 0 || positiveX < 0)
            return socket;

        socket.negativeXBone = negativeX;
        socket.positiveXBone = positiveX;
        socket.resolved      = true;
        return socket;
    }
}
