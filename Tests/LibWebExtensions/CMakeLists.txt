set(TEST_SOURCES
    TestManifest.cpp
    TestMessages.cpp
    TestExtensionId.cpp
    TestMatchPattern.cpp
    TestZip.cpp
    TestCrx.cpp
    TestNativeMessaging.cpp
    TestStorageArea.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibWebExtensions LIBS LibCore LibCrypto LibCompress LibFileSystem LibURL LibWebExtensions OpenSSL::Crypto)
endforeach()

