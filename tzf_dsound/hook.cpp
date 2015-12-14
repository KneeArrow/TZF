/**
 * This file is part of Tales of Zestiria "Fix".
 *
 * Tales of Zestiria "Fix" is free software : you can redistribute it
 * and/or modify it under the terms of the GNU General Public License
 * as published by The Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * Tales of Zestiria "Fix" is distributed in the hope that it will be
 * useful,
 *
 * But WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Tales of Zestiria "Fix".
 *
 *   If not, see <http://www.gnu.org/licenses/>.
 *
**/
#include <string>

#include "hook.h"
#include "log.h"
#include "command.h"
#include "sound.h"
#include "steam.h"

#include "framerate.h"
#include "render.h"

#include <d3d9.h>

#include "config.h"

#include <mmsystem.h>
#pragma comment (lib, "winmm.lib")

#include <comdef.h>

struct window_t {
  DWORD proc_id;
  HWND  root;
};

BOOL
CALLBACK
TZF_EnumWindows (HWND hWnd, LPARAM lParam)
{
  window_t& win = *(window_t*)lParam;

  DWORD proc_id = 0;

  GetWindowThreadProcessId (hWnd, &proc_id);

  if (win.proc_id != proc_id) {
    if (GetWindow (hWnd, GW_OWNER) != (HWND)nullptr ||
        GetWindowTextLength (hWnd) < 30             ||
     (! IsWindowVisible     (hWnd)))
      return TRUE;
  }

  win.root = hWnd;
  return FALSE;
}

HWND
TZF_FindRootWindow (DWORD proc_id)
{
  window_t win;

  win.proc_id  = proc_id;
  win.root     = 0;

  EnumWindows (TZF_EnumWindows, (LPARAM)&win);

  return win.root;
}


// Returns the original cursor position and stores the new one in pPoint
POINT
CalcCursorPos (LPPOINT pPoint)
{
  float xscale, yscale;
  float xoff,   yoff;

  extern void TZF_ComputeAspectCoeffs ( float& xscale,
                                        float& yscale,
                                        float& xoff,
                                        float& yoff );

  TZF_ComputeAspectCoeffs (xscale, yscale, xoff, yoff);

  pPoint->x = (pPoint->x - xoff) * xscale;
  pPoint->y = (pPoint->y - yoff) * yscale;

  return *pPoint;
}


WNDPROC original_wndproc = nullptr;

LRESULT
CALLBACK
DetourWindowProc ( _In_  HWND   hWnd,
                   _In_  UINT   uMsg,
                   _In_  WPARAM wParam,
                   _In_  LPARAM lParam )
{
  if (game_state.needsFixedMouseCoords () && config.render.aspect_correction) {
    if (uMsg >= WM_MOUSEFIRST && uMsg <= WM_MOUSELAST) {
      POINT p;

      p.x = MAKEPOINTS (lParam).x;
      p.y = MAKEPOINTS (lParam).y;

      CalcCursorPos (&p);

      return CallWindowProc (original_wndproc, hWnd, uMsg, wParam, MAKELPARAM (p.x, p.y));
    }
  }

  return CallWindowProc (original_wndproc, hWnd, uMsg, wParam, lParam);
}


typedef BOOL (WINAPI *GetCursorInfo_t)
  (_Inout_ PCURSORINFO pci);

GetCursorInfo_t GetCursorInfo_Original = nullptr;

BOOL
WINAPI
GetCursorInfo_Detour (PCURSORINFO pci)
{
  BOOL ret = GetCursorInfo_Original (pci);

  // Correct the cursor position for Aspect Ratio
  if (game_state.needsFixedMouseCoords () && config.render.aspect_correction) {
    POINT pt;

    pt.x = pci->ptScreenPos.x;
    pt.y = pci->ptScreenPos.y;

    CalcCursorPos (&pt);

    pci->ptScreenPos.x = pt.x;
    pci->ptScreenPos.y = pt.y;
  }

  return ret;
}

typedef BOOL (WINAPI *GetCursorPos_t)
  (_Out_ LPPOINT lpPoint);

GetCursorPos_t GetCursorPos_Original = nullptr;

