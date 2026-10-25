// Records the events the tests look for, under the extension's version, and counts its starts.
const version = chrome.runtime.getManifest().version;
const events = [];
const record = (name, details) => {
    events.push({ [name]: details });
    chrome.storage.local.set({ [`events_${version}`]: events });
};
chrome.runtime.onInstalled.addListener(details => record("onInstalled", details));
chrome.runtime.onStartup.addListener(() => record("onStartup", {}));
chrome.permissions.onAdded.addListener(permissions => record("permissions.onAdded", permissions));
chrome.permissions.onRemoved.addListener(permissions => record("permissions.onRemoved", permissions));
chrome.management.onInstalled.addListener(info => record("management.onInstalled", info.id));
chrome.management.onEnabled.addListener(info => record("management.onEnabled", info.id));
chrome.management.onDisabled.addListener(info => record("management.onDisabled", info.id));
chrome.privacy.services.passwordSavingEnabled.onChange.addListener(details => record("privacy.onChange", details));
chrome.storage.local.get("starts").then(({ starts }) => chrome.storage.local.set({ starts: (starts || 0) + 1 }));
