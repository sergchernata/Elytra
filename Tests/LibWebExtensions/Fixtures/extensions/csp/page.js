// What an extension page may run under its CSP: the declared policy, and Chrome's minimum on top of it.
(async () => {
    const report = { inline: window.inlineRan === true };
    try {
        report.eval = eval("1 + 1");
    } catch (e) {
        report.eval = "blocked";
    }
    try {
        // The smallest module: "\0asm" and version 1.
        await WebAssembly.compile(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]));
        report.wasm = "compiled";
    } catch (e) {
        report.wasm = `blocked: ${e.message}`;
    }
    const image = document.getElementById("image");
    await new Promise(resolve => image.complete ? resolve() : (image.onload = image.onerror = resolve));
    report.image = image.naturalWidth > 0 ? "loaded" : "blocked";
    document.title = JSON.stringify(report);
})();
