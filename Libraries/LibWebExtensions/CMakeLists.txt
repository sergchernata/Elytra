set(SOURCES
    Manifest.cpp
    Messages.cpp
    ExtensionId.cpp
    MatchPattern.cpp
    Zip.cpp
    Crx.cpp
    NativeMessaging.cpp
    StorageArea.cpp
)

ladybird_lib(LibWebExtensions webextensions)
target_link_libraries(LibWebExtensions PRIVATE LibCore LibCrypto LibCompress LibFileSystem LibURL OpenSSL::Crypto)
