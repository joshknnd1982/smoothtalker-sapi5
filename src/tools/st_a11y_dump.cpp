//: st_a11y_dump -- report what a screen reader will see in a dialog.
//:
//: Launches an executable, waits for its top-level window, and prints, for
//: every child control in z-order: the window class, the control id, whether
//: it is a tab stop, and the MSAA name, role, value, state and default
//: action.  For a dialog, tab stops in z-order *are* the tab order, so this
//: answers "is everything labelled and reachable" in one pass.
//:
//: Written in C++ rather than scripted because the scripted equivalents do
//: not work here.  PowerShell's UIA client reports every one of these
//: controls as an anonymous "Pane", and its IDispatch late binding cannot
//: call the oleacc proxy that backs a plain Button or Static -- only the
//: trackbar, which implements IAccessible itself, comes back.  Calling
//: IAccessible through its vtable, as a screen reader does, is the only way
//: to see what a screen reader sees.

#include <windows.h>
#include <oleacc.h>

#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "oleacc.lib")

namespace {

int g_problems = 0;

[[nodiscard]] std::wstring class_of(HWND h)
{
    wchar_t buf[256] = {0};
    GetClassNameW(h, buf, 256);
    return buf;
}

[[nodiscard]] std::wstring text_of(HWND h)
{
    wchar_t buf[512] = {0};
    GetWindowTextW(h, buf, 512);
    return buf;
}

[[nodiscard]] std::wstring role_text(DWORD role)
{
    wchar_t buf[128] = {0};
    GetRoleTextW(role, buf, 128);
    return buf;
}

[[nodiscard]] std::wstring state_text(DWORD state)
{
    std::wstring out;
    for (int bit = 0; bit < 32; ++bit) {
        const DWORD mask = 1u << bit;
        if (!(state & mask)) {
            continue;
        }
        wchar_t buf[128] = {0};
        if (GetStateTextW(mask, buf, 128) > 0) {
            if (!out.empty()) {
                out += L",";
            }
            out += buf;
        }
    }
    return out;
}

[[nodiscard]] std::wstring bstr_to_wstring(BSTR b)
{
    std::wstring s = b ? b : L"";
    if (b) {
        SysFreeString(b);
    }
    return s;
}

struct control_info {
    std::wstring cls;
    int id = 0;
    bool tabstop = false;
    bool visible = false;
    std::wstring caption;
    std::wstring name;
    std::wstring role;
    std::wstring value;
    std::wstring state;
    std::wstring action;
    std::wstring shortcut;
};

[[nodiscard]] control_info inspect(HWND h)
{
    control_info info;
    info.cls = class_of(h);
    info.id = GetDlgCtrlID(h);
    const LONG style = GetWindowLongW(h, GWL_STYLE);
    info.tabstop = (style & WS_TABSTOP) != 0;
    info.visible = IsWindowVisible(h) != FALSE;
    info.caption = text_of(h);

    IAccessible* acc = nullptr;
    const HRESULT hr = AccessibleObjectFromWindow(
        h, OBJID_CLIENT, IID_IAccessible, reinterpret_cast<void**>(&acc));
    if (FAILED(hr) || !acc) {
        info.name = L"<no IAccessible>";
        return info;
    }

    VARIANT self;
    VariantInit(&self);
    self.vt = VT_I4;
    self.lVal = CHILDID_SELF;

    BSTR b = nullptr;
    if (SUCCEEDED(acc->get_accName(self, &b))) {
        info.name = bstr_to_wstring(b);
    }
    b = nullptr;
    if (SUCCEEDED(acc->get_accValue(self, &b))) {
        info.value = bstr_to_wstring(b);
    }
    b = nullptr;
    if (SUCCEEDED(acc->get_accDefaultAction(self, &b))) {
        info.action = bstr_to_wstring(b);
    }
    b = nullptr;
    if (SUCCEEDED(acc->get_accKeyboardShortcut(self, &b))) {
        info.shortcut = bstr_to_wstring(b);
    }

    VARIANT v;
    VariantInit(&v);
    if (SUCCEEDED(acc->get_accRole(self, &v)) && v.vt == VT_I4) {
        info.role = role_text(static_cast<DWORD>(v.lVal));
    }
    VariantClear(&v);
    VariantInit(&v);
    if (SUCCEEDED(acc->get_accState(self, &v)) && v.vt == VT_I4) {
        info.state = state_text(static_cast<DWORD>(v.lVal));
    }
    VariantClear(&v);

    acc->Release();
    return info;
}

//: A control the user can land on must say what it is.  Static text is
//: exempt: it is the label, not the thing being labelled.
void audit(const control_info& c)
{
    if (!c.tabstop || !c.visible) {
        return;
    }
    if (c.name.empty() || c.name == L"<no IAccessible>") {
        std::wprintf(L"  PROBLEM: control %d (%ls) is focusable but has no "
                     L"accessible name\n",
                     c.id, c.cls.c_str());
        ++g_problems;
    }
    if (c.role.empty()) {
        std::wprintf(L"  PROBLEM: control %d (%ls) has no accessible role\n",
                     c.id, c.cls.c_str());
        ++g_problems;
    }
    if (c.shortcut.empty()) {
        std::wprintf(L"  NOTE: control %d (\"%ls\") has no access key\n", c.id,
                     c.name.c_str());
    }
}

}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        std::fwprintf(stderr,
                      L"usage: st_a11y_dump <exe> [--keep]\n"
                      L"  Launches <exe>, dumps the MSAA view of its window, "
                      L"and closes it again.\n");
        return 2;
    }
    const std::wstring exe = argv[1];
    bool keep = false;
    for (int i = 2; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--keep") {
            keep = true;
        }
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + exe + L"\"";
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi)) {
        std::fwprintf(stderr, L"could not start %ls (%lu)\n", exe.c_str(),
                      GetLastError());
        return 1;
    }
    WaitForInputIdle(pi.hProcess, 5000);

    // Find the process's top-level window.  EnumWindows rather than
    // FindWindow so the caption does not have to be known in advance.
    struct finder {
        DWORD pid;
        HWND found;
    } f = {pi.dwProcessId, nullptr};

    for (int attempt = 0; attempt < 50 && !f.found; ++attempt) {
        EnumWindows(
            [](HWND h, LPARAM p) -> BOOL {
                auto* self = reinterpret_cast<finder*>(p);
                DWORD pid = 0;
                GetWindowThreadProcessId(h, &pid);
                if (pid == self->pid && IsWindowVisible(h)) {
                    self->found = h;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&f));
        if (!f.found) {
            Sleep(100);
        }
    }

    if (!f.found) {
        std::fwprintf(stderr, L"no window appeared\n");
        TerminateProcess(pi.hProcess, 1);
        return 1;
    }

    std::wprintf(L"window: \"%ls\" (class %ls)\n\n", text_of(f.found).c_str(),
                 class_of(f.found).c_str());
    std::wprintf(L"%-3s %-18s %-5s %-4s %-38s %-12s %-22s %s\n", L"#",
                 L"class", L"id", L"tab", L"MSAA name", L"MSAA role",
                 L"MSAA value", L"state");
    std::wprintf(L"%ls\n", std::wstring(150, L'-').c_str());

    std::vector<control_info> controls;
    int n = 0;
    for (HWND child = GetWindow(f.found, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        const control_info c = inspect(child);
        controls.push_back(c);
        std::wprintf(L"%-3d %-18ls %-5d %-4ls %-38ls %-12ls %-22ls %ls\n", ++n,
                     c.cls.c_str(), c.id, c.tabstop ? L"yes" : L"no",
                     c.name.c_str(), c.role.c_str(), c.value.c_str(),
                     c.state.c_str());
    }

    std::wprintf(L"\nTAB ORDER (focusable, in order):\n");
    int order = 0;
    for (const control_info& c : controls) {
        if (c.tabstop && c.visible) {
            std::wprintf(L"  %d. \"%ls\" [%ls]%ls%ls\n", ++order,
                         c.name.c_str(), c.role.c_str(),
                         c.value.empty() ? L"" : (L" = " + c.value).c_str(),
                         c.shortcut.empty()
                             ? L""
                             : (L"  access key " + c.shortcut).c_str());
        }
    }

    std::wprintf(L"\nAUDIT:\n");
    for (const control_info& c : controls) {
        audit(c);
    }
    std::wprintf(L"%d problem(s)\n", g_problems);

    if (!keep) {
        PostMessageW(f.found, WM_CLOSE, 0, 0);
        if (WaitForSingleObject(pi.hProcess, 3000) != WAIT_OBJECT_0) {
            TerminateProcess(pi.hProcess, 0);
        }
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CoUninitialize();
    return g_problems ? 1 : 0;
}
