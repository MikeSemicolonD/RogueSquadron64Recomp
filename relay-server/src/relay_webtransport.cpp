#include "relay_webtransport.h"
#include <enet6/enet.h>
#include <wtf.h>
#include <cstdio>

#ifdef _WIN32
#include <ws2tcpip.h>
#include <wincrypt.h>
#include <ncrypt.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace rs64::relay {

namespace {

constexpr int64_t kDayMs = 24LL * 60 * 60 * 1000;

std::string hex(const uint8_t* p, size_t n)
{

    static const char kDigits[] = "0123456789abcdef";
    std::string out;

    for (size_t i = 0; i < n; ++i) {
        out += kDigits[p[i] >> 4];
        out += kDigits[p[i] & 15];
    }

    return out;
}

// The browser's address as ENet would print it, so a browser and a desktop player on one IP count as one address.
std::string peer_text(wtf_session_t* session)
{

    sockaddr_storage a = {};
    size_t len = sizeof a;
    char text[64] = "";

    if (wtf_session_get_peer_address(session, &a, &len) != WTF_SUCCESS)
    {
        return "?";
    }

    if (a.ss_family == AF_INET) {
        char v4[INET_ADDRSTRLEN] = "";

        inet_ntop(AF_INET, &((sockaddr_in*)&a)->sin_addr, v4, sizeof v4);
        snprintf(text, sizeof text, "::ffff:%s", v4);
    } else {
        inet_ntop(AF_INET6, &((sockaddr_in6*)&a)->sin6_addr, text, sizeof text);
    }

    ENetAddress e;
    char canon[64];

    if (enet_address_set_host_ip(&e, text) != 0 || enet_address_get_host_ip(&e, canon, sizeof canon) != 0)
    {
        return text;
    }

    return canon;
}

void log_line(wtf_log_level_t, const char* component, const char*, int, const char* message, void*)
{
    std::fprintf(stderr, "[webtransport] %s: %s\n", component ? component : "", message ? message : "");
}

wtf_connection_decision_t accept_all(const wtf_connection_request_t*, wtf_connection_response_t*, void*)
{
    return WTF_CONNECTION_ACCEPT;
}

// libwtf hands each event the session's own context (0 until CONNECTED sets the bridge id), so the bridge is reached here instead.
Bridge* g_bridge = nullptr;

void on_session(const wtf_session_event_t* ev)
{

    Bridge& bridge = *g_bridge;
    wtf_session_t* session = ev->session;
    const uint32_t id = (uint32_t)(uintptr_t)wtf_session_get_context(session);

    switch (ev->type) {
    case WTF_SESSION_EVENT_CONNECTED: {
        wtf_session_ref(session);

        const uint32_t opened = bridge.open(peer_text(session), [session](const uint8_t* d, size_t n) {
            wtf_session_send_datagram_copy(session, d, n);
        });

        if (!opened) {
            wtf_session_close(session, 0, "relay full");
            wtf_session_unref(session);
            break;
        }

        wtf_session_set_context(session, (void*)(uintptr_t)opened);
        break;
    }

    case WTF_SESSION_EVENT_DATAGRAM_RECEIVED:
        if (id) bridge.from_browser(id, ev->datagram_received.data, ev->datagram_received.length);

        break;

    case WTF_SESSION_EVENT_DISCONNECTED:
        if (id) {
            bridge.close(id);
            wtf_session_set_context(session, nullptr);
            wtf_session_unref(session);
        }

        break;

    default:
        break;
    }
}

}

#ifdef _WIN32

// Owns the Windows handles make_certificate opens, so every early return frees them.
struct CertHandles {
    NCRYPT_PROV_HANDLE prov = 0;
    NCRYPT_KEY_HANDLE key = 0;
    PCCERT_CONTEXT cert = nullptr;
    HCERTSTORE store = nullptr;

    ~CertHandles()
    {

        if (store) CertCloseStore(store, 0);

        if (cert) CertFreeCertificateContext(cert);

        if (key) NCryptFreeObject(key);

        if (prov) NCryptFreeObject(prov);
    }
};

