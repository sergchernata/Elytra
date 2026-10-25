/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWebExtensions/MatchPattern.h>

using WebExtensions::MatchPattern;
using WebExtensions::MatchPatternSet;

static URL::URL url(StringView string)
{
    auto result = URL::Parser::basic_parse(string);
    VERIFY(result.has_value());
    return result.release_value();
}

static bool pattern_matches(StringView pattern, StringView candidate)
{
    auto parsed = MatchPattern::parse(pattern);
    if (parsed.is_error()) {
        warnln("unexpected parse failure for '{}': {}", pattern, parsed.error());
        return false;
    }
    return parsed.value().matches(url(candidate));
}

static bool host_permission_matches(StringView pattern, StringView candidate)
{
    auto parsed = MatchPattern::parse(pattern);
    if (parsed.is_error())
        return false;
    return parsed.value().matches_ignoring_path(url(candidate));
}

static bool is_valid(StringView pattern)
{
    return !MatchPattern::parse(pattern).is_error();
}

TEST_CASE(valid_patterns)
{
    EXPECT(is_valid("<all_urls>"sv));
    EXPECT(is_valid("https://*/*"sv));
    EXPECT(is_valid("https://*/"sv));
    EXPECT(is_valid("*://*/*"sv));
    EXPECT(is_valid("http://*.example.com/*"sv));
    EXPECT(is_valid("http://example.com/foo*bar"sv));
    EXPECT(is_valid("http://example.com:8080/*"sv));
    EXPECT(is_valid("http://example.com:*/*"sv));
    EXPECT(is_valid("http://*:*/*"sv));
    EXPECT(is_valid("http://127.0.0.1/*"sv));
    EXPECT(is_valid("http://[::1]/*"sv));
    EXPECT(is_valid("http://[::1]:3000/*"sv));
    EXPECT(is_valid("file:///foo*"sv));
    EXPECT(is_valid("file:///"sv));
    EXPECT(is_valid("ftp://ftp.example.com/*"sv));
    EXPECT(is_valid("ws://example.com/*"sv));
    EXPECT(is_valid("wss://*/*"sv));
    EXPECT(is_valid("HTTPS://EXAMPLE.COM/*"sv));
}

TEST_CASE(invalid_patterns)
{
    EXPECT(!is_valid(""sv));
    EXPECT(!is_valid("*"sv));
    EXPECT(!is_valid("<all_urls"sv));
    EXPECT(!is_valid("<ALL_URLS>"sv));
    EXPECT(!is_valid("http://example.com"sv));   // missing path
    EXPECT(!is_valid("http://*"sv));             // missing path
    EXPECT(!is_valid("http://www.foo.*.com/"sv)); // '*' in middle of host
    EXPECT(!is_valid("http://*foo/bar"sv));      // '*' not followed by '.'
    EXPECT(!is_valid("http://foo.*/bar"sv));
    EXPECT(!is_valid("http://google.*/*"sv));
    EXPECT(!is_valid("http://*./*"sv));
    EXPECT(!is_valid("http://**/*"sv));
    EXPECT(!is_valid("http:///*"sv)); // empty host
    EXPECT(!is_valid("http:/example.com/*"sv));
    EXPECT(!is_valid("http:example.com/*"sv));
    EXPECT(!is_valid("example.com/*"sv));
    EXPECT(!is_valid("chrome://*/*"sv));
    EXPECT(!is_valid("about:blank"sv));
    EXPECT(!is_valid("data:text/html,hi"sv));
    EXPECT(!is_valid("javascript://*/*"sv));
    EXPECT(!is_valid("foo://*/*"sv));
    EXPECT(!is_valid("://*/*"sv));
    EXPECT(!is_valid("*s://*/*"sv));
    EXPECT(!is_valid("http://example.com:/*"sv));
    EXPECT(!is_valid("http://example.com:abc/*"sv));
    EXPECT(!is_valid("http://example.com:65536/*"sv));
    EXPECT(!is_valid("http://example.com:-1/*"sv));
    EXPECT(!is_valid("http://example.com:8*/*"sv));
    EXPECT(!is_valid("http://user@example.com/*"sv));
    EXPECT(!is_valid("http://exa mple.com/*"sv));
    EXPECT(!is_valid("http://[::1/*"sv));
    EXPECT(!is_valid("http://[::1]x/*"sv));
    EXPECT(!is_valid("http://[zz]/*"sv));
    EXPECT(!is_valid("file://"sv));
    EXPECT(is_valid("file:///foo:8080/"sv)); // a colon in a file path is just path
    EXPECT(!is_valid("file://host/*"sv));
}

