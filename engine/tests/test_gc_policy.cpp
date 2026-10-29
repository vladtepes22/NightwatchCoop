// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone CPU checks of the texture collector's rules (texture_cache/gc_policy.h).
#include <cassert>
#include <cstdint>
#include <cstdio>
#include "gpu/shadps4/video_core/texture_cache/gc_policy.h"

using namespace VideoCore::GcPolicy;

int main() {
    // The player's 8 GB card with 5835 MiB of budget: 0.7.2 started at 4668 (80 %) and freed
    // textures used a moment ago from 5251 (88 %). Now the hard mark is 5% (at most 384 MiB) under
    // the budget and the gradual band 10 % (at most 768 MiB) under that.
    {
        const Marks m = MarksFor(5835 * MiB);
        assert(m.budget == 5835 * MiB);
        assert(m.hard == 5835 * MiB - 5835 * MiB / 20);
        assert(m.soft == m.hard - 5835 * MiB / 10);
        assert(m.soft > 4668 * MiB && m.hard > 5251 * MiB);
        assert(m.trigger <= m.soft && m.soft < m.hard && m.hard < m.budget);
    }
    // A full 8 GB budget: the reserves are capped (384 + 768 MiB).
    {
        const Marks m = MarksFor(8000 * MiB);
        assert(m.hard == 8000 * MiB - 384 * MiB);
        assert(m.soft == m.hard - 768 * MiB);
    }
    // A 3 GB debug cap (BB_GC_BUDGET_MB=3000): the floors (160 + 300 MiB).
    {
        const Marks m = MarksFor(3000 * MiB);
        assert(m.hard == 3000 * MiB - 160 * MiB);
        assert(m.soft == m.hard - 300 * MiB);
    }
    // A tiny budget still orders the marks.
    {
        const Marks m = MarksFor(256 * MiB);
        assert(m.trigger <= m.soft && m.soft <= m.hard && m.hard < m.budget);
        assert(LevelFor(0, MarksFor(0)) == Level::None);
    }

    // The budget never exceeds the VRAM heaps; no driver budget: the heaps.
    assert(BudgetFor(9000 * MiB, 8192 * MiB) == 8192 * MiB);
    assert(BudgetFor(5835 * MiB, 8192 * MiB) == 5835 * MiB);
    assert(BudgetFor(0, 8192 * MiB) == 8192 * MiB);
    assert(BudgetFor(3000 * MiB, 0) == 3000 * MiB);

    // The GTX 1650 report: 4149 MiB heap, the driver's budget lower. The marks come from the
    // budget, and the game's data leaves VRAM only past the texture hard mark (textures go first
    // from the soft mark on), no new copies halfway from the hard mark to the budget.
    {
        const Marks m = MarksFor(BudgetFor(3500 * MiB, 4149 * MiB));
        assert(m.budget == 3500 * MiB && m.soft < 3200 * MiB);
        assert(GuestLevelFor(m.soft, m) == 0);
        assert(GuestLevelFor(m.hard - 1, m) == 0);
        assert(GuestLevelFor(m.hard, m) == 1);
        assert(GuestLevelFor(m.hard + (m.budget - m.hard) / 2 - 1, m) == 1);
        assert(GuestLevelFor(3771 * MiB, m) == 2);
        assert(GuestLevelFor(3771 * MiB, MarksFor(0)) == 0);
    }

    // Holes in shared blocks and images not destroyed yet count as free.
    assert(EffectiveUsage(5000 * MiB, 300 * MiB, 200 * MiB) == 4500 * MiB);
    assert(EffectiveUsage(100 * MiB, 300 * MiB, 0) == 0);

    // The game's data: the collector's usage less its own free VRAM slots, never above the
    // collector's, so it leaves VRAM only while textures are past their hard mark too.
    assert(GuestUsage(5000 * MiB, 300 * MiB, 200 * MiB, 500 * MiB) == 4000 * MiB);
    assert(GuestUsage(5000 * MiB, 300 * MiB, 200 * MiB, 9000 * MiB) == 0);
    {
        // The GTX 1650 log of 0.2.5 beta 2: budget 3391 MiB, ~3336 MiB raw, ~180 MiB of VMA block
        // slack, 624 MiB of free slots in the game's chunks. On the raw usage it stayed full for
        // good (1-3 FPS) while the collector saw 3156 MiB, under its hard mark, and freed nothing.
        const Marks m = MarksFor(3391 * MiB);
        const u64 raw = 3336 * MiB, slack = 180 * MiB, slots = 624 * MiB;
        assert(GuestLevelFor(raw, m) == 2);
        assert(LevelFor(EffectiveUsage(raw, slack, 0), m) < Level::Hard);
        assert(GuestLevelFor(GuestUsage(raw, slack, 0, slots), m) == 0);
        // Whenever the game's data is under pressure, the collector is at its hard mark or past.
        for (u64 used = 2000 * MiB; used <= 4000 * MiB; used += 16 * MiB) {
            for (u64 free_slots = 0; free_slots <= 800 * MiB; free_slots += 200 * MiB) {
                if (GuestLevelFor(GuestUsage(used, slack, 0, free_slots), m) >= 1) {
                    assert(LevelFor(EffectiveUsage(used, slack, 0), m) >= Level::Hard);
                }
            }
        }
        // That case stays at level 0 on the driver's count too: the raw usage is under the budget.
        assert(GuestLevelOnDriver(0, 0, raw, m) == 0);
        assert(GuestLevelOnDriver(0, 1, raw, m) == 1); // 3336 >= release 3306: stays tight
    }
    {
        // The GTX 1650 log of 0.2.5 beta 3: 3282 of 3151 MiB by the driver, 266 MiB of VMA block
        // slack, 190 MiB of free slots: level 0 by GuestUsage, the chunks grew, nothing left VRAM.
        const Marks m = MarksFor(3151 * MiB);
        const u64 raw = 3282 * MiB;
        const int discounted = GuestLevelFor(GuestUsage(raw, 266 * MiB, 0, 190 * MiB), m);
        assert(discounted == 0);
        assert(GuestLevelOnDriver(discounted, 0, raw, m) == 1);
        // Never lowers a level, and leaves the hard / full levels alone.
        assert(GuestLevelOnDriver(2, 0, raw, m) == 2);
        assert(GuestLevelOnDriver(2, 2, 2000 * MiB, m) == 2);
        // Hysteresis: tight until halfway between the hard mark and the budget.
        const u64 release = m.hard + (m.budget - m.hard) / 2;
        assert(GuestLevelOnDriver(0, 1, release, m) == 1);
        assert(GuestLevelOnDriver(0, 1, release - 1, m) == 0);
        assert(GuestLevelOnDriver(0, 0, m.budget - 1, m) == 0);
        assert(GuestLevelOnDriver(0, 1, raw, MarksFor(0)) == 0);
    }

    // Levels.
    {
        const Marks m = MarksFor(5835 * MiB);
        assert(LevelFor(m.trigger - 1, m) == Level::None);
        assert(LevelFor(m.trigger, m) == Level::Idle);
        assert(LevelFor(m.soft, m) == Level::Soft);
        assert(LevelFor(m.hard - 1, m) == Level::Soft);
        assert(LevelFor(m.hard, m) == Level::Hard);
        assert(LevelFor(m.budget, m) == Level::Over);
        // The player's reports: 4628 MiB in use (0.7.2: past the pressure mark) is no pressure now.
        assert(LevelFor(4628 * MiB, m) == Level::Idle);
    }

    // Rules: the higher the level, the younger the images that may go and the bigger the quota;
    // images of the last 8 frames never; write-backs (a GPU wait) only past the budget.
    {
        const Rule idle = RuleFor(Level::Idle), soft = RuleFor(Level::Soft),
                   hard = RuleFor(Level::Hard), over = RuleFor(Level::Over);
        assert(idle.idle_seconds == 20 && soft.idle_seconds == 5 && hard.idle_seconds == 2);
        assert(over.idle_seconds == 0 && over.protect_frames >= 8);
        assert(idle.protect_frames > soft.protect_frames && soft.protect_frames > hard.protect_frames);
        assert(hard.protect_frames > over.protect_frames);
        assert(idle.images_per_frame < soft.images_per_frame &&
               soft.images_per_frame < hard.images_per_frame &&
               hard.images_per_frame < over.images_per_frame);
        assert(!idle.write_back && !soft.write_back && !hard.write_back && over.write_back);
        assert(!hard.drop_old_tiled && over.drop_old_tiled);
        assert(over.images_per_frame <= 128);
        assert(RuleFor(Level::None).images_per_frame == 0);
        assert(RuleFor(Level::Soft, 20, 3).idle_seconds == 3);
        assert(RuleFor(Level::Hard, 20, 1).idle_seconds == 1);
    }

    // The frame quota: shared by all submissions of a frame, new with the next frame or after 64
    // submissions without one.
    {
        const Rule soft = RuleFor(Level::Soft);
        FrameQuota q;
        q.Begin(10, 1000);
        u32 taken = 0;
        while (q.Left(soft)) {
            q.Take(1 * MiB);
            ++taken;
        }
        assert(taken == soft.images_per_frame);
        q.Begin(10, 1010); // the same frame, later submission: still used up
        assert(!q.Left(soft));
        q.Begin(11, 1020);
        assert(q.Left(soft) && q.images == 0);
        // Big images: the byte quota ends it first.
        taken = 0;
        while (q.Left(soft)) {
            q.Take(40 * MiB);
            ++taken;
        }
        assert(taken == 3);
        q.Begin(11, 1020 + 64); // no frame for 64 submissions (a loading screen)
        assert(q.Left(soft));
        q.write_backs = 1;
        q.Begin(12, 1100);
        assert(q.write_backs == 0);
    }

    // Frame ticks: the tick at the start of the frame N ago; frames without a submission take the
    // next tick seen; nothing that old yet: 0.
    {
        FrameTicks<16> f;
        assert(f.TickFramesAgo(0) == 0);
        f.Note(100, 5000);
        f.Note(100, 5007); // the same frame: its start stays
        assert(f.TickFramesAgo(0) == 5000);
        f.Note(101, 5020);
        f.Note(104, 5100); // 102, 103 without a submission
        assert(f.TickFramesAgo(0) == 5100);
        assert(f.TickFramesAgo(1) == 5100 && f.TickFramesAgo(2) == 5100);
        assert(f.TickFramesAgo(3) == 5020);
        assert(f.TickFramesAgo(4) == 5000);
        assert(f.TickFramesAgo(16) == 0);
        f.Note(200, 9000); // a long gap: the ring holds the new tick everywhere
        assert(f.TickFramesAgo(15) == 9000);
    }

    std::printf("gc-policy-test: ok\n");
    return 0;
}