BOOL
WINAPI
GetCursorPos_Detour (LPPOINT lpPoint)
{
  BOOL ret = GetCursorPos_Original (lpPoint);

  // Correct the cursor position for Aspect Ratio
  if (game_state.needsFixedMouseCoords () && config.render.aspect_correction)
    CalcCursorPos (lpPoint);

  // Defer initialization of the Window Message redirection stuff until
  //   the first time the game calls GetCursorPos (...)
  if (original_wndproc == nullptr && tzf::RenderFix::hWndDevice != NULL) {
    original_wndproc =
      (WNDPROC)GetWindowLong (tzf::RenderFix::hWndDevice, GWL_WNDPROC);

    SetWindowLong ( tzf::RenderFix::hWndDevice,
                      GWL_WNDPROC,
                        (LONG)DetourWindowProc );
  }

  return ret;
}


class TZF_InputHooker
{
private:
  HANDLE                  hMsgPump;
  struct hooks_t {
    HHOOK                 keyboard;
    HHOOK                 mouse;
  } hooks;

  static TZF_InputHooker* pInputHook;

  static char                text [16384];

  static BYTE keys_ [256];
  static bool visible;

  static bool command_issued;
  static std::string result_str;

  struct command_history_t {
    std::vector <std::string> history;
    int_fast32_t              idx     = -1;
  } static commands;

protected:
  TZF_InputHooker (void) { }

public:
  static TZF_InputHooker* getInstance (void)
  {
    if (pInputHook == NULL)
      pInputHook = new TZF_InputHooker ();

    return pInputHook;
  }

  void Start (void)
  {
    hMsgPump =
      CreateThread ( NULL,
                       NULL,
                         TZF_InputHooker::MessagePump,
                           &hooks,
                             NULL,
                               NULL );

    TZF_CreateDLLHook ( L"user32.dll", "GetCursorInfo",
                        GetCursorInfo_Detour,
              (LPVOID*)&GetCursorInfo_Original );

    TZF_CreateDLLHook ( L"user32.dll", "GetCursorPos",
                        GetCursorPos_Detour,
              (LPVOID*)&GetCursorPos_Original );
  }

  void End (void)
  {
    TerminateThread     (hMsgPump, 0);
    UnhookWindowsHookEx (hooks.keyboard);
    UnhookWindowsHookEx (hooks.mouse);
  }

  void Draw (void)
  {
    typedef BOOL (__stdcall *BMF_DrawExternalOSD_t)(std::string app_name, std::string text);

    static HMODULE               hMod =
      GetModuleHandle (L"d3d9.dll");
    static BMF_DrawExternalOSD_t BMF_DrawExternalOSD
      =
      (BMF_DrawExternalOSD_t)GetProcAddress (hMod, "BMF_DrawExternalOSD");

    std::string output;

    static DWORD last_time = timeGetTime ();
    static bool  carret    = true;

    if (visible) {
      output += text;

      // Blink the Carret
      if (timeGetTime () - last_time > 333) {
        carret = ! carret;

        last_time = timeGetTime ();
      }

      if (carret)
        output += "-";

      // Show Command Results
      if (command_issued) {
        output += "\n";
        output += result_str;
      }
    }

    BMF_DrawExternalOSD ("ToZ Fix", output.c_str ());
  }

  HANDLE GetThread (void)
  {
    return hMsgPump;
  }

  static DWORD
  WINAPI
  MessagePump (LPVOID hook_ptr)
  {
    hooks_t* pHooks = (hooks_t *)hook_ptr;

    ZeroMemory (text, 16384);

    text [0] = '>';

    extern    HMODULE hDLLMod;

    DWORD dwThreadId;

    int hits = 0;

    DWORD dwTime = timeGetTime ();

    while (true) {
      // Spin until the game has a render window setup and various
      //   other resources loaded
      if (! tzf::RenderFix::pDevice) {
        Sleep (83);
        continue;
      }

      dwThreadId =
        GetWindowThreadProcessId (tzf::RenderFix::hWndDevice, nullptr);

      break;
    }

    dll_log.Log ( L"  # Found window in %03.01f seconds, "
                     L"installing keyboard hook...",
                   (float)(timeGetTime () - dwTime) / 1000.0f );

    dwTime = timeGetTime ();
    hits   = 1;

    while (! (pHooks->keyboard = SetWindowsHookEx ( WH_KEYBOARD,
                                                      KeyboardProc,
                                                        hDLLMod,
                                                          dwThreadId ))) {
      _com_error err (HRESULT_FROM_WIN32 (GetLastError ()));

      dll_log.Log ( L"  @ SetWindowsHookEx failed: 0x%04X (%s)",
                    err.WCode (), err.ErrorMessage () );

      ++hits;

      if (hits >= 5) {
        dll_log.Log ( L"  * Failed to install keyboard hook after %lu tries... "
          L"bailing out!",
          hits );
        return 0;
      }

      Sleep (1);
    }

    while (! (pHooks->mouse = SetWindowsHookEx ( WH_MOUSE,
                                                   MouseProc,
                                                     hDLLMod,
                                                       dwThreadId ))) {
      _com_error err (HRESULT_FROM_WIN32 (GetLastError ()));

      dll_log.Log ( L"  @ SetWindowsHookEx failed: 0x%04X (%s)",
                    err.WCode (), err.ErrorMessage () );

      ++hits;

      if (hits >= 5) {
        dll_log.Log ( L"  * Failed to install mouse hook after %lu tries... "
          L"bailing out!",
          hits );
        return 0;
      }

      Sleep (1);
    }

    dll_log.Log ( L"  * Installed keyboard hook for command console... "
                        L"%lu %s (%lu ms!)",
                  hits,
                    hits > 1 ? L"tries" : L"try",
                      timeGetTime () - dwTime );

    while (true) {
      Sleep (10);
    }
    //193 - 199

    return 0;
  }

