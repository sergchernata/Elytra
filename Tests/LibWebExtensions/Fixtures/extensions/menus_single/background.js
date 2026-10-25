// One item, created as the background starts (as 1Password does): it shows in the page's menu without a parent.
chrome.contextMenus.removeAll(() => {
    chrome.contextMenus.create({ id: "only", title: "The only item", contexts: ["all"] });
});
// Says what was chosen on its button, where the browser's UI (its self-test) can see it.
chrome.contextMenus.onClicked.addListener(info => {
    chrome.action.setTitle({ title: `Clicked ${info.menuItemId}` });
});
