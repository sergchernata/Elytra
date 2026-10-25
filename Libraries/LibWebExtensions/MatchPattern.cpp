/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/StringBuilder.h>
#include <AK/Utf8View.h>
#include <LibURL/Host.h>
#include <LibURL/Origin.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWebExtensions/MatchPattern.h>

namespace WebExtensions {

static constexpr StringView all_urls_pattern = "<all_urls>"sv;

static bool is_valid_pattern_scheme(StringView scheme)
{
    return scheme.is_one_of("*"sv, "http"sv, "https"sv, "ws"sv, "wss"sv, "ftp"sv, "file"sv);
}

// Schemes a URL may have to match <all_urls>.
static bool is_matchable_url_scheme(StringView scheme)
{
    return scheme.is_one_of("http"sv, "https"sv, "ws"sv, "wss"sv, "ftp"sv, "file"sv);
}

static Vector<u32> to_code_points(StringView string)
{
    Vector<u32> code_points;
    for (auto code_point : Utf8View { string })
        code_points.append(code_point);
    return code_points;
}

enum class QuestionMarkIsWildcard {
    No,
    Yes,
};

// Iterative glob matching with single-star backtracking, so it is linear-ish
// in practice and never blows up on patterns like "*a*a*a*b".
static bool glob_matches_impl(StringView glob_string, StringView subject_string, QuestionMarkIsWildcard question_mark)
{
    auto glob = to_code_points(glob_string);
    auto subject = to_code_points(subject_string);

    size_t g = 0;
    size_t s = 0;
    Optional<size_t> star_glob;
    size_t star_subject = 0;

    while (s < subject.size()) {
        if (g < glob.size() && glob[g] == '*') {
            star_glob = g++;
            star_subject = s;
        } else if (g < glob.size() && (glob[g] == subject[s] || (question_mark == QuestionMarkIsWildcard::Yes && glob[g] == '?'))) {
            ++g;
            ++s;
        } else if (star_glob.has_value()) {
            g = *star_glob + 1;
            s = ++star_subject;
        } else {
            return false;
        }
    }
    while (g < glob.size() && glob[g] == '*')
        ++g;
    return g == glob.size();
}

bool glob_matches(StringView glob, StringView string)
{
    return glob_matches_impl(glob, string, QuestionMarkIsWildcard::Yes);
}

static ErrorOr<Optional<u16>> parse_port(StringView port)
{
    if (port == "*"sv)
        return Optional<u16> {};
    if (port.is_empty() || port.length() > 5)
        return Error::from_string_literal("Invalid port in match pattern");
    u32 value = 0;
    for (auto c : port) {
        if (!is_ascii_digit(c))
            return Error::from_string_literal("Invalid port in match pattern");
        value = value * 10 + parse_ascii_digit(c);
    }
    if (value > 65535)
        return Error::from_string_literal("Invalid port in match pattern");
    return Optional<u16> { static_cast<u16>(value) };
}

ErrorOr<MatchPattern> MatchPattern::parse(StringView input)
{
    MatchPattern pattern;
    pattern.m_source = TRY(String::from_utf8(input));

    if (input == all_urls_pattern) {
        pattern.m_all_urls = true;
        pattern.m_match_subdomains = true;
        pattern.m_path = "*"_string;
        return pattern;
    }

    auto separator = input.find("://"sv);
    if (!separator.has_value())
        return Error::from_string_literal("Match pattern is missing '://'");

    auto scheme = input.substring_view(0, *separator).to_ascii_lowercase_string();
    if (!is_valid_pattern_scheme(scheme))
        return Error::from_string_literal("Invalid scheme in match pattern");
    pattern.m_scheme = scheme;

    auto rest = input.substring_view(*separator + 3);

    // For file: everything after "file://" is the path, and any host matches.
    if (scheme == "file"sv) {
        if (!rest.starts_with('/'))
            return Error::from_string_literal("File match pattern must start with 'file:///'");
        pattern.m_match_subdomains = true;
        pattern.m_path = TRY(String::from_utf8(rest));
        return pattern;
    }

    auto slash = rest.find('/');
    if (!slash.has_value())
        return Error::from_string_literal("Match pattern is missing a path");
    auto authority = rest.substring_view(0, *slash);
    pattern.m_path = TRY(String::from_utf8(rest.substring_view(*slash)));

    StringView host;
    Optional<StringView> port;
    if (authority.starts_with('[')) {
        auto close = authority.find(']');
        if (!close.has_value())
            return Error::from_string_literal("Invalid IPv6 host in match pattern");
        host = authority.substring_view(0, *close + 1);
        auto after = authority.substring_view(*close + 1);
        if (!after.is_empty()) {
            if (!after.starts_with(':'))
                return Error::from_string_literal("Invalid host in match pattern");
            port = after.substring_view(1);
        }
    } else if (auto colon = authority.find(':'); colon.has_value()) {
        host = authority.substring_view(0, *colon);
        port = authority.substring_view(*colon + 1);
    } else {
        host = authority;
    }

    if (port.has_value())
        pattern.m_port = TRY(parse_port(*port));

    if (host.is_empty())
        return Error::from_string_literal("Match pattern has an empty host");

    if (host == "*"sv) {
        pattern.m_match_subdomains = true;
        return pattern;
    }

    if (host.starts_with("*."sv)) {
        pattern.m_match_subdomains = true;
        host = host.substring_view(2);
        if (host.is_empty())
            return Error::from_string_literal("Match pattern has an empty host");
    }

    // A wildcard is only allowed as the first host character.
    if (host.contains('*'))
        return Error::from_string_literal("Invalid wildcard in match pattern host");

    // Canonicalize like a URL host: lowercase, IDNA to ASCII, IP normalization.
    auto parsed_host = URL::Parser::parse_host(host);
    if (!parsed_host.has_value() || parsed_host->is_empty_host() || parsed_host->has<URL::OpaqueHost>())
        return Error::from_string_literal("Invalid host in match pattern");
    pattern.m_host = parsed_host->serialize();

    return pattern;
}

bool MatchPattern::matches_scheme(StringView url_scheme) const
{
    if (m_all_urls)
        return is_matchable_url_scheme(url_scheme);
    if (url_scheme == m_scheme)
        return true;

    // WebSocket URLs are treated as http/https for matching.
    if (url_scheme == "ws"sv)
        url_scheme = "http"sv;
    else if (url_scheme == "wss"sv)
        url_scheme = "https"sv;

    if (m_scheme == "*"sv)
        return url_scheme.is_one_of("http"sv, "https"sv);
    return url_scheme == m_scheme;
}

bool MatchPattern::matches_host(URL::URL const& url) const
{
    if (m_match_subdomains && m_host.is_empty())
        return true;

    auto url_host = url.host();
    if (!url_host.has_value())
        return false;
    auto serialized = url_host->serialize();
    if (serialized == m_host)
        return true;
    if (!m_match_subdomains)
        return false;

    // No subdomain matching against IP addresses.
    if (url_host->has<IPv4Address>() || url_host->has<IPv6Address>())
        return false;

    auto view = serialized.bytes_as_string_view();
    auto host = m_host.bytes_as_string_view();
    return view.length() > host.length() && view.ends_with(host) && view[view.length() - host.length() - 1] == '.';
}

bool MatchPattern::matches_impl(URL::URL const& url, bool check_path) const
{
    if (!matches_scheme(url.scheme()))
        return false;
    if (!matches_host(url))
        return false;
    if (m_port.has_value() && url.port_or_default() != *m_port)
        return false;
    if (!check_path)
        return true;

    // Match against path plus "?query"; '?' in the pattern path is literal.
    StringBuilder builder;
    builder.append(url.serialize_path());
    if (auto query = url.query(); query.has_value()) {
        builder.append('?');
        builder.append(*query);
    }
    return glob_matches_impl(m_path, builder.string_view(), QuestionMarkIsWildcard::No);
}

bool MatchPattern::matches(URL::URL const& url) const
{
    return matches_impl(url, true);
}

bool MatchPattern::matches_ignoring_path(URL::URL const& url) const
{
    return matches_impl(url, false);
}

bool MatchPattern::matches_origin(URL::Origin const& origin) const
{
    if (origin.is_opaque())
        return false;
    auto url = URL::Parser::basic_parse(ByteString::formatted("{}/", origin.serialize()));
    if (!url.has_value())
        return false;
    return matches_ignoring_path(*url);
}

ErrorOr<MatchPatternSet> MatchPatternSet::parse(ReadonlySpan<String> patterns)
{
    MatchPatternSet set;
    for (auto const& pattern : patterns)
        set.add(TRY(MatchPattern::parse(pattern)));
    return set;
}

bool MatchPatternSet::matches(URL::URL const& url) const
{
    return any_of(m_patterns, [&](auto const& pattern) { return pattern.matches(url); });
}

bool MatchPatternSet::matches_ignoring_path(URL::URL const& url) const
{
    return any_of(m_patterns, [&](auto const& pattern) { return pattern.matches_ignoring_path(url); });
}

bool MatchPatternSet::matches_origin(URL::Origin const& origin) const
{
    return any_of(m_patterns, [&](auto const& pattern) { return pattern.matches_origin(origin); });
}

bool should_inject_content_script(URL::URL const& url, MatchPatternSet const& matches, MatchPatternSet const& exclude_matches,
    ReadonlySpan<String> include_globs, ReadonlySpan<String> exclude_globs)
{
    if (!matches.matches(url) || exclude_matches.matches(url))
        return false;

    auto const& spec = url.serialize();
    if (!include_globs.is_empty() && !any_of(include_globs, [&](auto const& glob) { return glob_matches(glob, spec); }))
        return false;
    return !any_of(exclude_globs, [&](auto const& glob) { return glob_matches(glob, spec); });
}

static bool needs_fallback_resolution(StringView scheme)
{
    return scheme.is_one_of("about"sv, "data"sv, "blob"sv, "filesystem"sv);
}

static Optional<URL::URL> url_from_origin(URL::Origin const& origin)
{
    if (origin.is_opaque())
        return {};
    return URL::Parser::basic_parse(ByteString::formatted("{}/", origin.serialize()));
}

static Optional<URL::URL> creator_origin_url(Optional<URL::URL> const& creator_url)
{
    if (!creator_url.has_value() || needs_fallback_resolution(creator_url->scheme()))
        return {};
    return url_from_origin(creator_url->origin());
}

Optional<URL::URL> url_for_matching(URL::URL const& frame_url, Optional<URL::URL> const& creator_url,
    bool match_about_blank, bool match_origin_as_fallback)
{
    auto scheme = frame_url.scheme();
    if (!needs_fallback_resolution(scheme))
        return frame_url;

    if (scheme == "about"sv) {
        if (!frame_url.serialize_path().is_one_of("blank"sv, "srcdoc"sv))
            return {};
        if (match_origin_as_fallback)
            return creator_origin_url(creator_url);
        if (match_about_blank) {
            if (!creator_url.has_value() || needs_fallback_resolution(creator_url->scheme()))
                return {};
            return creator_url;
        }
        return {};
    }

    if (!match_origin_as_fallback)
        return {};

    // A blob: URL carries its own origin; prefer it over the creator's.
    if (scheme == "blob"sv) {
        if (auto url = url_from_origin(frame_url.origin()); url.has_value())
            return url;
    }
    return creator_origin_url(creator_url);
}

static bool is_web_store_url(URL::URL const& url)
{
    auto host = url.serialized_host();
    if (host == "chromewebstore.google.com"sv)
        return true;
    if (host != "chrome.google.com"sv)
        return false;
    auto path = url.serialize_path();
    return path == "/webstore"sv || path.starts_with("/webstore/"sv);
}

bool is_restricted_url(URL::URL const& url, bool allow_file_access, Optional<StringView> own_extension_id)
{
    auto scheme = url.scheme();
    if (scheme.is_one_of("http"sv, "https"sv, "ws"sv, "wss"sv, "ftp"sv))
        return is_web_store_url(url);
    if (scheme == "file"sv)
        return !allow_file_access;
    if (scheme == "chrome-extension"sv)
        return !own_extension_id.has_value() || url.serialized_host() != *own_extension_id;

    // Everything else (about:, data:, blob:, javascript:, ladybird:, chrome:,
    // view-source:, devtools:, unknown schemes) is off limits.
    return true;
}

}
