#include <windows.h>

#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "app/chat_window.h"
#include "core/claude_process.h"
#include "core/secrets.h"

namespace {

// The GUI exe has no console of its own; when started from a script its stdout is a pipe or a file.
void Print(const std::string& text) {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == nullptr || out == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

// aicontrols --import-secrets A,B,C : stores the values of the environment variables A, B, C. This is how
// secrets move in from another program (it passes them as env_secrets); no value is ever printed.
// aicontrols --list-secrets : the names.
int RunSecretsCommand(const std::wstring& command, const std::wstring& argument) {
    aic::SecretStore store(aic::SecretStore::DefaultPath());
    store.Load();
    std::string report;
    if (command == L"--import-secrets") {
        std::stringstream names(aic::ToUtf8(argument));
        std::string name;
        std::vector<wchar_t> value(32768);
        int failed = 0;
        while (std::getline(names, name, ',')) {
            if (name.empty()) {
                continue;
            }
            const DWORD length = GetEnvironmentVariableW(aic::ToWide(name).c_str(), value.data(),
                                                         static_cast<DWORD>(value.size()));
            if (length == 0 || length >= value.size()) {
                report += "missing: " + name + "\n";
                ++failed;
                continue;
            }
            store.Set(name, aic::ToUtf8(std::wstring_view(value.data(), length)));
            report += "stored: " + name + "\n";
        }
        SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
        if (!store.Save()) {
            Print("cannot save the secrets file\n");
            return 2;
        }
        Print(report);
        return failed == 0 ? 0 : 1;
    }
    for (const std::string& name : store.Names()) {
        report += name + "\n";
    }
    Print(report.empty() ? "no secrets\n" : report);
    return 0;
}

// The parent must be ssh.exe. A same-user process with another name gets nothing, and a failed lookup refuses.
bool ParentIsSsh() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD parent = 0;
    const DWORD self = GetCurrentProcessId();
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == self) {
                parent = entry.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    if (parent == 0) {
        return false;
    }
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent);
    if (process == nullptr) {
        return false;
    }
    wchar_t image[1024]{};
    DWORD size = static_cast<DWORD>(std::size(image));
    const BOOL named = QueryFullProcessImageNameW(process, 0, image, &size);
    CloseHandle(process);
    if (!named || size == 0) {
        return false;
    }
    const wchar_t* slash = wcsrchr(image, L'\\');
    const wchar_t* file = slash != nullptr ? slash + 1 : image;
    return CompareStringOrdinal(file, -1, L"ssh.exe", -1, TRUE) == CSTR_EQUAL;
}

// ssh's SSH_ASKPASS. AIC_ASKPASS is a one-shot ticket, not the secret's name. Answered only when the parent
// process is ssh.exe and the prompt asks for a password.
int AskPass(const std::wstring& token, std::wstring prompt) {
    std::ranges::transform(prompt, prompt.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (prompt.find(L"password") == std::wstring::npos || !ParentIsSsh()) {
        return 1;
    }
    const std::optional<std::string> name = aic::ClaimAskPass(token);
    if (!name) {
        return 1;
    }
    aic::SecretStore store(aic::SecretStore::DefaultPath());
    store.Load();
    std::optional<std::string> value = store.Get(*name);
    if (!value) {
        return 1;
    }
    value->push_back('\n');
    Print(*value);
    SecureZeroMemory(value->data(), value->size());
    return 0;
}

}  // namespace

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int show) {
    int count = 0;
    const std::unique_ptr<LPWSTR, decltype(&LocalFree)> args(CommandLineToArgvW(GetCommandLineW(), &count),
                                                              &LocalFree);
    std::wstring askpass(512, L'\0');
    if (const DWORD length = GetEnvironmentVariableW(L"AIC_ASKPASS", askpass.data(), static_cast<DWORD>(askpass.size()));
        length > 0 && length < askpass.size()) {
        askpass.resize(length);
        return AskPass(askpass, args && count >= 2 ? args.get()[1] : L"");
    }
    if (args && count >= 2) {
        const std::wstring command = args.get()[1];
        if (command == L"--import-secrets" && count >= 3) {
            return RunSecretsCommand(command, args.get()[2]);
        }
        if (command == L"--list-secrets") {
            return RunSecretsCommand(command, {});
        }
    }
    return aic::RunChatWindow(instance, show);
}
