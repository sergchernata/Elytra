// Marks the frames it runs in, where the browser sees it: in their URL's fragment.
if (window !== top)
    location.hash = "content-script-ran";