bool make_certificate(std::vector<uint8_t>* pkcs12, std::string* sha256_hex, std::string* error)
{

    CertHandles h;

    // The key stays in memory (no name, never persisted); exportable so it can go into the PKCS#12 blob.
    const DWORD policy = NCRYPT_ALLOW_EXPORT_FLAG | NCRYPT_ALLOW_PLAINTEXT_EXPORT_FLAG;

    if (NCryptOpenStorageProvider(&h.prov, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS)
    {
        *error = "could not open the key provider";
        return false;
    }

    if (NCryptCreatePersistedKey(h.prov, &h.key, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0, 0) != ERROR_SUCCESS)
    {
        *error = "could not make a key";
        return false;
    }

    if (NCryptSetProperty(h.key, NCRYPT_EXPORT_POLICY_PROPERTY, (PBYTE)&policy, sizeof policy, NCRYPT_PERSIST_FLAG) != ERROR_SUCCESS || NCryptFinalizeKey(h.key, 0) != ERROR_SUCCESS)
    {
        *error = "could not finish the key";
        return false;
    }

    BYTE name[128];
    DWORD name_len = sizeof name;

    if (!CertStrToNameW(X509_ASN_ENCODING, L"CN=rs64-relay", CERT_X500_NAME_STR, nullptr, name, &name_len, nullptr))
    {
        *error = "could not name the certificate";
        return false;
    }

    // Valid from an hour ago (clock skew) to 13 days from now; FILETIME counts 100 ns ticks.
    constexpr uint64_t kHour = 36000000000ULL;
    constexpr uint64_t kDay = 24 * kHour;
    FILETIME now;
    ULARGE_INTEGER from;
    ULARGE_INTEGER to;

    GetSystemTimeAsFileTime(&now);
    from.LowPart = now.dwLowDateTime;
    from.HighPart = now.dwHighDateTime;
    to = from;
    from.QuadPart -= kHour;
    to.QuadPart += 13 * kDay;

    const FILETIME from_ft = { from.LowPart, from.HighPart };
    const FILETIME to_ft = { to.LowPart, to.HighPart };
    SYSTEMTIME start;
    SYSTEMTIME end;

    FileTimeToSystemTime(&from_ft, &start);
    FileTimeToSystemTime(&to_ft, &end);

    // NO_KEY_INFO: the key has no provider name to record.
    CERT_NAME_BLOB subject = { name_len, name };
    CRYPT_ALGORITHM_IDENTIFIER alg = { (LPSTR)szOID_ECDSA_SHA256, { 0, nullptr } };

    // Chrome's hash-pinned certificate parser needs an extensions block; a subjectAltName is the usual one.
    CERT_ALT_NAME_ENTRY alt_entry = {};
    CERT_ALT_NAME_INFO alt_info = { 1, &alt_entry };
    BYTE alt_der[64];
    DWORD alt_len = sizeof alt_der;

    alt_entry.dwAltNameChoice = CERT_ALT_NAME_DNS_NAME;
    alt_entry.pwszDNSName = (LPWSTR)L"rs64-relay";

    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_ALTERNATE_NAME, &alt_info, 0, nullptr, alt_der, &alt_len))
    {
        *error = "could not name the certificate";
        return false;
    }

    CERT_EXTENSION alt_ext = { (LPSTR)szOID_SUBJECT_ALT_NAME2, FALSE, { alt_len, alt_der } };
    CERT_EXTENSIONS extensions = { 1, &alt_ext };

    h.cert = CertCreateSelfSignCertificate(h.key, &subject, CERT_CREATE_SELFSIGN_NO_KEY_INFO, nullptr, &alg, &start, &end, &extensions);

    if (!h.cert)
    {
        *error = "could not sign the certificate";
        return false;
    }

    // The export reads the key from the store's copy of the certificate, so the key goes on that copy.
    PCCERT_CONTEXT stored = nullptr;

    h.store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr);

    CERT_KEY_CONTEXT key_context = {};

    key_context.cbSize = sizeof key_context;
    key_context.hNCryptKey = h.key;
    key_context.dwKeySpec = CERT_NCRYPT_KEY_SPEC;

    const bool paired = h.store && CertAddCertificateContextToStore(h.store, h.cert, CERT_STORE_ADD_ALWAYS, &stored) && CertSetCertificateContextProperty(stored, CERT_KEY_CONTEXT_PROP_ID, CERT_SET_PROPERTY_INHIBIT_PERSIST_FLAG, &key_context);

    if (stored) CertFreeCertificateContext(stored);

    if (!paired)
    {
        *error = "could not pair the certificate with its key";
        return false;
    }

    // AES-256 with SHA-256: MsQuic's OpenSSL 3 rejects Windows' default RC2/3DES PKCS#12. The first call sizes the blob.
    PKCS12_PBES2_EXPORT_PARAMS params = { sizeof params, 0, (PWSTR)PKCS12_PBES2_ALG_AES256_SHA256 };
    const DWORD flags = EXPORT_PRIVATE_KEYS | REPORT_NO_PRIVATE_KEY | REPORT_NOT_ABLE_TO_EXPORT_PRIVATE_KEY | PKCS12_EXPORT_PBES2_PARAMS;
    CRYPT_DATA_BLOB blob = {};

    if (!PFXExportCertStoreEx(h.store, &blob, L"", &params, flags))
    {
        *error = "could not export the certificate";
        return false;
    }

    pkcs12->resize(blob.cbData);
    blob.pbData = pkcs12->data();

    if (!PFXExportCertStoreEx(h.store, &blob, L"", &params, flags))
    {
        *error = "could not export the certificate";
        return false;
    }

    pkcs12->resize(blob.cbData);

    BYTE digest[32];
    DWORD digest_len = sizeof digest;

    if (!CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0, nullptr, h.cert->pbCertEncoded, h.cert->cbCertEncoded, digest, &digest_len))
    {
        *error = "could not hash the certificate";
        return false;
    }

    *sha256_hex = hex(digest, digest_len);
    return true;
}

