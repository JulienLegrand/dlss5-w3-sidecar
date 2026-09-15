#include "present/WindowTracker.h"

#include <cwchar>
#include <iterator>
#include <mutex>
#include <unordered_map>

namespace sidecar {
namespace {

std::mutex g_mutex;
std::unordered_map<HWND, WindowTracker*> g_trackers;

struct EnumWindowContext {
  std::optional<TargetWindow> target;
};

BOOL CALLBACK FindWowWindowByTitle(HWND hwnd, LPARAM parameter) {
  auto& context = *reinterpret_cast<EnumWindowContext*>(parameter);
  if (!IsWindowVisible(hwnd)) return TRUE;

  wchar_t title[256] = {};
  GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
  if (wcsstr(title, kWowWindowTitle) == nullptr) return TRUE;

  auto rect = ClientRectInScreen(hwnd);
  if (!rect || rect->right - rect->left <= 0 || rect->bottom - rect->top <= 0) {
    return TRUE;
  }

  context.target = TargetWindow{hwnd, *rect, IsBorderless(hwnd)};
  return FALSE;
}

}  // namespace

bool IsBorderless(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd)) return false;
  const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
  // Borderless means no caption and no thick frame. WoW's borderless mode is
  // WS_POPUP; its windowed mode carries WS_CAPTION.
  return (style & WS_CAPTION) == 0 && (style & WS_THICKFRAME) == 0;
}

std::optional<RECT> ClientRectInScreen(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd)) return std::nullopt;
  RECT client{};
  if (!GetClientRect(hwnd, &client)) return std::nullopt;
  POINT topLeft{client.left, client.top};
  POINT bottomRight{client.right, client.bottom};
  if (!ClientToScreen(hwnd, &topLeft)) return std::nullopt;
  if (!ClientToScreen(hwnd, &bottomRight)) return std::nullopt;
  return RECT{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
}

bool ClassNameIsSpecificEnough(const wchar_t* className) {
  if (!className) return false;
  // Short names are the ones that could belong to anything. The threshold is
  // deliberately crude: the point is to separate "w" from "GxWindowClass", not
  // to rank plausibility.
  return wcslen(className) > 4;
}

bool WowWindowMatches(const wchar_t* className, const wchar_t* title) {
  if (!className || !title) return false;

  if (wcscmp(className, L"waApplication Window") == 0 ||
      wcscmp(className, L"GxWindowClass") == 0) {
    return true;
  }

  return wcscmp(className, L"w") == 0 &&
         wcscmp(title, kWowWindowTitle) == 0;
}

std::optional<TargetWindow> FindWowWindow() {
  // Take the first visible window with a real client area.
  for (const wchar_t* className : kWowWindowClasses) {
    HWND hwnd = nullptr;
    while ((hwnd = FindWindowExW(nullptr, hwnd, className, nullptr)) != nullptr) {
      if (!IsWindowVisible(hwnd)) continue;

      // GetWindowTextW opens nothing: it is a message to the window, not a
      // handle to the process behind it (I2).
      wchar_t title[256] = {};
      GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
      if (!WowWindowMatches(className, title)) continue;

      auto rect = ClientRectInScreen(hwnd);
      if (!rect) continue;
      if (rect->right - rect->left <= 0 || rect->bottom - rect->top <= 0) continue;

      TargetWindow t;
      t.hwnd = hwnd;
      t.clientScreen = *rect;
      t.borderless = IsBorderless(hwnd);
      return t;
    }
  }

  // Reforged can register a different top-level class depending on the
  // renderer and launcher version. Its main window keeps the product title,
  // so use that as a fallback after the known class names.
  EnumWindowContext context;
  EnumWindows(&FindWowWindowByTitle, reinterpret_cast<LPARAM>(&context));
  if (context.target) return context.target;

  return std::nullopt;
}

std::unique_ptr<WindowTracker> WindowTracker::Create(HWND target, MovedCallback onMoved) {
  if (!target || !IsWindow(target)) return nullptr;

  std::unique_ptr<WindowTracker> t(new WindowTracker());
  t->target_ = target;
  t->onMoved_ = std::move(onMoved);

  DWORD processId = 0;
  const DWORD threadId = GetWindowThreadProcessId(target, &processId);

  // WINEVENT_OUTOFCONTEXT is mandatory (I3): it delivers events to our own
  // process without loading anything into the observed one.
  //
  // The hook is already narrowed to the target's process and thread, so
  // WINEVENT_SKIPOWNPROCESS would buy nothing against WoW while making a
  // target inside this process -- which is how the tracker is tested --
  // impossible to observe.
  t->hook_ = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
                             nullptr, &WindowTracker::EventProc,
                             processId, threadId,
                             WINEVENT_OUTOFCONTEXT);
  if (!t->hook_) return nullptr;

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_trackers[target] = t.get();
  }
  return t;
}

WindowTracker::~WindowTracker() {
  if (hook_) UnhookWinEvent(hook_);
  std::lock_guard<std::mutex> lock(g_mutex);
  g_trackers.erase(target_);
}

void CALLBACK WindowTracker::EventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                       LONG objectId, LONG, DWORD, DWORD) {
  if (event != EVENT_OBJECT_LOCATIONCHANGE || objectId != OBJID_WINDOW) return;

  WindowTracker* tracker = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_trackers.find(hwnd);
    if (it == g_trackers.end()) return;
    tracker = it->second;
  }
  if (!tracker || !tracker->onMoved_) return;
  if (auto rect = ClientRectInScreen(hwnd)) tracker->onMoved_(*rect);
}

}  // namespace sidecar
