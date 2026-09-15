#pragma once
#include <windows.h>

#include <functional>
#include <memory>
#include <optional>

namespace sidecar {

struct TargetWindow {
  HWND hwnd = nullptr;
  RECT clientScreen{};
  bool borderless = false;
};

// Warcraft 3 Reforged's top-level window classes. Matching by class rather than
// by process is deliberate: it needs no process handle at all (I2).
inline constexpr const wchar_t* kWowWindowClasses[] = {
    L"Warcraft III",
};

// The window title Warcraft 3 Reforged gives its main window.
inline constexpr const wchar_t* kWowWindowTitle = L"Warcraft III";

// Whether a class name is specific enough to identify Warcraft 3 Reforged by itself.
//
// Pure, and unit-tested, because getting it wrong either misses the game
// entirely or captures somebody else's window.
bool ClassNameIsSpecificEnough(const wchar_t* className);

// Given what can be read from a window without opening its process, does this
// look like Warcraft 3 Reforged's main window?
bool WowWindowMatches(const wchar_t* className, const wchar_t* title);

std::optional<TargetWindow> FindWowWindow();

// Display mode from window styles only.
bool IsBorderless(HWND hwnd);

std::optional<RECT> ClientRectInScreen(HWND hwnd);

// Follows a window through moves and resizes.
//
// The hook is installed WINEVENT_OUTOFCONTEXT. The in-context form maps a DLL
// into the observed process and would destroy the premise of this project (I3).
class WindowTracker {
 public:
  using MovedCallback = std::function<void(const RECT& clientScreen)>;

  static std::unique_ptr<WindowTracker> Create(HWND target, MovedCallback onMoved);
  ~WindowTracker();

 private:
  WindowTracker() = default;

  static void CALLBACK EventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                 LONG objectId, LONG childId,
                                 DWORD threadId, DWORD timestamp);

  HWINEVENTHOOK hook_ = nullptr;
  HWND target_ = nullptr;
  MovedCallback onMoved_;
};

}  // namespace sidecar