#else

// shortcut: no certificate maker outside Windows yet; write one on libcrypto (EVP_EC_gen, X509, PKCS12_create) when the relay ships for Linux.
bool make_certificate(std::vector<uint8_t>*, std::string*, std::string* error)
{

    *error = "browser players need a Windows relay for now";
    return false;
}

#endif

bool BrowserListener::start(uint16_t port, int64_t now_ms, std::string* error)
{

    std::vector<uint8_t> pkcs12;
    std::string hash;

    if (!make_certificate(&pkcs12, &hash, error))
    {
        return false;
    }

    wtf_context_config_t cc = {};
    wtf_context_t* context = nullptr;

    cc.log_level = WTF_LOG_LEVEL_ERROR;
    cc.log_callback = log_line;
    cc.worker_thread_count = 2;

    if (wtf_context_create(&cc, &context) != WTF_SUCCESS) {
        *error = "could not start WebTransport";
        return false;
    }

    wtf_certificate_config_t cert = {};

    cert.cert_type = WTF_CERT_TYPE_PKCS12;
    cert.cert_data.pkcs12.data = pkcs12.data();
    cert.cert_data.pkcs12.data_size = pkcs12.size();
    cert.cert_data.pkcs12.password = "";

    wtf_server_config_t sc = {};
    wtf_server_t* server = nullptr;

    sc.port = port;
    sc.cert_config = &cert;
    sc.max_sessions_per_connection = 1;
    sc.max_streams_per_session = 16;
    sc.idle_timeout_ms = 30000;
    sc.handshake_timeout_ms = 10000;
    sc.connection_validator = accept_all;
    sc.session_callback = on_session;
    g_bridge = &bridge_;

    const wtf_result_t created = wtf_server_create(context, &sc, &server);

    if (created != WTF_SUCCESS) {
        wtf_context_destroy(context);
        *error = std::string("could not set up the browser listener: ") + wtf_result_to_string(created);
        return false;
    }

    const wtf_result_t started = wtf_server_start(server);

    if (started != WTF_SUCCESS) {
        wtf_server_destroy(server);
        wtf_context_destroy(context);
        *error = "cannot listen on UDP port " + std::to_string(port) + " for browser players: " + wtf_result_to_string(started);
        return false;
    }

    context_ = context;
    server_ = server;
    port_ = port;
    made_ms_ = now_ms;
    hash_ = hash;
    return true;
}

bool BrowserListener::tick(int64_t now_ms, std::string* error)
{

    const int64_t age = now_ms - made_ms_;

    if (!server_ || age < 12 * kDayMs || (age < 13 * kDayMs && bridge_.sessions() > 0))
    {
        return false;
    }

    const uint16_t port = port_;

    stop();
    return start(port, now_ms, error);
}

void BrowserListener::stop()
{

    if (server_) {
        wtf_server_stop((wtf_server_t*)server_);
        wtf_server_destroy((wtf_server_t*)server_);
        server_ = nullptr;
    }

    if (context_) {
        wtf_context_destroy((wtf_context_t*)context_);
        context_ = nullptr;
    }
}

}
