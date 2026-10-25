/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// chrome.permissions, chrome.management, chrome.privacy and the rest of chrome.runtime.

#include "ExtensionTestHarness.h"

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    return ExtensionTests::run_extension_tests(arguments, [](ExtensionTests::Harness&) {
    });
}
