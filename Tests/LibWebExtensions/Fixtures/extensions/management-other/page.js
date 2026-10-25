const management_id = "epcdjeeimbldakmnfpideclgjeiflopp";

steps.management = async () => {
    await attempt("all", async () => (await chrome.management.getAll()).map(info => info.id).join(","));
    await attempt("self", async () => (await chrome.management.getSelf()).id);
    await attempt("get_other", () => chrome.management.get(management_id));
    await attempt("disable_other", () => chrome.management.setEnabled(management_id, false));
    await attempt("options", () => chrome.runtime.openOptionsPage());
};

steps.privacy = async () => {
    await attempt("before", privacyState);
    await attempt("set", () => privacy.set({ value: true }));
    await attempt("after", privacyState);
};

steps.privacy_clear = async () => {
    await attempt("clear", () => privacy.clear({}));
    await attempt("bad_clear", () => privacy.clear("x"));
    await attempt("after", privacyState);
};
