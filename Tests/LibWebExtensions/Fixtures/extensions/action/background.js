// Notes clicks and commands where the test's page can read them.
chrome.action.onClicked.addListener(tab => {
    chrome.storage.session.set({ clicked: tab?.id ?? "no tab" });
});
chrome.commands.onCommand.addListener((command, tab) => {
    chrome.storage.session.set({ command, command_tab: tab?.id ?? "no tab" });
});
