#include "app/toast.h"

#include <propidl.h>
#include <propkey.h>
#include <propvarutil.h>

// propkey.h only declares these unless INITGUID is set, and propsys.lib does not define them.
EXTERN_C const PROPERTYKEY PKEY_AppUserModel_ID = INIT_PKEY_AppUserModel_ID;
EXTERN_C const PROPERTYKEY PKEY_AppUserModel_ToastActivatorCLSID = INIT_PKEY_AppUserModel_ToastActivatorCLSID;
#include <roapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <windows.data.xml.dom.h>
#include <windows.ui.notifications.h>
#include <NotificationActivationCallback.h>

#include <wrl/client.h>
#include <wrl/implements.h>
#include <wrl/wrappers/corewrappers.h>

#include <wil/resource.h>

#include <chrono>
#include <mutex>
#include <string>

namespace aic {
namespace {

// The shortcut's System.AppUserModel.ToastActivatorCLSID. Fixed: a click after an update still finds this class.
constexpr GUID kActivatorClsid = {
    0x5AC4EBF9, 0x5B2D, 0x4C9C, {0x9B, 0xAF, 0x39, 0xF6, 0xFB, 0xDB, 0x72, 0x44}};

// Without a registered activator, Windows starts the shortcut's exe for a click and a second window opens.
// The running process registers this class; COM delivers the click here instead of launching again.
class Activator final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                                            INotificationActivationCallback> {
 public:
    IFACEMETHODIMP Activate(LPCWSTR /*appUserModelId*/, LPCWSTR invokedArgs,
                            const NOTIFICATION_USER_INPUT_DATA* /*data*/, ULONG /*count*/) override;
};

class Factory final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                                          IClassFactory> {
 public:
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID iid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (outer != nullptr) {
            return CLASS_E_NOAGGREGATION;
        }
        auto made = Microsoft::WRL::Make<Activator>();
        return made ? made.CopyTo(iid, object) : E_OUTOFMEMORY;
    }

    IFACEMETHODIMP LockServer(BOOL) override { return S_OK; }
};

std::mutex g_mu;
HWND g_hwnd = nullptr;
bool g_deliver = false;  // until the chat list is loaded, a click is only remembered
std::string g_pending;
Microsoft::WRL::ComPtr<IClassFactory> g_factory;
DWORD g_cookie = 0;

std::wstring ExePath() {
    std::wstring path(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return path;
}

bool InstalledCopy() {
    wil::unique_cotaskmem_string local;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, local.put()))) {
        return false;
    }
    const std::wstring prefix = std::wstring(local.get()) + L"\\Programs\\AIControls\\";
    const std::wstring exe = ExePath();
    return exe.size() >= prefix.size() && CompareStringOrdinal(exe.c_str(), static_cast<int>(prefix.size()),
                                                                prefix.c_str(), static_cast<int>(prefix.size()),
                                                                TRUE) == CSTR_EQUAL;
}

std::wstring EscapeXml(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size());
    for (const wchar_t c : text) {
        switch (c) {
        case L'&':
            out += L"&amp;";
            break;
        case L'<':
            out += L"&lt;";
            break;
        case L'>':
            out += L"&gt;";
            break;
        case L'"':
            out += L"&quot;";
            break;
        case L'\'':
            out += L"&apos;";
            break;
        default:
            if (c >= 0x20 || c == L'\t') {
                out += c;
            }
            break;
        }
    }
    return out;
}

// Tag is 16 characters and group is 64, and a group may not be empty. One group per chat, one tag, so the next
// notice for that chat replaces the one still sitting in Action Center.
std::wstring GroupOf(std::string_view session) {
    std::wstring group;
    group.reserve(session.size());
    for (const unsigned char c : session) {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            group.push_back(static_cast<wchar_t>(c));
        }
    }
    if (group.empty()) {
        group = L"chat";
    }
    if (group.size() > 64) {
        group.resize(64);
    }
    return group;
}

