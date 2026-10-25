/*
 * Copyright (c) 2022, Lucas Chollet <lucas.chollet@free.fr>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibURL/Origin.h>
#include <LibWeb/Export.h>

namespace Web {

// A file of an installed extension, which only the browser can find.
struct ExtensionFile {
    String extension_id;
    ByteString path;
    // Who asked: the browser gives other origins only what the extension makes web accessible.
    Optional<URL::Origin> requester;
};

class WEB_API FileRequest {
public:
    FileRequest(ByteString path, ESCAPING Function<void(ErrorOr<i32>)> on_file_request_finish);

    ByteString path() const;

    Optional<ExtensionFile> const& extension_file() const { return m_extension_file; }
    void set_extension_file(ExtensionFile file) { m_extension_file = move(file); }

    Function<void(ErrorOr<i32>)> on_file_request_finish;

private:
    ByteString m_path {};
    Optional<ExtensionFile> m_extension_file;
};

}
