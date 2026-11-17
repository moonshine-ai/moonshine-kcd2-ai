#include "secrets.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "child.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <wincred.h>
#else
#include <sys/stat.h>
#endif

namespace moonshine::gui {

#ifdef _WIN32

namespace {

std::wstring wide_name() {
    const std::string name = kApiKeyName;
    return std::wstring(name.begin(), name.end());
}

class CredentialManager : public SecretStore {
public:
    std::string get() override {
        PCREDENTIALW found = nullptr;
        if (!CredReadW(wide_name().c_str(), CRED_TYPE_GENERIC, 0, &found)) return {};
        std::string key(reinterpret_cast<const char*>(found->CredentialBlob), found->CredentialBlobSize);
        CredFree(found);
        return key;
    }

    bool set(const std::string& key, std::string* trouble) override {
        std::wstring name = wide_name();
        if (key.empty()) {
            if (CredDeleteW(name.c_str(), CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND) return true;
            if (trouble) *trouble = "Windows would not remove the key (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        std::wstring user = L"Moonshine";
        std::wstring comment = L"The key Moonshine sends to the model API chosen in its Settings.";
        CREDENTIALW entry{};
        entry.Type = CRED_TYPE_GENERIC;
        entry.TargetName = name.data();
        entry.Comment = comment.data();
        entry.UserName = user.data();
        entry.CredentialBlobSize = static_cast<DWORD>(key.size());
        entry.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(key.data()));
        // This account on this PC: not carried to other machines with a roaming profile.
        entry.Persist = CRED_PERSIST_LOCAL_MACHINE;
        if (CredWriteW(&entry, 0)) return true;
        if (trouble) *trouble = "Windows would not keep the key (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
};

}  // namespace

std::unique_ptr<SecretStore> system_secrets() { return std::make_unique<CredentialManager>(); }

#else

namespace {

class OwnerOnlyFile : public SecretStore {
public:
    explicit OwnerOnlyFile(std::filesystem::path path) : path_(std::move(path)) {}

    std::string get() override {
        std::ifstream in(path_, std::ios::binary);
        if (!in) return {};
        std::ostringstream text;
        text << in.rdbuf();
        std::string key = text.str();
        while (!key.empty() && (key.back() == '\n' || key.back() == '\r')) key.pop_back();
        return key;
    }

    bool set(const std::string& key, std::string* trouble) override {
        std::error_code ec;
        if (key.empty()) {
            std::filesystem::remove(path_, ec);
            return true;
        }
        std::filesystem::create_directories(path_.parent_path(), ec);
        // Made empty and closed to everyone else before anything is written into it.
        { std::ofstream made(path_, std::ios::binary | std::ios::trunc); }
        ::chmod(path_.c_str(), S_IRUSR | S_IWUSR);
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        out << key;
        out.flush();
        if (out) return true;
        if (trouble) *trouble = "could not write " + path_.string();
        return false;
    }

private:
    std::filesystem::path path_;
};

}  // namespace

std::unique_ptr<SecretStore> system_secrets() {
    std::filesystem::path base = environment("XDG_CONFIG_HOME");
    if (base.empty()) base = std::filesystem::path(environment("HOME")) / ".config";
    return std::make_unique<OwnerOnlyFile>(base / "moonshine" / "api-key");
}

#endif

}  // namespace moonshine::gui
