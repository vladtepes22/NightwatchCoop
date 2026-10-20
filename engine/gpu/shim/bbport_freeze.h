// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: a frozen game reports itself. See bbport_freeze.cpp.
#pragma once

#include <functional>

namespace BbFreeze {
/// A flip was handled (VideoOut): the game is showing frames.
void NoteFlip();
/// Prints the GPU side's state (schedulers, presentation frames) into the freeze report.
void SetStateReporter(std::function<void()> reporter);
} // namespace BbFreeze