TEST_CASE(malformed_input_does_not_crash)
{
    StringView inputs[] = {
        "\0"sv, ":"sv, "::"sv, ":///"sv, "http"sv, "http:"sv, "http:/"sv, "http://"sv, "http://:"sv, "http://:80/"sv,
        "http://[/"sv, "http://]/"sv, "http://[]/"sv, "http://*.:80/"sv, "http://*:/"sv, "http://%/"sv, "http://%zz/"sv,
        "http://\xff\xfe/"sv, "http://xn--/"sv, "<all_urls>/"sv, "file:"sv, "file:/"sv, "*://"sv, "*://*"sv,
        "http://*.*.com/"sv, "http://a..b/"sv, "\xf0\x9f\x98\x80://x/"sv,
    };
    for (auto input : inputs)
        (void)MatchPattern::parse(input);
}

TEST_CASE(all_urls)
{
    EXPECT(pattern_matches("<all_urls>"sv, "http://example.com/"sv));
    EXPECT(pattern_matches("<all_urls>"sv, "https://example.com/a/b?c#d"sv));
    EXPECT(pattern_matches("<all_urls>"sv, "file:///etc/passwd"sv));
    EXPECT(pattern_matches("<all_urls>"sv, "ftp://example.com/x"sv));
    EXPECT(pattern_matches("<all_urls>"sv, "ws://example.com/socket"sv));
    EXPECT(pattern_matches("<all_urls>"sv, "http://localhost:8080/"sv));
    EXPECT(!pattern_matches("<all_urls>"sv, "about:blank"sv));
    EXPECT(!pattern_matches("<all_urls>"sv, "data:text/html,hi"sv));
    EXPECT(!pattern_matches("<all_urls>"sv, "chrome://settings/"sv));
    EXPECT(!pattern_matches("<all_urls>"sv, "ladybird://about/"sv));
    EXPECT(!pattern_matches("<all_urls>"sv, "javascript:alert(1)"sv));
    EXPECT(!pattern_matches("<all_urls>"sv, "chrome-extension://abc/page.html"sv));
    EXPECT(MatchPattern::parse("<all_urls>"sv).value().matches_all_urls());
}

TEST_CASE(scheme_wildcard)
{
    EXPECT(pattern_matches("*://mail.google.com/*"sv, "http://mail.google.com/x"sv));
    EXPECT(pattern_matches("*://mail.google.com/*"sv, "https://mail.google.com/x"sv));
    EXPECT(!pattern_matches("*://mail.google.com/*"sv, "ftp://mail.google.com/x"sv));
    EXPECT(!pattern_matches("*://*/*"sv, "file:///foo"sv));
    EXPECT(!pattern_matches("*://*/*"sv, "ftp://example.com/"sv));
    EXPECT(pattern_matches("*://mail.google.com/"sv, "https://mail.google.com/"sv));
    EXPECT(!pattern_matches("*://mail.google.com/"sv, "https://mail.google.com/mail"sv));
}

TEST_CASE(explicit_scheme)
{
    EXPECT(pattern_matches("http://example.com/*"sv, "http://example.com/"sv));
    EXPECT(!pattern_matches("http://example.com/*"sv, "https://example.com/"sv));
    EXPECT(!pattern_matches("https://example.com/*"sv, "http://example.com/"sv));
    EXPECT(pattern_matches("ftp://example.com/*"sv, "ftp://example.com/pub"sv));
    EXPECT(pattern_matches("HTTPS://Example.COM/*"sv, "https://example.com/"sv));
}

