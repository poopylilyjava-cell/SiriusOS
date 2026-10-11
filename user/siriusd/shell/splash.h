// The loading screen: what the desktop shows first, while it starts -- a
// star, the name and a spinner over the wallpaper, coming up out of the
// kernel's black screen and fading away to reveal the desktop.
#pragma once

namespace desktop {

// Show it from now, for at least `seconds` before it starts fading out.
void StartSplash(double seconds);

// True while any of it is on screen: the desktop draws continuously then.
bool SplashActive();

// True while it covers the screen completely: nothing under it need be
// drawn, and input is not the desktop's yet.
bool SplashCovers();

// Draw it, over whatever the frame holds so far. Between NewFrame() and
// Render(), after everything else.
void DrawSplash();

} // namespace desktop
