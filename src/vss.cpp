#include "vss.h"

#include "util.h"

#include <windows.h>

#include <objbase.h>
#include <vss.h>
#include <vsserror.h>
#include <vswriter.h>
#include <vsbackup.h>

#include <algorithm>
#include <format>

namespace tc {

namespace {

bool fail(std::wstring& error, const wchar_t* what, HRESULT hr) {
    const wchar_t* why = L"";
    switch (hr) {
    case VSS_E_VOLUME_NOT_SUPPORTED:
        why = L": the volume does not support snapshots (e.g. removable or FAT)";
        break;
    case VSS_E_PROVIDER_VETO:
        why = L": the snapshot provider vetoed the operation";
        break;
    case VSS_E_SNAPSHOT_SET_IN_PROGRESS:
        why = L": another snapshot set is being created; retry later";
        break;
    case VSS_E_INSUFFICIENT_STORAGE:
        why = L": not enough free space for the snapshot";
        break;
    }
    error = std::format(L"{} failed (hr=0x{:08X}){}", what,
                        static_cast<unsigned long>(hr), why);
    return false;
}

// Runs a VSS async operation to completion.
bool wait_async(IVssAsync* async, const wchar_t* what, std::wstring& error) {
    if (!async) return fail(error, what, E_POINTER);
    HRESULT hr = async->Wait();
    if (SUCCEEDED(hr)) {
        HRESULT status = S_OK;
        hr = async->QueryStatus(&status, nullptr);
        if (SUCCEEDED(hr)) hr = status;
    }
    async->Release();
    if (hr != VSS_S_ASYNC_FINISHED) return fail(error, what, hr);
    return true;
}

template <typename F>
bool run_async(IVssBackupComponents* comp, F&& start, const wchar_t* what,
               std::wstring& error) {
    IVssAsync* async = nullptr;
    const HRESULT hr = start(comp, &async);
    if (FAILED(hr)) return fail(error, what, hr);
    return wait_async(async, what, error);
}

} // namespace

struct VssSnapshotSet::Impl {
    bool com_init = false;
    IVssBackupComponents* comp = nullptr;
    bool writers = false; // writer-involved session; owes BackupComplete

    ~Impl() {
        if (comp) {
            if (writers) {
                std::wstring ignored;
                run_async(comp, [](IVssBackupComponents* c, IVssAsync** a) {
                    return c->BackupComplete(a);
                }, L"BackupComplete", ignored);
            }
            comp->Release(); // non-persistent snapshots are deleted here
        }
        if (com_init) ::CoUninitialize();
    }
};

VssSnapshotSet::~VssSnapshotSet() { delete impl_; }

bool VssSnapshotSet::create(const std::vector<std::wstring>& volumes,
                            std::vector<std::wstring>& devices, std::wstring& error) {
    devices.assign(volumes.size(), std::wstring{});
    if (volumes.empty()) {
        error = L"no volumes to snapshot";
        return false;
    }

    impl_ = new Impl;
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) impl_->com_init = true;
    else if (hr != RPC_E_CHANGED_MODE) return fail(error, L"CoInitializeEx", hr);
    // Recommended for VSS; harmless if some other component set it already.
    ::CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                           RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);

    std::vector<VSS_ID> snap_ids(volumes.size(), VSS_ID{});
    std::vector<bool> included(volumes.size(), false);

    // First with writers (application-consistent), then the plain file-share
    // context (no writers) when any writer stage fails.
    for (const bool with_writers : {true, false}) {
        if (impl_->comp) {
            impl_->comp->Release();
            impl_->comp = nullptr;
            impl_->writers = false;
        }
        hr = ::CreateVssBackupComponents(&impl_->comp);
        if (FAILED(hr)) return fail(error, L"CreateVssBackupComponents", hr);
        IVssBackupComponents* comp = impl_->comp;

        std::wstring why;
        bool ok = SUCCEEDED(hr = comp->InitializeForBackup());
        if (!ok) fail(why, L"InitializeForBackup", hr);
        if (ok && !with_writers) {
            ok = SUCCEEDED(hr = comp->SetContext(VSS_CTX_FILE_SHARE_BACKUP));
            if (!ok) fail(why, L"SetContext", hr);
        }
        if (ok && with_writers) {
            ok = SUCCEEDED(hr = comp->SetBackupState(false, false, VSS_BT_COPY, false));
            if (!ok) fail(why, L"SetBackupState", hr);
            if (ok)
                ok = run_async(comp, [](IVssBackupComponents* c, IVssAsync** a) {
                    return c->GatherWriterMetadata(a);
                }, L"GatherWriterMetadata", why);
            if (ok) comp->FreeWriterMetadata();
        }

        VSS_ID set_id{};
        if (ok) {
            ok = SUCCEEDED(hr = comp->StartSnapshotSet(&set_id));
            if (!ok) fail(why, L"StartSnapshotSet", hr);
        }

        size_t added = 0;
        if (ok) {
            for (size_t i = 0; i < volumes.size(); ++i) {
                std::wstring name = volumes[i];
                hr = comp->AddToSnapshotSet(name.data(), VSS_ID{}, &snap_ids[i]);
                included[i] = SUCCEEDED(hr);
                if (included[i]) ++added;
            }
            if (added == 0) {
                ok = false;
                fail(why, L"AddToSnapshotSet", hr);
            }
        }

        if (ok && with_writers)
            ok = run_async(comp, [](IVssBackupComponents* c, IVssAsync** a) {
                return c->PrepareForBackup(a);
            }, L"PrepareForBackup", why);
        if (ok)
            ok = run_async(comp, [](IVssBackupComponents* c, IVssAsync** a) {
                return c->DoSnapshotSet(a);
            }, L"DoSnapshotSet", why);

        if (ok) {
            impl_->writers = with_writers;
            if (!with_writers)
                log_info(L"vss: writer-involved snapshot failed (" + error +
                         L"); using a writerless snapshot");
            break;
        }
        error = why;
        if (!with_writers) return false; // both attempts failed
        std::fill(included.begin(), included.end(), false);
    }

    for (size_t i = 0; i < volumes.size(); ++i) {
        if (!included[i]) continue;
        VSS_SNAPSHOT_PROP prop{};
        hr = impl_->comp->GetSnapshotProperties(snap_ids[i], &prop);
        if (FAILED(hr)) continue; // volume stays live-read
        if (prop.m_pwszSnapshotDeviceObject) devices[i] = prop.m_pwszSnapshotDeviceObject;
        ::VssFreeSnapshotProperties(&prop);
    }
    error.clear();
    return true;
}

} // namespace tc