  static LRESULT
  CALLBACK
  MouseProc (int nCode, WPARAM wParam, LPARAM lParam)
  {
    MOUSEHOOKSTRUCT* pmh = (MOUSEHOOKSTRUCT *)lParam;

#if 0
    static bool fudging = false;

    if (game_state.hasFixedAspect () && tzf::RenderFix::pDevice != nullptr && (! fudging)) {
      GetCursorPos (&pmh->pt);

      extern POINT CalcCursorPos (LPPOINT pPos);
      extern POINT real_cursor;

      POINT adjusted_cursor;
      adjusted_cursor.x = pmh->pt.x;
      adjusted_cursor.y = pmh->pt.y;

      real_cursor = CalcCursorPos (&adjusted_cursor);

      pmh->pt.x = adjusted_cursor.x;
      pmh->pt.y = adjusted_cursor.y;

      //SetCursorPos (adjusted_cursor.x, adjusted_cursor.y);

      //tzf::RenderFix::pDevice->SetCursorPosition (adjusted_cursor.x,
                                                   //adjusted_cursor.y,
                                                    //0);//D3DCURSOR_IMMEDIATE_UPDATE);
      fudging = true;
      SendMessage (pmh->hwnd, WM_MOUSEMOVE, 0, LOWORD (pmh->pt.x) | HIWORD (pmh->pt.y));
      return 1;
    }
    else {
      fudging = false;
    }
#endif

    return CallNextHookEx (TZF_InputHooker::getInstance ()->hooks.mouse, nCode, wParam, lParam);
  }

