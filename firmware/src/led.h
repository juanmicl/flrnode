#pragma once

// LED abstraction so apps stay Arduino-free. D1 mini onboard LED is
// active-low on GPIO2.

// Context: setup. Blocking: never.
void ledInit();
// Context: any. Blocking: never.
void ledSet(bool on);
// SOS pattern: 3x200 ms / 3x600 ms / 3x200 ms, 2 s pause (~3.4 s/cycle).
// Context: halted error loop ONLY (it delays).
void ledSosStep();
