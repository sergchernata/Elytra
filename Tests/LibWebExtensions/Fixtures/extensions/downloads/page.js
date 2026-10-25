// What a page of the extension can do with downloads. ?step=… picks what to do; the report is JSON in the title.
(async () => {
    const params = new URLSearchParams(location.search);
    const step = params.get("step");
    const report = {};
    const attempt = async (name, fn) => {
        try {
            report[name] = await fn();
        } catch (e) {
            report[name] = `error: ${e.message}`;
        }
    };
    const until = async condition => {
        for (let tries = 0; tries < 100 && !(await condition()); ++tries)
            await new Promise(resolve => setTimeout(resolve, 50));
    };
    const complete = async id => {
        await until(async () => (await chrome.downloads.search({ id }))[0]?.state !== "in_progress");
        return (await chrome.downloads.search({ id }))[0];
    };
    if (step === "download") {
        await attempt("data", () => chrome.downloads.download({ url: "data:text/plain;base64,SGVsbG8=", filename: "hello.txt" }));
        // As 1Password saves a file: a data: URL of its own making.
        await attempt("attachment", () => chrome.downloads.download({ url: "data:attachment/file;base64,eyJhIjoxfQ==", filename: "export.json" }));
        await attempt("subdirectory", () => chrome.downloads.download({ url: "data:,In%20a%20folder", filename: "sub/dir/note.txt" }));
        await attempt("blob", () => chrome.downloads.download({ url: URL.createObjectURL(new Blob(["From a blob"], { type: "text/plain" })), filename: "blob.txt" }));
        await attempt("own_file", () => chrome.downloads.download({ url: chrome.runtime.getURL("file.txt") }));
        await attempt("uniquified", () => chrome.downloads.download({ url: "data:,Again", filename: "hello.txt" }));
        await attempt("overwritten", () => chrome.downloads.download({ url: "data:,Bye", filename: "hello.txt", conflictAction: "overwrite" }));
        await attempt("web", () => chrome.downloads.download({ url: params.get("server"), filename: "web.txt" }));
        await attempt("callback", () => new Promise(resolve => chrome.downloads.download({ url: "data:,cb", filename: "cb.txt" }, id => resolve(id))));

        await attempt("escaping", () => chrome.downloads.download({ url: "data:,x", filename: "../escape.txt" }));
        await attempt("escaping_inside", () => chrome.downloads.download({ url: "data:,x", filename: "sub/../../escape.txt" }));
        await attempt("through_link", () => chrome.downloads.download({ url: "data:,x", filename: "link/escape.txt", conflictAction: "overwrite" }));
        await attempt("absolute", () => chrome.downloads.download({ url: "data:,x", filename: "/tmp/absolute.txt" }));
        await attempt("backslash", () => chrome.downloads.download({ url: "data:,x", filename: "..\\escape.txt" }));
        await attempt("empty_filename", () => chrome.downloads.download({ url: "data:,x", filename: "" }));
        await attempt("hidden_folder", () => chrome.downloads.download({ url: "data:,x", filename: "sub/./x.txt" }));
        await attempt("bad_filename", () => chrome.downloads.download({ url: "data:,x", filename: 42 }));
        await attempt("no_url", () => chrome.downloads.download({ filename: "x.txt" }));
        await attempt("bad_url", () => chrome.downloads.download({ url: "not a url" }));
        await attempt("javascript_url", () => chrome.downloads.download({ url: "javascript:alert(1)" }));
        await attempt("file_url", () => chrome.downloads.download({ url: "file:///etc/passwd" }));
        await attempt("other_extension", () => chrome.downloads.download({ url: "chrome-extension://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/file.txt" }));
        await attempt("missing_file", () => chrome.downloads.download({ url: chrome.runtime.getURL("nothing.txt") }));
        await attempt("bad_conflict_action", () => chrome.downloads.download({ url: "data:,x", filename: "x.txt", conflictAction: "explode" }));
        await attempt("post", () => chrome.downloads.download({ url: params.get("server"), method: "POST", body: "x" }));
        await attempt("headers", () => chrome.downloads.download({ url: params.get("server"), headers: [{ name: "X-Test", value: "1" }] }));
        await attempt("bad_options", () => chrome.downloads.download("data:,x"));

        const data = await complete(report.data);
        report.data_state = data?.state;
        report.data_filename = data?.filename;
        report.data_mime = data?.mime;
        report.data_by = data?.byExtensionId;
        report.data_exists = data?.exists;
        report.data_size = data?.fileSize;
        for (const key of ["attachment", "subdirectory", "blob", "own_file", "uniquified", "overwritten", "web", "callback"])
            report[`${key}_filename`] = (await complete(report[key]))?.filename;
        report.web_state = (await complete(report.web))?.state;
        await attempt("search_all", async () => (await chrome.downloads.search({})).length);
        await attempt("search_query", async () => (await chrome.downloads.search({ query: ["export"] })).map(item => item.id).join(","));
        await attempt("search_state", async () => (await chrome.downloads.search({ state: "complete", limit: 2 })).length);
        await attempt("search_missing", async () => (await chrome.downloads.search({ id: 123456 })).length);
        await attempt("search_bad", () => chrome.downloads.search("everything"));
        await attempt("pause_complete", () => chrome.downloads.pause(report.data));
        await attempt("resume_complete", () => chrome.downloads.resume(report.data));
        await attempt("cancel_complete", async () => (await chrome.downloads.cancel(report.data), "canceled"));
        await attempt("pause_missing", () => chrome.downloads.pause(123456));
        await attempt("show", async () => (await chrome.downloads.show(report.data), "shown"));
        await attempt("open_without_permission", () => chrome.downloads.open(report.data));
        await attempt("erase", () => chrome.downloads.erase({ id: report.data }));
        await attempt("remove_file", async () => (await chrome.downloads.removeFile(report.callback), "removed"));
        await attempt("remove_file_again", async () => (await chrome.downloads.removeFile(report.callback), "removed"));
        await attempt("callback_exists", async () => (await chrome.downloads.search({ id: report.callback }))[0]?.exists);
        await attempt("file_icon", () => chrome.downloads.getFileIcon(report.data));
    } else if (step === "open") {
        // With the "downloads.open" permission: opening needs the user's gesture.
        const id = await chrome.downloads.download({ url: "data:,Open%20me", filename: "open-me.txt" });
        await complete(id);
        await attempt("open", async () => (await chrome.downloads.open(id), "opened"));
        await attempt("open_missing", () => chrome.downloads.open(123456));
        await attempt("open_bad", () => chrome.downloads.open("x"));
    } else {
        // Wait for the events.
        const wanted = Number(params.get("events") ?? "1");
        await until(async () => ((await chrome.storage.session.get("events")).events ?? []).length >= wanted);
        report.events = JSON.stringify((await chrome.storage.session.get("events")).events ?? []);
    }
    document.title = JSON.stringify(report);
})();
