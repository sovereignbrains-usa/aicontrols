#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace aic {

// One id for the process, the Start Menu shortcut and the toasts. It is not a version: an update keeps it.
inline constexpr wchar_t kAppUserModelId[] = L"sovereignbrains.AIControls";

// lParam owns a std::string* — the session a toast click asked to open. Empty only brings the window forward.
inline constexpr UINT kToastOpen = WM_APP + 21;

// Before any window: the process id, the shortcut properties and the COM class a click comes back through.
void PrepareToasts();

// The window exists. A click that arrived while it was being created is delivered, later clicks are posted.
void AttachToastWindow(HWND window);

// A click queued before delivery was turned on. Empty if there was none. Turns delivery on.
std::string TakePendingToast();

// Drops the COM registration. Call before COM is uninitialized.
void StopToasts();

// Action Center. Does not take focus. The same chat and the same heading twice within a few seconds is one notice.
void ShowToast(std::wstring_view heading, std::wstring_view body, std::string_view sessionId);

// Minimized, hidden, or another program is in front. A menu or dialog of this window still counts as looking.
bool WindowIsAway(HWND window);

// A second launch finds the window that is already open, brings it forward and returns true. The caller exits.
bool RaiseRunningWindow();

}  // namespace aic
