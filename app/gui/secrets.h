// Where the key to a model API is kept.
//
// Not in moonshine.cfg: Collect-Logs.ps1 copies that file into the zip a player sends
// when something is wrong, and a key is money. On Windows it is in Credential Manager,
// under the player's own account, where they can see and remove it themselves ("Windows
// Credentials", the entry named below). It leaves there only in the environment of the
// engine this window starts, and is never written to a log or a command line.
//
// The Mac and Linux builds exist for working on the window, not for players; there the
// key is a file only its owner can read, under the user's own configuration folder.

#ifndef MOONSHINE_GUI_SECRETS_H
#define MOONSHINE_GUI_SECRETS_H

#include <memory>
#include <string>

namespace moonshine::gui {

// The name the key is kept under in Credential Manager.
inline constexpr const char* kApiKeyName = "Moonshine: model API key";

class SecretStore {
public:
    virtual ~SecretStore() = default;
    // Empty when there is none.
    virtual std::string get() = 0;
    // An empty key removes what is kept. False, with the reason, when it could not be done.
    virtual bool set(const std::string& key, std::string* trouble = nullptr) = 0;
};

// The system's store.
std::unique_ptr<SecretStore> system_secrets();

// One that forgets when the program ends, for tests.
class MemorySecrets : public SecretStore {
public:
    std::string get() override { return key_; }
    bool set(const std::string& key, std::string*) override {
        key_ = key;
        return true;
    }

private:
    std::string key_;
};

}  // namespace moonshine::gui

#endif  // MOONSHINE_GUI_SECRETS_H