  static LRESULT
  CALLBACK
  KeyboardProc (int nCode, WPARAM wParam, LPARAM lParam)
  {
    typedef BOOL (__stdcall *BMF_DrawExternalOSD_t)(std::string app_name, std::string text);

    static HMODULE               hMod =
      GetModuleHandle (L"d3d9.dll");
    static BMF_DrawExternalOSD_t BMF_DrawExternalOSD
                                      =
      (BMF_DrawExternalOSD_t)GetProcAddress (hMod, "BMF_DrawExternalOSD");

    if (nCode >= 0) {
      BYTE    vkCode   = LOWORD (wParam) & 0xFF;
      BYTE    scanCode = HIWORD (lParam) & 0x7F;
      bool    repeated = LOWORD (lParam);
      bool    keyDown  = ! (lParam & 0x80000000);

      if (visible && vkCode == VK_BACK) {
        if (keyDown) {
          size_t len = strlen (text);
                 len--;

          if (len < 1)
            len = 1;

          text [len] = '\0';
        }
      }

      else if ((vkCode == VK_SHIFT || vkCode == VK_LSHIFT || vkCode == VK_RSHIFT)) {
        if (keyDown) keys_ [VK_SHIFT] = 0x81; else keys_ [VK_SHIFT] = 0x00;
      }

      else if ((!repeated) && vkCode == VK_CAPITAL) {
        if (keyDown) if (keys_ [VK_CAPITAL] == 0x00) keys_ [VK_CAPITAL] = 0x81; else keys_ [VK_CAPITAL] = 0x00;
      }

      else if ((vkCode == VK_CONTROL || vkCode == VK_LCONTROL || vkCode == VK_RCONTROL)) {
        if (keyDown) keys_ [VK_CONTROL] = 0x81; else keys_ [VK_CONTROL] = 0x00;
      }

      else if ((vkCode == VK_UP) || (vkCode == VK_DOWN)) {
        if (keyDown && visible) {
          if (vkCode == VK_UP)
            commands.idx--;
          else
            commands.idx++;

          // Clamp the index
          if (commands.idx < 0)
            commands.idx = 0;
          else if (commands.idx >= commands.history.size ())
            commands.idx = commands.history.size () - 1;

          if (commands.history.size ()) {
            strcpy (&text [1], commands.history [commands.idx].c_str ());
            command_issued = false;
          }
        }
      }

      else if (visible && vkCode == VK_RETURN) {
        if (keyDown && LOWORD (lParam) < 2) {
          size_t len = strlen (text+1);
          // Don't process empty or pure whitespace command lines
          if (len > 0 && strspn (text+1, " ") != len) {
            eTB_CommandResult result = command.ProcessCommandLine (text+1);

            if (result.getStatus ()) {
              // Don't repeat the same command over and over
              if (commands.history.size () == 0 ||
                  commands.history.back () != &text [1]) {
                commands.history.push_back (&text [1]);
              }

              commands.idx = commands.history.size ();

              text [1] = '\0';

              command_issued = true;
            }
            else {
              command_issued = false;
            }

            result_str = result.getWord   () + std::string (" ")   +
                         result.getArgs   () + std::string (":  ") +
                         result.getResult ();
          }
        }
      }

      else if (keyDown) {
        bool new_press = keys_ [vkCode] != 0x81;

        keys_ [vkCode] = 0x81;

        if (keys_ [VK_CONTROL] && keys_ [VK_SHIFT]) {
          if (keys_ [VK_TAB] && new_press) {
            visible = ! visible;
            tzf::SteamFix::SetOverlayState (visible);
          }
          else if (keys_ ['1'] && new_press) {
            command.ProcessCommandLine ("AutoAdjust false");
            command.ProcessCommandLine ("TargetFPS 60");
            command.ProcessCommandLine ("TickScale 1");
          }
          else if (keys_ ['2'] && new_press) {
            command.ProcessCommandLine ("AutoAdjust false");
            command.ProcessCommandLine ("TargetFPS 30");
            command.ProcessCommandLine ("TickScale 2");
          }
          else if (keys_ ['3'] && new_press) {
            command.ProcessCommandLine ("AutoAdjust false");
            command.ProcessCommandLine ("TargetFPS 20");
            command.ProcessCommandLine ("TickScale 3");
          }
          else if (keys_ ['4'] && new_press) {
            command.ProcessCommandLine ("AutoAdjust false");
            command.ProcessCommandLine ("TargetFPS 15");
            command.ProcessCommandLine ("TickScale 4");
          }
          else if (keys_ ['5'] && new_press) {
            command.ProcessCommandLine ("AutoAdjust false");
            command.ProcessCommandLine ("TargetFPS 12");
            command.ProcessCommandLine ("TickScale 5");
          }
          else if (keys_ ['6'] && new_press) {
            command.ProcessCommandLine ("AutoAdjust false");
            command.ProcessCommandLine ("TargetFPS 10");
            command.ProcessCommandLine ("TickScale 6");
          }
          else if (keys_ ['9'] && new_press) {
            command.ProcessCommandLine ("AutoAdjust true");
            command.ProcessCommandLine ("TargetFPS 60");
            command.ProcessCommandLine ("TickScale 1");
          }
          else if (keys_ [VK_OEM_PERIOD]) {
            command.ProcessCommandLine ("AutoAdjust true");
            command.ProcessCommandLine ("TickScale 30");
          }
        }

        if (visible) {
          char key_str [2];
          key_str [1] = '\0';

          if (1 == ToAsciiEx ( vkCode,
                                scanCode,
                                keys_,
                              (LPWORD)key_str,
                                0,
                                GetKeyboardLayout (0) )) {
            strncat (text, key_str, 1);
            command_issued = false;
          }
        }
      }

      else if ((! keyDown))
        keys_ [vkCode] = 0x00;

      if (visible) return 1;
    }

    return CallNextHookEx (TZF_InputHooker::getInstance ()->hooks.keyboard, nCode, wParam, lParam);
  };
};