TEST_CASE(websocket_urls_match_as_http)
{
    EXPECT(pattern_matches("https://example.com/*"sv, "wss://example.com/socket"sv));
    EXPECT(pattern_matches("http://example.com/*"sv, "ws://example.com/socket"sv));
    EXPECT(!pattern_matches("https://example.com/*"sv, "ws://example.com/socket"sv));
    EXPECT(pattern_matches("*://example.com/*"sv, "wss://example.com/"sv));
    EXPECT(pattern_matches("wss://example.com/*"sv, "wss://example.com/"sv));
    EXPECT(!pattern_matches("wss://example.com/*"sv, "https://example.com/"sv));
}

TEST_CASE(chrome_documented_examples)
{
    // https://*/*
    EXPECT(pattern_matches("https://*/*"sv, "https://www.google.com/"sv));
    EXPECT(pattern_matches("https://*/*"sv, "https://example.org/foo/bar.html"sv));
    EXPECT(!pattern_matches("https://*/*"sv, "http://www.google.com/"sv));

    // https://*/foo*
    EXPECT(pattern_matches("https://*/foo*"sv, "https://example.com/foo/bar.html"sv));
    EXPECT(pattern_matches("https://*/foo*"sv, "https://www.google.com/foo"sv));
    EXPECT(!pattern_matches("https://*/foo*"sv, "https://example.com/bar/foo"sv));
    EXPECT(!pattern_matches("https://*/foo*"sv, "http://example.com/foo"sv));

    // https://*.google.com/foo*bar
    EXPECT(pattern_matches("https://*.google.com/foo*bar"sv, "https://www.google.com/foo/baz/bar"sv));
    EXPECT(pattern_matches("https://*.google.com/foo*bar"sv, "https://docs.google.com/foobar"sv));
    EXPECT(pattern_matches("https://*.google.com/foo*bar"sv, "https://google.com/foobar"sv));
    EXPECT(!pattern_matches("https://*.google.com/foo*bar"sv, "https://www.google.com/foo/baz/bar/qux"sv));
    EXPECT(!pattern_matches("https://*.google.com/foo*bar"sv, "https://evilgoogle.com/foobar"sv));
    EXPECT(!pattern_matches("https://*.google.com/foo*bar"sv, "https://google.com.evil.com/foobar"sv));

    // file:///foo*
    EXPECT(pattern_matches("file:///foo*"sv, "file:///foo/bar.html"sv));
    EXPECT(pattern_matches("file:///foo*"sv, "file:///foo"sv));
    EXPECT(!pattern_matches("file:///foo*"sv, "file:///bar/foo"sv));
    EXPECT(!pattern_matches("file:///foo*"sv, "http://example.com/foo"sv));

    // http://127.0.0.1/*
    EXPECT(pattern_matches("http://127.0.0.1/*"sv, "http://127.0.0.1/"sv));
    EXPECT(pattern_matches("http://127.0.0.1/*"sv, "http://127.0.0.1/foo/bar.html"sv));
    EXPECT(pattern_matches("http://127.0.0.1/*"sv, "http://127.0.0.1:8000/"sv));
    EXPECT(!pattern_matches("http://127.0.0.1/*"sv, "http://127.0.0.2/"sv));

    // http://127.0.0.1/ as a host permission matches any path.
    EXPECT(host_permission_matches("http://127.0.0.1/"sv, "http://127.0.0.1/foo/bar.html"sv));
    EXPECT(!pattern_matches("http://127.0.0.1/"sv, "http://127.0.0.1/foo/bar.html"sv));
    EXPECT(host_permission_matches("https://*/"sv, "https://example.com/a/b"sv));

    // http://localhost/* matches any port.
    EXPECT(pattern_matches("http://localhost/*"sv, "http://localhost/"sv));
    EXPECT(pattern_matches("http://localhost/*"sv, "http://localhost:3000/app"sv));
    EXPECT(!pattern_matches("http://localhost/*"sv, "https://localhost/"sv));
}

