#pragma once
// Writes a throwaway self-signed certificate + key as one PEM file (tests only).

#include <cstdio>
#include <filesystem>
#include <string>

#include "test_util/Unique.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

namespace tcng::test {

inline bool writeSelfSignedPem(const std::string& path) {
    EVP_PKEY* key = EVP_RSA_gen(2048);
    if (!key)
        return false;
    X509* cert = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
    X509_set_pubkey(cert, key);
    X509_NAME* name = X509_NAME_new();  // built separately: OpenSSL 4 returns const subject names
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
    X509_set_subject_name(cert, name);
    X509_set_issuer_name(cert, name);
    X509_NAME_free(name);
    X509_sign(cert, key, EVP_sha256());

    bool ok = false;
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        ok = PEM_write_X509(f, cert) && PEM_write_PrivateKey(f, key, nullptr, nullptr, 0, nullptr, nullptr);
        std::fclose(f);
    }
    X509_free(cert);
    EVP_PKEY_free(key);
    return ok;
}

// A self-signed PEM in the temp directory for the lifetime of the object.
struct TempPem {
    std::string path;
    bool ok;
    explicit TempPem(const std::string& name) : path(uniqueSocketPath(name) + ".pem"), ok(writeSelfSignedPem(path)) {}
    ~TempPem() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    TempPem(const TempPem&) = delete;
    TempPem& operator=(const TempPem&) = delete;
};

}  // namespace tcng::test