std::string Ascii(std::wstring_view text) {
    std::string out;
    out.reserve(text.size());
    for (const wchar_t c : text) {
        if (c == 0) {
            break;
        }
        if (c < 128) {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

void PostOpen(HWND hwnd, std::string session) {
    auto owned = std::make_unique<std::string>(std::move(session));
    if (PostMessageW(hwnd, kToastOpen, 0, reinterpret_cast<LPARAM>(owned.get()))) {
        owned.release();
    }
}

// A click before the chat list exists is stored. A later click is posted: SendMessage from here would deadlock,
// the callback runs on the window's thread.
void Deliver(std::string session) {
    HWND hwnd = nullptr;
    bool post = false;
    {
        const std::lock_guard lock(g_mu);
        hwnd = g_hwnd;
        if (!g_deliver || g_hwnd == nullptr) {
            g_pending = std::move(session);
        } else {
            post = true;
        }
    }
    if (hwnd != nullptr) {
        // The click is what Windows treats as a user action, so the raise is allowed from here and from nowhere later.
        if (IsIconic(hwnd)) {
            ShowWindow(hwnd, SW_RESTORE);
        }
        SetForegroundWindow(hwnd);
    }
    if (post) {
        PostOpen(hwnd, std::move(session));
    }
}

void StampLink(const std::wstring& path, bool create) {
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
        return;
    }
    Microsoft::WRL::ComPtr<IPersistFile> file;
    if (FAILED(link.As(&file))) {
        return;
    }
    const bool exists = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (exists) {
        if (FAILED(file->Load(path.c_str(), STGM_READWRITE))) {
            return;
        }
        wchar_t target[MAX_PATH]{};
        WIN32_FIND_DATAW found{};
        if (FAILED(link->GetPath(target, MAX_PATH, &found, SLGP_RAWPATH))) {
            return;
        }
        const wchar_t* slash = wcsrchr(target, L'\\');
        const wchar_t* name = slash != nullptr ? slash + 1 : target;
        if (CompareStringOrdinal(name, -1, L"aicontrols.exe", -1, TRUE) != CSTR_EQUAL) {
            return;
        }
    } else if (!create) {
        return;
    } else if (FAILED(link->SetPath(ExePath().c_str()))) {
        return;
    }
    Microsoft::WRL::ComPtr<IPropertyStore> store;
    if (FAILED(link.As(&store))) {
        return;
    }
    PROPVARIANT id{};
    if (SUCCEEDED(InitPropVariantFromString(kAppUserModelId, &id))) {
        store->SetValue(PKEY_AppUserModel_ID, id);
        PropVariantClear(&id);
    }
    PROPVARIANT clsid{};
    if (SUCCEEDED(InitPropVariantFromCLSID(kActivatorClsid, &clsid))) {
        store->SetValue(PKEY_AppUserModel_ToastActivatorCLSID, clsid);
        PropVariantClear(&clsid);
    }
    if (FAILED(store->Commit())) {
        return;
    }
    file->Save(path.c_str(), TRUE);
}

void StampShortcuts() {
    const bool create = InstalledCopy();
    const auto stamp = [&](REFKNOWNFOLDERID folder, bool mayCreate) {
        wil::unique_cotaskmem_string root;
        if (FAILED(SHGetKnownFolderPath(folder, 0, nullptr, root.put())) || root.get() == nullptr) {
            return;
        }
        StampLink(std::wstring(root.get()) + L"\\AIControls.lnk", mayCreate && create);
    };
    stamp(FOLDERID_Programs, true);
    stamp(FOLDERID_Desktop, false);
}

void RegisterActivatorKey() {
    wchar_t guid[64]{};
    if (StringFromGUID2(kActivatorClsid, guid, static_cast<int>(std::size(guid))) == 0) {
        return;
    }
    const std::wstring subkey = std::wstring(L"Software\\Classes\\CLSID\\") + guid + L"\\LocalServer32";
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return;
    }
    const std::wstring command = L"\"" + ExePath() + L"\"";
    const DWORD bytes = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
    RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), bytes);
    RegCloseKey(key);
}

struct RecentNotice {
    std::wstring heading;
    std::string session;
    std::chrono::steady_clock::time_point when{};
};

RecentNotice g_recent;

IFACEMETHODIMP Activator::Activate(LPCWSTR /*appUserModelId*/, LPCWSTR invokedArgs,
                                   const NOTIFICATION_USER_INPUT_DATA* /*data*/, ULONG /*count*/) {
    Deliver(Ascii(invokedArgs != nullptr ? invokedArgs : L""));
    return S_OK;
}

}  // namespace

void PrepareToasts() {
    SetCurrentProcessExplicitAppUserModelID(kAppUserModelId);
    StampShortcuts();
    RegisterActivatorKey();
    g_factory = Microsoft::WRL::Make<Factory>();
    if (g_factory == nullptr) {
        return;
    }
    // Suspended until the window exists: a click during startup waits instead of arriving with nowhere to go.
    // A failed registration must not leave a cookie: StopToasts would revoke it.
    DWORD cookie = 0;
    if (SUCCEEDED(CoRegisterClassObject(kActivatorClsid, g_factory.Get(), CLSCTX_LOCAL_SERVER,
                                        REGCLS_MULTIPLEUSE | REGCLS_SUSPENDED, &cookie))) {
        g_cookie = cookie;
    }
}