TEST_CASE(host_matching)
{
    EXPECT(pattern_matches("http://example.com/*"sv, "http://EXAMPLE.com/"sv));
    EXPECT(!pattern_matches("http://example.com/*"sv, "http://www.example.com/"sv));
    EXPECT(pattern_matches("http://*.example.com/*"sv, "http://a.b.example.com/"sv));
    EXPECT(pattern_matches("http://*.example.com/*"sv, "http://example.com/"sv));
    EXPECT(!pattern_matches("http://*.example.com/*"sv, "http://notexample.com/"sv));
    EXPECT(!pattern_matches("http://*.example.com/*"sv, "http://example.com.au/"sv));
    EXPECT(pattern_matches("http://*/*"sv, "http://192.168.0.1/"sv));
    // No subdomain matching against IP addresses.
    EXPECT(!pattern_matches("http://*.0.0.1/*"sv, "http://127.0.0.1/"sv));
    EXPECT(pattern_matches("http://[::1]/*"sv, "http://[0:0::1]:8080/x"sv));
    EXPECT(!pattern_matches("http://[::1]/*"sv, "http://127.0.0.1/"sv));
}

TEST_CASE(host_normalization)
{
    // Pattern hosts are canonicalized like URL hosts: lowercase, punycode, IPv4 forms.
    EXPECT(pattern_matches("https://BÜCHER.example/*"sv, "https://xn--bcher-kva.example/"sv));
    EXPECT(pattern_matches("https://xn--bcher-kva.example/*"sv, "https://bücher.example/"sv));
    EXPECT(pattern_matches("https://*.bücher.example/*"sv, "https://www.xn--bcher-kva.example/"sv));
    EXPECT(pattern_matches("http://0x7f.1/*"sv, "http://127.0.0.1/"sv));
    EXPECT_EQ(MatchPattern::parse("https://*.ExAmple.COM/*"sv).value().host(), "example.com"sv);
}

TEST_CASE(port_matching)
{
    EXPECT(pattern_matches("http://example.com:8080/*"sv, "http://example.com:8080/"sv));
    EXPECT(!pattern_matches("http://example.com:8080/*"sv, "http://example.com/"sv));
    EXPECT(!pattern_matches("http://example.com:8080/*"sv, "http://example.com:8081/"sv));
    EXPECT(pattern_matches("http://example.com:80/*"sv, "http://example.com/"sv));
    EXPECT(pattern_matches("https://example.com:443/*"sv, "https://example.com/"sv));
    EXPECT(pattern_matches("http://example.com:*/*"sv, "http://example.com:1234/"sv));
    EXPECT(pattern_matches("http://example.com/*"sv, "http://example.com:1234/"sv));
    EXPECT(pattern_matches("*://example.com:443/*"sv, "https://example.com/"sv));
    EXPECT(!pattern_matches("*://example.com:443/*"sv, "http://example.com/"sv));
    EXPECT(pattern_matches("http://[::1]:3000/*"sv, "http://[::1]:3000/"sv));
    EXPECT(!pattern_matches("http://[::1]:3000/*"sv, "http://[::1]/"sv));
    EXPECT(!MatchPattern::parse("http://example.com/*"sv).value().port().has_value());
    EXPECT_EQ(MatchPattern::parse("http://example.com:8080/*"sv).value().port(), 8080);
}

TEST_CASE(path_and_query)
{
    // The path glob covers path + "?" + query; '?' in a pattern path is literal.
    EXPECT(pattern_matches("https://www.director.ai/?*"sv, "https://www.director.ai/?ref=1"sv));
    EXPECT(pattern_matches("https://www.director.ai/?*"sv, "https://www.director.ai/?"sv));
    EXPECT(!pattern_matches("https://www.director.ai/?*"sv, "https://www.director.ai/"sv));
    EXPECT(!pattern_matches("https://www.director.ai/?*"sv, "https://www.director.ai/x"sv));
    EXPECT(pattern_matches("https://www.director.ai/"sv, "https://www.director.ai/"sv));
    EXPECT(pattern_matches("https://www.director.ai/"sv, "https://www.director.ai/#frag"sv));
    EXPECT(!pattern_matches("https://www.director.ai/"sv, "https://www.director.ai/?a=b"sv));
    EXPECT(!pattern_matches("https://www.director.ai/"sv, "https://www.director.ai/app"sv));
    EXPECT(pattern_matches("http://example.com/*?q=1"sv, "http://example.com/search?q=1"sv));
    EXPECT(pattern_matches("http://example.com/a*b*c"sv, "http://example.com/aXXbYYc"sv));
    EXPECT(!pattern_matches("http://example.com/a*b*c"sv, "http://example.com/aXXcYYb"sv));
    EXPECT(pattern_matches("http://example.com/*"sv, "http://example.com/a#b"sv));
    EXPECT(!pattern_matches("http://example.com/a#b"sv, "http://example.com/a#b"sv));
    EXPECT(pattern_matches("http://example.com/***"sv, "http://example.com/"sv));
    EXPECT(pattern_matches("http://example.com/%20*"sv, "http://example.com/ x"sv));
}

