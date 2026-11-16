#pragma once

#include <windows.h>

namespace aic {

// The owned popup that paints a revealed secret. Its title is empty, so a screenshot that looks for
// "AIControls" does not pick it. Capture of this class is scrubbed out of screenshots.
inline constexpr wchar_t kSecretRevealClass[] = L"AIControlsSecret";

// True when the message was swallowed. A read that arrived by SendMessage from another thread (a stealer)
// gets nothing back. A same-thread GetWindowText still works, so the form can save what was typed.
bool GuardSecretField(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);

// Fills `rect` with dark bands and no letters. A print of the reveal window paints this.
void PaintSecretSmear(HDC dc, const RECT& rect);

// Paints over every visible top-level AIControlsSecret window that intersects the captured bitmap.
// `originX` and `originY` are the capture's top-left in screen pixels (negative on a monitor to the left).
void ScrubSecretReveal(HDC dc, int originX, int originY, int width, int height);

// WDA_EXCLUDEFROMCAPTURE while `exclude` is true, otherwise WDA_NONE. False when the call does not take
// (the window is not top-level, or this Windows has no such affinity).
bool ExcludeFromCapture(HWND hwnd, bool exclude);

}  // namespace aic