MH_STATUS
WINAPI
TZF_CreateFuncHook ( LPCWSTR pwszFuncName,
                     LPVOID  pTarget,
                     LPVOID  pDetour,
                     LPVOID *ppOriginal )
{
  static HMODULE hParent = GetModuleHandle (L"d3d9.dll");

  typedef MH_STATUS (WINAPI *BMF_CreateFuncHook_t)
      ( LPCWSTR pwszFuncName, LPVOID  pTarget,
        LPVOID  pDetour,      LPVOID *ppOriginal );
  static BMF_CreateFuncHook_t BMF_CreateFuncHook =
    (BMF_CreateFuncHook_t)GetProcAddress (hParent, "BMF_CreateFuncHook");

  return
    BMF_CreateFuncHook (pwszFuncName, pTarget, pDetour, ppOriginal);
}

MH_STATUS
WINAPI
TZF_CreateDLLHook ( LPCWSTR pwszModule, LPCSTR  pszProcName,
                    LPVOID  pDetour,    LPVOID *ppOriginal,
                    LPVOID *ppFuncAddr )
{
  static HMODULE hParent = GetModuleHandle (L"d3d9.dll");

  typedef MH_STATUS (WINAPI *BMF_CreateDLLHook_t)(
        LPCWSTR pwszModule, LPCSTR  pszProcName,
        LPVOID  pDetour,    LPVOID *ppOriginal, 
        LPVOID *ppFuncAddr );
  static BMF_CreateDLLHook_t BMF_CreateDLLHook =
    (BMF_CreateDLLHook_t)GetProcAddress (hParent, "BMF_CreateDLLHook");

  return
    BMF_CreateDLLHook (pwszModule,pszProcName,pDetour,ppOriginal,ppFuncAddr);
}

MH_STATUS
WINAPI
TZF_EnableHook (LPVOID pTarget)
{
  static HMODULE hParent = GetModuleHandle (L"d3d9.dll");

  typedef MH_STATUS (WINAPI *BMF_EnableHook_t)(LPVOID pTarget);
  static BMF_EnableHook_t BMF_EnableHook =
    (BMF_EnableHook_t)GetProcAddress (hParent, "BMF_EnableHook");

  return BMF_EnableHook (pTarget);
}

MH_STATUS
WINAPI
TZF_DisableHook (LPVOID pTarget)
{
  static HMODULE hParent = GetModuleHandle (L"d3d9.dll");

  typedef MH_STATUS (WINAPI *BMF_DisableHook_t)(LPVOID pTarget);
  static BMF_DisableHook_t BMF_DisableHook =
    (BMF_DisableHook_t)GetProcAddress (hParent, "BMF_DisableHook");

  return BMF_DisableHook (pTarget);
}

MH_STATUS
WINAPI
TZF_RemoveHook (LPVOID pTarget)
{
  static HMODULE hParent = GetModuleHandle (L"d3d9.dll");

  typedef MH_STATUS (WINAPI *BMF_RemoveHook_t)(LPVOID pTarget);
  static BMF_RemoveHook_t BMF_RemoveHook =
    (BMF_RemoveHook_t)GetProcAddress (hParent, "BMF_RemoveHook");

  return BMF_RemoveHook (pTarget);
}

MH_STATUS
WINAPI
TZF_Init_MinHook (void)
{
  MH_STATUS status = MH_OK;

  TZF_InputHooker* pHook = TZF_InputHooker::getInstance ();
  pHook->Start ();

  return status;
}

MH_STATUS
WINAPI
TZF_UnInit_MinHook (void)
{
  MH_STATUS status = MH_OK;

  TZF_InputHooker* pHook = TZF_InputHooker::getInstance ();
  pHook->End ();

  return status;
}

void
TZF_DrawCommandConsole (void)
{
  static int draws = 0;

  // Skip the first frame, so that the console appears below the
  //  other OSD.
  if (draws++ > 20) {
    TZF_InputHooker* pHook = TZF_InputHooker::getInstance ();
    pHook->Draw ();
  }
}


TZF_InputHooker* TZF_InputHooker::pInputHook;
char             TZF_InputHooker::text [16384];

BYTE TZF_InputHooker::keys_ [256] = { 0 };
bool TZF_InputHooker::visible     = false;

bool TZF_InputHooker::command_issued = false;
std::string TZF_InputHooker::result_str;

TZF_InputHooker::command_history_t TZF_InputHooker::commands;