TEST_CASE(onepassword_patterns)
{
    EXPECT(pattern_matches("https://*/*"sv, "https://my.1password.com/signin"sv));
    EXPECT(!pattern_matches("https://*/*"sv, "http://my.1password.com/signin"sv));

    EXPECT(pattern_matches("http://localhost/*"sv, "http://localhost:4000/"sv));
    EXPECT(!pattern_matches("http://localhost/*"sv, "http://localhost.evil.com/"sv));

    EXPECT(pattern_matches("https://*.1password.com/*"sv, "https://1password.com/"sv));
    EXPECT(pattern_matches("https://*.1password.com/*"sv, "https://my.1password.com/vaults"sv));
    EXPECT(pattern_matches("https://*.1password.com/*"sv, "https://a.b.1password.com/"sv));
    EXPECT(!pattern_matches("https://*.1password.com/*"sv, "https://fake1password.com/"sv));
    EXPECT(!pattern_matches("https://*.1password.com/*"sv, "https://1password.com.evil.io/"sv));
    EXPECT(!pattern_matches("https://*.1password.com/*"sv, "http://my.1password.com/"sv));

    EXPECT(pattern_matches("https://app.kolide.com/*"sv, "https://app.kolide.com/x/y?z"sv));
    EXPECT(!pattern_matches("https://app.kolide.com/*"sv, "https://kolide.com/"sv));

    EXPECT(pattern_matches("https://autofill.me/*"sv, "https://autofill.me/form/login"sv));
    EXPECT(!pattern_matches("https://autofill.me/*"sv, "https://www.autofill.me/"sv));

    EXPECT(pattern_matches("<all_urls>"sv, "https://example.com/"sv));
}

TEST_CASE(matches_origin)
{
    auto pattern = TRY_OR_FAIL(MatchPattern::parse("https://*.example.com/only/this/path"sv));
    EXPECT(pattern.matches_origin(url("https://www.example.com/other"sv).origin()));
    EXPECT(!pattern.matches_origin(url("http://www.example.com/"sv).origin()));
    EXPECT(!pattern.matches_origin(url("data:text/html,x"sv).origin()));

    auto with_port = TRY_OR_FAIL(MatchPattern::parse("http://localhost:8080/*"sv));
    EXPECT(with_port.matches_origin(url("http://localhost:8080/"sv).origin()));
    EXPECT(!with_port.matches_origin(url("http://localhost:9090/"sv).origin()));
}

TEST_CASE(pattern_set)
{
    Vector<String> patterns { "https://a.com/*"_string, "http://*.b.com/x*"_string };
    auto set = TRY_OR_FAIL(MatchPatternSet::parse(patterns));
    EXPECT_EQ(set.patterns().size(), 2u);
    EXPECT(set.matches(url("https://a.com/"sv)));
    EXPECT(set.matches(url("http://c.b.com/xyz"sv)));
    EXPECT(!set.matches(url("http://c.b.com/y"sv)));
    EXPECT(set.matches_ignoring_path(url("http://c.b.com/y"sv)));
    EXPECT(set.matches_origin(url("http://c.b.com/y"sv).origin()));
    EXPECT(!set.matches(url("https://c.com/"sv)));

    Vector<String> bad { "https://a.com/*"_string, "nope"_string };
    EXPECT(MatchPatternSet::parse(bad).is_error());

    MatchPatternSet empty;
    EXPECT(empty.is_empty());
    EXPECT(!empty.matches(url("https://a.com/"sv)));
}

