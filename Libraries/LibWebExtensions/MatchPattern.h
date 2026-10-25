/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibURL/Forward.h>

namespace WebExtensions {

// A Chrome extension match pattern, e.g. "https://*.example.com/foo*".
// https://developer.chrome.com/docs/extensions/develop/concepts/match-patterns
class MatchPattern {
public:
    static ErrorOr<MatchPattern> parse(StringView);

    // Matches scheme, host, port and path. The path glob is tested against the
    // URL's path plus "?query" when the URL has a query. The fragment is ignored.
    bool matches(URL::URL const&) const;

    // Like matches(), but any path matches. Used for host permissions, where
    // Chrome requires a path in the pattern but ignores it.
    bool matches_ignoring_path(URL::URL const&) const;
    bool matches_origin(URL::Origin const&) const;

    bool matches_all_urls() const { return m_all_urls; }
    String const& scheme() const { return m_scheme; }
    String const& host() const { return m_host; }
    bool matches_subdomains() const { return m_match_subdomains; }
    Optional<u16> port() const { return m_port; }
    String const& path() const { return m_path; }
    String const& to_string() const { return m_source; }

private:
    MatchPattern() = default;

    bool matches_impl(URL::URL const&, bool check_path) const;
    bool matches_scheme(StringView) const;
    bool matches_host(URL::URL const&) const;

    String m_source;
    String m_scheme;
    String m_host;
    String m_path;
    Optional<u16> m_port;
    bool m_all_urls { false };
    bool m_match_subdomains { false };
};

class MatchPatternSet {
public:
    // Fails on the first invalid pattern.
    static ErrorOr<MatchPatternSet> parse(ReadonlySpan<String>);

    void add(MatchPattern pattern) { m_patterns.append(move(pattern)); }

    bool matches(URL::URL const&) const;
    bool matches_ignoring_path(URL::URL const&) const;
    bool matches_origin(URL::Origin const&) const;

    bool is_empty() const { return m_patterns.is_empty(); }
    Vector<MatchPattern> const& patterns() const { return m_patterns; }

private:
    Vector<MatchPattern> m_patterns;
};

// Glob over a whole string: '*' matches any run of characters, '?' exactly one.
bool glob_matches(StringView glob, StringView string);

// Chrome's content script rule: matches AND NOT exclude_matches AND
// (no include_globs OR any include glob) AND NOT any exclude glob.
bool should_inject_content_script(URL::URL const&, MatchPatternSet const& matches, MatchPatternSet const& exclude_matches,
    ReadonlySpan<String> include_globs, ReadonlySpan<String> exclude_globs);

// The URL a frame's content scripts are matched against.
// - Ordinary URLs are returned unchanged.
// - about:blank/about:srcdoc use the creator (parent or opener) URL when
//   match_about_blank is set, or its origin when match_origin_as_fallback is set.
// - data:, blob: and filesystem: use the creator's origin (for blob: the blob's
//   own origin) when match_origin_as_fallback is set.
// Returns an empty Optional when the frame must not be matched at all. The
// creator URL must already be resolved (not itself about:/data:/blob:).
Optional<URL::URL> url_for_matching(URL::URL const& frame_url, Optional<URL::URL> const& creator_url,
    bool match_about_blank, bool match_origin_as_fallback);

// Pages the browser never lets extensions script: internal schemes, other
// extensions' pages, the Chrome Web Store, and file: unless allowed.
bool is_restricted_url(URL::URL const&, bool allow_file_access, Optional<StringView> own_extension_id = {});

}
