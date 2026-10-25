// Runs before the page's own scripts, in this extension's isolated world, in every frame.
window.isolatedValue = "isolated";
window.readyStateAtStart = document.readyState;
window.pageScriptsRanBeforeStart = document.querySelectorAll("script").length;