TEST_CASE(globs)
{
    using WebExtensions::glob_matches;
    EXPECT(glob_matches("*"sv, ""sv));
    EXPECT(glob_matches("*"sv, "anything"sv));
    EXPECT(glob_matches(""sv, ""sv));
    EXPECT(!glob_matches(""sv, "a"sv));
    EXPECT(glob_matches("abc"sv, "abc"sv));
    EXPECT(!glob_matches("abc"sv, "abcd"sv));
    EXPECT(!glob_matches("abc"sv, "xabc"sv));
    EXPECT(glob_matches("a?c"sv, "abc"sv));
    EXPECT(!glob_matches("a?c"sv, "ac"sv));
    EXPECT(glob_matches("https://???.example.com/foo/*"sv, "https://www.example.com/foo/bar"sv));
    EXPECT(!glob_matches("https://???.example.com/foo/*"sv, "https://the.example.com/foo"sv));
    EXPECT(!glob_matches("https://???.example.com/foo/*"sv, "https://ww.example.com/foo/bar"sv));
    EXPECT(glob_matches("*nytimes.com/???s/*"sv, "https://www.nytimes.com/arts/index.html"sv));
    EXPECT(glob_matches("*nytimes.com/???s/*"sv, "http://www.nytimes.com/jobs/index.html"sv));
    EXPECT(!glob_matches("*nytimes.com/???s/*"sv, "https://www.nytimes.com/sports/index.html"sv));
    EXPECT(glob_matches("*a*b*c*"sv, "xxaxxbxxcxx"sv));
    EXPECT(!glob_matches("*a*b*c*"sv, "xxcxxbxxaxx"sv));
    EXPECT(glob_matches("?"sv, "\xc3\xbc"sv)); // one code point
    // Pathological input must finish quickly.
    EXPECT(!glob_matches("*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*b"sv, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"sv));
}

TEST_CASE(content_script_combination)
{
    using WebExtensions::should_inject_content_script;
    Vector<String> m { "https://*.nytimes.com/*"_string };
    Vector<String> em { "*://*/*business*"_string };
    auto matches = TRY_OR_FAIL(MatchPatternSet::parse(m));
    auto exclude_matches = TRY_OR_FAIL(MatchPatternSet::parse(em));
    MatchPatternSet none;
    Vector<String> no_globs;
    Vector<String> include_globs { "*nytimes.com/???s/*"_string };
    Vector<String> exclude_globs { "*science*"_string };

    EXPECT(should_inject_content_script(url("https://www.nytimes.com/arts/index.html"sv), matches, none, no_globs, no_globs));
    EXPECT(!should_inject_content_script(url("https://www.example.com/"sv), matches, none, no_globs, no_globs));
    EXPECT(!should_inject_content_script(url("https://www.nytimes.com/business/x"sv), matches, exclude_matches, no_globs, no_globs));
    EXPECT(should_inject_content_script(url("https://www.nytimes.com/arts/x"sv), matches, exclude_matches, include_globs, no_globs));
    EXPECT(!should_inject_content_script(url("https://www.nytimes.com/sports/x"sv), matches, exclude_matches, include_globs, no_globs));
    EXPECT(!should_inject_content_script(url("https://www.nytimes.com/arts/science"sv), matches, none, include_globs, exclude_globs));
    EXPECT(should_inject_content_script(url("https://www.nytimes.com/sports/x"sv), matches, none, no_globs, exclude_globs));
    // Globs alone never widen the match set.
    EXPECT(!should_inject_content_script(url("https://www.nytimes.org/arts/x"sv), matches, none, include_globs, no_globs));
    EXPECT(!should_inject_content_script(url("https://www.nytimes.com/"sv), none, none, no_globs, no_globs));
}

TEST_CASE(url_for_matching)
{
    using WebExtensions::url_for_matching;
    auto page = url("https://example.com/page?x#y"sv);
    auto parent = url("https://parent.com/dir/index.html"sv);

    // Ordinary URLs are used as-is regardless of flags.
    EXPECT_EQ(url_for_matching(page, parent, false, false), page);
    EXPECT_EQ(url_for_matching(page, {}, true, true), page);

    // about:blank / about:srcdoc
    auto blank = url("about:blank"sv);
    auto srcdoc = url("about:srcdoc"sv);
    EXPECT(!url_for_matching(blank, parent, false, false).has_value());
    EXPECT_EQ(url_for_matching(blank, parent, true, false), parent);
    EXPECT_EQ(url_for_matching(srcdoc, parent, true, false), parent);
    EXPECT_EQ(url_for_matching(blank, parent, false, true), url("https://parent.com/"sv));
    EXPECT_EQ(url_for_matching(blank, parent, true, true), url("https://parent.com/"sv));
    EXPECT(!url_for_matching(blank, {}, true, true).has_value());
    EXPECT(!url_for_matching(blank, url("data:text/html,x"sv), true, true).has_value());

    // data: / blob: / filesystem: only with match_origin_as_fallback.
    auto data = url("data:text/html,hello"sv);
    EXPECT(!url_for_matching(data, parent, true, false).has_value());
    EXPECT_EQ(url_for_matching(data, parent, false, true), url("https://parent.com/"sv));
    EXPECT(!url_for_matching(data, {}, false, true).has_value());

    auto blob = url("blob:https://blobhost.com:8443/1234-5678"sv);
    EXPECT(!url_for_matching(blob, parent, true, false).has_value());
    EXPECT_EQ(url_for_matching(blob, parent, false, true), url("https://blobhost.com:8443/"sv));
    EXPECT_EQ(url_for_matching(blob, {}, false, true), url("https://blobhost.com:8443/"sv));

    auto filesystem = url("filesystem:https://fs.com/temporary/x"sv);
    EXPECT(!url_for_matching(filesystem, parent, false, false).has_value());
    EXPECT_EQ(url_for_matching(filesystem, parent, false, true), url("https://parent.com/"sv));

    // Other about: pages are never matched through a fallback.
    EXPECT(!url_for_matching(url("about:settings"sv), parent, true, true).has_value());

    // The fallback URL matches "/*" patterns but not path-specific ones.
    auto fallback = url_for_matching(blank, parent, false, true).value();
    EXPECT(TRY_OR_FAIL(MatchPattern::parse("https://parent.com/*"sv)).matches(fallback));
    EXPECT(!TRY_OR_FAIL(MatchPattern::parse("https://parent.com/dir/*"sv)).matches(fallback));
}

TEST_CASE(restricted_urls)
{
    using WebExtensions::is_restricted_url;
    EXPECT(!is_restricted_url(url("https://example.com/"sv), false));
    EXPECT(!is_restricted_url(url("http://localhost:8080/"sv), false));
    EXPECT(!is_restricted_url(url("ws://example.com/"sv), false));
    EXPECT(!is_restricted_url(url("ftp://example.com/"sv), false));
    EXPECT(is_restricted_url(url("file:///etc/hosts"sv), false));
    EXPECT(!is_restricted_url(url("file:///etc/hosts"sv), true));

    EXPECT(is_restricted_url(url("about:blank"sv), true));
    EXPECT(is_restricted_url(url("about:settings"sv), true));
    EXPECT(is_restricted_url(url("ladybird://settings"sv), true));
    EXPECT(is_restricted_url(url("chrome://extensions/"sv), true));
    EXPECT(is_restricted_url(url("view-source:https://example.com/"sv), true));
    EXPECT(is_restricted_url(url("devtools://devtools/bundled/x.html"sv), true));
    EXPECT(is_restricted_url(url("data:text/html,x"sv), true));
    EXPECT(is_restricted_url(url("javascript:alert(1)"sv), true));
    EXPECT(is_restricted_url(url("blob:https://example.com/uuid"sv), true));
    EXPECT(is_restricted_url(url("someapp://x"sv), true));

    EXPECT(is_restricted_url(url("chrome-extension://otherextensionid/page.html"sv), true));
    EXPECT(is_restricted_url(url("chrome-extension://otherextensionid/page.html"sv), true, "myextensionid"sv));
    EXPECT(!is_restricted_url(url("chrome-extension://myextensionid/page.html"sv), true, "myextensionid"sv));

    EXPECT(is_restricted_url(url("https://chromewebstore.google.com/detail/x"sv), true));
    EXPECT(is_restricted_url(url("https://CHROMEWEBSTORE.google.com/"sv), true));
    EXPECT(is_restricted_url(url("https://chrome.google.com/webstore/detail/x"sv), true));
    EXPECT(is_restricted_url(url("https://chrome.google.com/webstore"sv), true));
    EXPECT(!is_restricted_url(url("https://chrome.google.com/other"sv), true));
    EXPECT(!is_restricted_url(url("https://chrome.google.com/webstorefront"sv), true));
}