void AttachToastWindow(HWND window) {
    {
        const std::lock_guard lock(g_mu);
        g_hwnd = window;
    }
    CoResumeClassObjects();
}

std::string TakePendingToast() {
    const std::lock_guard lock(g_mu);
    g_deliver = true;
    return std::exchange(g_pending, {});
}

void StopToasts() {
    if (g_cookie != 0) {
        CoRevokeClassObject(g_cookie);
        g_cookie = 0;
    }
    g_factory.Reset();
    const std::lock_guard lock(g_mu);
    g_hwnd = nullptr;
    g_deliver = false;
}

void ShowToast(std::wstring_view heading, std::wstring_view body, std::string_view sessionId) {
    const auto now = std::chrono::steady_clock::now();
    const std::string session(sessionId);
    if (session == g_recent.session && heading == g_recent.heading && now - g_recent.when < std::chrono::seconds(20)) {
        return;
    }
    using Microsoft::WRL::ComPtr;
    using Microsoft::WRL::Wrappers::HStringReference;
    ComPtr<ABI::Windows::Data::Xml::Dom::IXmlDocument> document;
    ComPtr<IInspectable> inspectable;
    if (FAILED(RoActivateInstance(HStringReference(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument).Get(),
                                  &inspectable)) ||
        FAILED(inspectable.As(&document))) {
        return;
    }
    // launch is a session id, ASCII. The two text nodes are the heading and the chat's name.
    const std::wstring launch(sessionId.begin(), sessionId.end());
    const std::wstring toastXml =
        L"<toast launch=\"" + EscapeXml(launch) +
        L"\" activationType=\"foreground\"><visual><binding template=\"ToastGeneric\"><text>" + EscapeXml(heading) +
        L"</text><text>" + EscapeXml(body) + L"</text></binding></visual></toast>";
    ComPtr<ABI::Windows::Data::Xml::Dom::IXmlDocumentIO> loader;
    if (FAILED(document.As(&loader)) || FAILED(loader->LoadXml(HStringReference(toastXml.c_str()).Get()))) {
        return;
    }
    ComPtr<ABI::Windows::UI::Notifications::IToastNotificationFactory> factory;
    if (FAILED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotification).Get(),
                                      IID_PPV_ARGS(&factory)))) {
        return;
    }
    ComPtr<ABI::Windows::UI::Notifications::IToastNotification> toast;
    if (FAILED(factory->CreateToastNotification(document.Get(), &toast))) {
        return;
    }
    ComPtr<ABI::Windows::UI::Notifications::IToastNotification2> tagged;
    if (SUCCEEDED(toast.As(&tagged))) {
        const std::wstring group = GroupOf(sessionId);
        tagged->put_Tag(HStringReference(L"turn").Get());
        tagged->put_Group(HStringReference(group.c_str()).Get());
    }
    ComPtr<ABI::Windows::UI::Notifications::IToastNotificationManagerStatics> manager;
    if (FAILED(RoGetActivationFactory(
            HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).Get(),
            IID_PPV_ARGS(&manager)))) {
        return;
    }
    ComPtr<ABI::Windows::UI::Notifications::IToastNotifier> notifier;
    if (FAILED(manager->CreateToastNotifierWithId(HStringReference(kAppUserModelId).Get(), &notifier)) ||
        FAILED(notifier->Show(toast.Get()))) {
        return;
    }
    g_recent = {std::wstring(heading), session, now};
}

bool WindowIsAway(HWND window) {
    if (window == nullptr || !IsWindowVisible(window) || IsIconic(window)) {
        return true;
    }
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr || foreground == window || IsChild(window, foreground)) {
        return foreground == nullptr;
    }
    return GetAncestor(foreground, GA_ROOTOWNER) != window;
}

bool RaiseRunningWindow() {
    const HWND window = FindWindowW(L"AIControlsWindow", nullptr);
    if (window == nullptr) {
        return false;
    }
    const DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD self = GetCurrentThreadId();
    const bool attached = foregroundThread != 0 && foregroundThread != self && AttachThreadInput(self, foregroundThread, TRUE);
    if (IsIconic(window)) {
        ShowWindow(window, SW_RESTORE);
    } else {
        ShowWindow(window, SW_SHOW);
    }
    SetForegroundWindow(window);
    BringWindowToTop(window);
    if (attached) {
        AttachThreadInput(self, foregroundThread, FALSE);
    }
    return true;
}

}  // namespace aic
