// The Windows half of autostart: the current user's identity and the Task Scheduler's COM API
// (taskschd.h), used instead of schtasks.exe so no command line or temporary file is involved.
#include <windows.h>
// After windows.h, which these need.
#include <sddl.h>
#include <taskschd.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>
#include <vector>

#include "autostart.hpp"

namespace ghostdrive::autostart {

using core::Errc;
using core::Error;
using Microsoft::WRL::ComPtr;

namespace {

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    int n =
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) {
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                              n);
    }
    return out;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr,
                                  0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) {
        ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n,
                              nullptr, nullptr);
    }
    return out;
}

Error hresult_error(const std::string& what, HRESULT hr) {
    char hex[16];
    std::snprintf(hex, sizeof(hex), "0x%08lx", static_cast<unsigned long>(hr));
    return Error{Errc::io, what + " failed (" + hex + ")"};
}

class Bstr {
public:
    explicit Bstr(const std::wstring& text)
        : p_(::SysAllocStringLen(text.data(), static_cast<UINT>(text.size()))) {}
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    ~Bstr() { ::SysFreeString(p_); }
    BSTR get() const { return p_; }

private:
    BSTR p_;
};

VARIANT empty_variant() {
    VARIANT v;
    ::VariantInit(&v);  // VT_EMPTY
    return v;
}

// COM for this thread, and a connection to the local Task Scheduler as the current user.
class Scheduler {
public:
    Scheduler() {
        init_ = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // RPC_E_CHANGED_MODE: the thread already has COM, in another apartment; use that.
        if (FAILED(init_) && init_ != RPC_E_CHANGED_MODE) {
            status_ = hresult_error("CoInitializeEx", init_);
            return;
        }
        // As in Microsoft's Task Scheduler examples. RPC_E_TOO_LATE: the process already chose.
        HRESULT hr =
            ::CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                                   RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, 0, nullptr);
        if (FAILED(hr) && hr != RPC_E_TOO_LATE) {
            status_ = hresult_error("CoInitializeSecurity", hr);
            return;
        }
        hr = ::CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&service_));
        if (FAILED(hr)) {
            status_ = hresult_error("starting the Task Scheduler client", hr);
            return;
        }
        hr = service_->Connect(empty_variant(), empty_variant(), empty_variant(), empty_variant());
        if (FAILED(hr)) {
            status_ = hresult_error("connecting to the Task Scheduler", hr);
            return;
        }
        hr = service_->GetFolder(Bstr(L"\\").get(), &root_);
        if (FAILED(hr)) status_ = hresult_error("opening the root task folder", hr);
    }
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    ~Scheduler() {
        root_.Reset();
        service_.Reset();
        if (SUCCEEDED(init_)) ::CoUninitialize();
    }

    const core::Status& status() const { return status_; }
    ITaskFolder* root() const { return root_.Get(); }

private:
    HRESULT init_ = E_FAIL;
    core::Status status_;
    ComPtr<ITaskService> service_;
    ComPtr<ITaskFolder> root_;
};

bool not_found(HRESULT hr) {
    return hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
           hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
}

core::Status register_with(const std::string& name, const std::string& xml, LONG flags) {
    Scheduler scheduler;
    if (!scheduler.status()) return scheduler.status();
    ComPtr<IRegisteredTask> task;
    HRESULT hr = scheduler.root()->RegisterTask(
        Bstr(widen(name)).get(), Bstr(widen(xml)).get(), flags, empty_variant(), empty_variant(),
        TASK_LOGON_INTERACTIVE_TOKEN, empty_variant(), &task);
    if (FAILED(hr)) return hresult_error("registering the logon task", hr);
    return {};
}

}  // namespace

core::Result<WindowsUser> current_windows_user() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return Error{Errc::io, "cannot open this process's token"};
    }
    DWORD size = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buffer(size);
    bool ok = size > 0 && ::GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
    ::CloseHandle(token);
    if (!ok) return Error{Errc::io, "cannot read this process's user"};
    PSID sid = reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid;

    WindowsUser user;
    LPWSTR text = nullptr;
    if (!::ConvertSidToStringSidW(sid, &text)) return Error{Errc::io, "cannot format the user SID"};
    user.sid = narrow(text);
    ::LocalFree(text);

    DWORD name_size = 0;
    DWORD domain_size = 0;
    SID_NAME_USE use{};
    ::LookupAccountSidW(nullptr, sid, nullptr, &name_size, nullptr, &domain_size, &use);
    std::wstring name(name_size, L'\0');
    std::wstring domain(domain_size, L'\0');
    if (name_size == 0 || !::LookupAccountSidW(nullptr, sid, name.data(), &name_size, domain.data(),
                                               &domain_size, &use)) {
        return Error{Errc::io, "cannot look up the user's account name"};
    }
    name.resize(name_size);  // without the terminator, now that the call succeeded
    domain.resize(domain_size);
    user.name = narrow(domain) + "\\" + narrow(name);
    return user;
}

core::Status validate_task(const std::string& name, const std::string& xml) {
    return register_with(name, xml, TASK_VALIDATE_ONLY);
}

core::Status register_task(const std::string& name, const std::string& xml) {
    return register_with(name, xml, TASK_CREATE_OR_UPDATE);
}

core::Status run_task(const std::string& name) {
    Scheduler scheduler;
    if (!scheduler.status()) return scheduler.status();
    ComPtr<IRegisteredTask> task;
    HRESULT hr = scheduler.root()->GetTask(Bstr(widen(name)).get(), &task);
    if (FAILED(hr)) return hresult_error("opening the logon task", hr);
    ComPtr<IRunningTask> running;
    hr = task->Run(empty_variant(), &running);
    if (FAILED(hr)) return hresult_error("starting the logon task", hr);
    return {};
}

core::Result<bool> task_exists(const std::string& name) {
    Scheduler scheduler;
    if (!scheduler.status()) return scheduler.status().error();
    ComPtr<IRegisteredTask> task;
    HRESULT hr = scheduler.root()->GetTask(Bstr(widen(name)).get(), &task);
    if (not_found(hr)) return false;
    if (FAILED(hr)) return hresult_error("looking up the logon task", hr);
    return true;
}

core::Result<bool> delete_task(const std::string& name) {
    Scheduler scheduler;
    if (!scheduler.status()) return scheduler.status().error();
    HRESULT hr = scheduler.root()->DeleteTask(Bstr(widen(name)).get(), 0);
    if (not_found(hr)) return false;
    if (FAILED(hr)) return hresult_error("deleting the logon task", hr);
    return true;
}

}  // namespace ghostdrive::autostart
