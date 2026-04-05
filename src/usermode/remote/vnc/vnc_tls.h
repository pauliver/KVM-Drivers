// vnc_tls.h - Schannel TLS wrapper for VNC AnonTLS security type (RFB sec type 18)
// Wraps a raw SOCKET in a server-side TLS 1.2/1.3 channel using a self-signed cert.
// After Upgrade(), all send/recv go through TLS.

#pragma once
#define SECURITY_WIN32
#include <winsock2.h>
#include <windows.h>
#include <sspi.h>
#include <schannel.h>
#include <wincrypt.h>
#include <iostream>
#include <vector>
#include <string>

#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")

// TlsSocket wraps a SOCKET and provides TLS-encrypted send/recv.
// Usage:
//   TlsSocket tls(rawSocket, certContext);
//   if (tls.ServerHandshake()) { tls.Send(...); tls.Recv(...); }
//   tls.Shutdown();
class TlsSocket {
public:
    TlsSocket(SOCKET sock, PCCERT_CONTEXT cert)
        : sock_(sock), cert_(cert), initialized_(false)
        , ctxValid_(false), credsValid_(false) {
        SecInvalidateHandle(&creds_);
        SecInvalidateHandle(&ctx_);
    }

    ~TlsSocket() { Shutdown(); }

    // Perform TLS server handshake. Returns true on success.
    bool ServerHandshake() {
        if (!AcquireServerCredentials()) {
            std::cerr << "[TLS] AcquireCredentialsHandle failed" << std::endl;
            return false;
        }

        if (!PerformHandshakeLoop()) {
            std::cerr << "[TLS] Handshake loop failed" << std::endl;
            return false;
        }

        // Query negotiated stream sizes
        SECURITY_STATUS ss = QueryContextAttributes(
            &ctx_, SECPKG_ATTR_STREAM_SIZES, &streamSizes_);
        if (ss != SEC_E_OK) {
            std::cerr << "[TLS] QueryContextAttributes failed: 0x" << std::hex << ss << std::endl;
            return false;
        }

        initialized_ = true;
        std::cout << "[TLS] Handshake complete, header=" << streamSizes_.cbHeader
                  << " trailer=" << streamSizes_.cbTrailer
                  << " maxMsg=" << streamSizes_.cbMaximumMessage << std::endl;
        return true;
    }

    // Send data over TLS. Returns bytes sent, -1 on error.
    int Send(const void* data, int len) {
        if (!initialized_) return -1;

        const BYTE* p = (const BYTE*)data;
        int total = 0;

        while (total < len) {
            int chunk = std::min(len - total, (int)streamSizes_.cbMaximumMessage);
            int result = SendChunk(p + total, chunk);
            if (result <= 0) return result;
            total += result;
        }
        return total;
    }

    // Receive data over TLS. Returns bytes received, 0 on close, -1 on error.
    int Recv(void* buf, int len) {
        if (!initialized_) return -1;

        // Drain any previously decrypted leftover first
        if (!decrypted_.empty()) {
            int copy = std::min(len, (int)decrypted_.size());
            memcpy(buf, decrypted_.data(), copy);
            decrypted_.erase(decrypted_.begin(), decrypted_.begin() + copy);
            return copy;
        }

        return RecvDecrypt(buf, len);
    }

    void Shutdown() {
        if (initialized_) {
            SendCloseNotify();
            initialized_ = false;
        }
        if (ctxValid_) {
            DeleteSecurityContext(&ctx_);
            ctxValid_ = false;
        }
        if (credsValid_) {
            FreeCredentialsHandle(&creds_);
            credsValid_ = false;
        }
    }

    // Create a self-signed certificate usable for TLS server
    static PCCERT_CONTEXT CreateSelfSignedCert(const std::wstring& subject = L"CN=KVM-Drivers-VNC") {
        CERT_NAME_BLOB nameBlob = {};
        if (!CertStrToNameW(X509_ASN_ENCODING, subject.c_str(),
                CERT_X500_NAME_STR, NULL, NULL, &nameBlob.cbData, NULL))
            return nullptr;

        std::vector<BYTE> nameBuf(nameBlob.cbData);
        nameBlob.pbData = nameBuf.data();
        if (!CertStrToNameW(X509_ASN_ENCODING, subject.c_str(),
                CERT_X500_NAME_STR, NULL, nameBuf.data(), &nameBlob.cbData, NULL))
            return nullptr;

        // Key provider info
        CRYPT_KEY_PROV_INFO keyInfo = {};
        keyInfo.pwszContainerName = const_cast<LPWSTR>(L"KVMDriversVNCKey");
        keyInfo.pwszProvName      = const_cast<LPWSTR>(MS_DEF_RSA_SCHANNEL_PROV_W);
        keyInfo.dwProvType        = PROV_RSA_SCHANNEL;
        keyInfo.dwFlags           = CRYPT_MACHINE_KEYSET;
        keyInfo.dwKeySpec         = AT_KEYEXCHANGE;

        // Validity: 10 years
        SYSTEMTIME start, end;
        GetSystemTime(&start);
        GetSystemTime(&end);
        end.wYear += 10;
        // Signature algorithm: SHA-256 RSA
        CRYPT_ALGORITHM_IDENTIFIER sigAlg = {};
        sigAlg.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);

        PCCERT_CONTEXT cert = CertCreateSelfSignCertificate(
            NULL, &nameBlob, 0, &keyInfo, &sigAlg, &start, &end, NULL);

        if (!cert) {
            std::cerr << "[TLS] CertCreateSelfSignCertificate failed: 0x"
                      << std::hex << GetLastError() << std::endl;
        }
        return cert;
    }

private:
    SOCKET           sock_;
    PCCERT_CONTEXT   cert_;
    CredHandle       creds_;
    CtxtHandle       ctx_;
    SecPkgContext_StreamSizes streamSizes_;
    bool             initialized_;
    bool             ctxValid_;
    bool             credsValid_;
    std::vector<BYTE> rawBuf_;    // undecrypted socket data
    std::vector<BYTE> decrypted_; // leftover decrypted data

    bool AcquireServerCredentials() {
        SCHANNEL_CRED sc = {};
        sc.dwVersion     = SCHANNEL_CRED_VERSION;
        sc.cCreds        = 1;
        sc.paCred        = &cert_;
        sc.grbitEnabledProtocols = SP_PROT_TLS1_2_SERVER | SP_PROT_TLS1_3_SERVER;
        sc.dwFlags       = SCH_CRED_NO_SYSTEM_MAPPER | SCH_USE_STRONG_CRYPTO;

        SECURITY_STATUS ss = AcquireCredentialsHandleW(
            NULL, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_INBOUND,
            NULL, &sc, NULL, NULL, &creds_, NULL);

        if (ss != SEC_E_OK) return false;
        credsValid_ = true;
        return true;
    }

    bool PerformHandshakeLoop() {
        static constexpr int BUFSIZE = 16384;
        rawBuf_.resize(BUFSIZE);
        int rawLen = 0;
        bool firstCall = true;

        while (true) {
            // Receive more data if needed
            if (firstCall || rawLen == 0) {
                int n = recv(sock_, (char*)rawBuf_.data() + rawLen,
                    (int)rawBuf_.size() - rawLen, 0);
                if (n <= 0) return false;
                rawLen += n;
                firstCall = false;
            }

            SecBuffer inBufs[2] = {};
            inBufs[0].BufferType = SECBUFFER_TOKEN;
            inBufs[0].pvBuffer   = rawBuf_.data();
            inBufs[0].cbBuffer   = rawLen;
            inBufs[1].BufferType = SECBUFFER_EMPTY;

            SecBuffer outBufs[1] = {};
            outBufs[0].BufferType = SECBUFFER_TOKEN;

            SecBufferDesc inDesc  = { SECBUFFER_VERSION, 2, inBufs };
            SecBufferDesc outDesc = { SECBUFFER_VERSION, 1, outBufs };

            ULONG attrs = 0;
            SECURITY_STATUS ss = AcceptSecurityContext(
                &creds_,
                ctxValid_ ? &ctx_ : NULL,
                &inDesc, ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT |
                         ASC_REQ_CONFIDENTIALITY | ASC_REQ_STREAM,
                SECURITY_NATIVE_DREP, &ctx_, &outDesc, &attrs, NULL);

            ctxValid_ = true;

            // Send output token if present
            if (outBufs[0].cbBuffer > 0 && outBufs[0].pvBuffer) {
                int sent = send(sock_, (char*)outBufs[0].pvBuffer,
                    (int)outBufs[0].cbBuffer, 0);
                FreeContextBuffer(outBufs[0].pvBuffer);
                if (sent <= 0) return false;
            }

            // Handle leftover data from input
            if (inBufs[1].BufferType == SECBUFFER_EXTRA && inBufs[1].cbBuffer > 0) {
                memmove(rawBuf_.data(),
                    rawBuf_.data() + rawLen - inBufs[1].cbBuffer,
                    inBufs[1].cbBuffer);
                rawLen = inBufs[1].cbBuffer;
            } else {
                rawLen = 0;
            }

            if (ss == SEC_E_OK) {
                // Move any leftover raw data for subsequent Recv calls
                if (rawLen > 0) {
                    rawBuf_.resize(rawLen);
                    // Will be decrypted on first Recv call
                } else {
                    rawBuf_.clear();
                }
                return true;
            }
            if (ss == SEC_I_CONTINUE_NEEDED) continue;  // need more data
            if (ss == SEC_E_INCOMPLETE_MESSAGE) continue;  // need more data

            std::cerr << "[TLS] AcceptSecurityContext error: 0x" << std::hex << ss << std::endl;
            return false;
        }
    }

    int SendChunk(const BYTE* data, int len) {
        std::vector<BYTE> msg(streamSizes_.cbHeader + len + streamSizes_.cbTrailer);

        SecBuffer bufs[3] = {};
        bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
        bufs[0].pvBuffer   = msg.data();
        bufs[0].cbBuffer   = streamSizes_.cbHeader;
        bufs[1].BufferType = SECBUFFER_DATA;
        bufs[1].pvBuffer   = msg.data() + streamSizes_.cbHeader;
        bufs[1].cbBuffer   = len;
        memcpy(bufs[1].pvBuffer, data, len);
        bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
        bufs[2].pvBuffer   = msg.data() + streamSizes_.cbHeader + len;
        bufs[2].cbBuffer   = streamSizes_.cbTrailer;

        SecBufferDesc desc = { SECBUFFER_VERSION, 3, bufs };
        SECURITY_STATUS ss = EncryptMessage(&ctx_, 0, &desc, 0);
        if (ss != SEC_E_OK) {
            std::cerr << "[TLS] EncryptMessage failed: 0x" << std::hex << ss << std::endl;
            return -1;
        }

        int total = bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer;
        return send(sock_, (char*)msg.data(), total, 0) > 0 ? len : -1;
    }

    int RecvDecrypt(void* buf, int maxLen) {
        static constexpr int BUFSIZE = 32768;
        if (rawBuf_.size() < BUFSIZE) rawBuf_.resize(BUFSIZE);

        // Receive until we can decrypt a full record
        int rawLen = 0;
        while (true) {
            if (rawLen > 0) {
                SecBuffer bufs[4] = {};
                bufs[0].BufferType = SECBUFFER_DATA;
                bufs[0].pvBuffer   = rawBuf_.data();
                bufs[0].cbBuffer   = rawLen;
                bufs[1].BufferType = SECBUFFER_EMPTY;
                bufs[2].BufferType = SECBUFFER_EMPTY;
                bufs[3].BufferType = SECBUFFER_EMPTY;

                SecBufferDesc desc = { SECBUFFER_VERSION, 4, bufs };
                SECURITY_STATUS ss = DecryptMessage(&ctx_, &desc, 0, NULL);

                if (ss == SEC_E_OK) {
                    // Find decrypted data buffer
                    for (int i = 0; i < 4; i++) {
                        if (bufs[i].BufferType == SECBUFFER_DATA && bufs[i].cbBuffer > 0) {
                            int copy = std::min(maxLen, (int)bufs[i].cbBuffer);
                            memcpy(buf, bufs[i].pvBuffer, copy);
                            // Save leftover decrypted data
                            if ((int)bufs[i].cbBuffer > copy) {
                                decrypted_.insert(decrypted_.end(),
                                    (BYTE*)bufs[i].pvBuffer + copy,
                                    (BYTE*)bufs[i].pvBuffer + bufs[i].cbBuffer);
                            }
                            return copy;
                        }
                    }
                    // Handle SECBUFFER_EXTRA (leftover undecrypted)
                    for (int i = 1; i < 4; i++) {
                        if (bufs[i].BufferType == SECBUFFER_EXTRA && bufs[i].cbBuffer > 0) {
                            memmove(rawBuf_.data(),
                                rawBuf_.data() + rawLen - bufs[i].cbBuffer,
                                bufs[i].cbBuffer);
                            rawLen = bufs[i].cbBuffer;
                            break;
                        }
                    }
                    return 0;  // empty record
                }
                if (ss == SEC_I_CONTEXT_EXPIRED) return 0;  // TLS close_notify
                if (ss != SEC_E_INCOMPLETE_MESSAGE) {
                    std::cerr << "[TLS] DecryptMessage error: 0x" << std::hex << ss << std::endl;
                    return -1;
                }
                // INCOMPLETE - need more data
            }

            int n = recv(sock_, (char*)rawBuf_.data() + rawLen,
                (int)rawBuf_.size() - rawLen, 0);
            if (n <= 0) return n;
            rawLen += n;
        }
    }

    void SendCloseNotify() {
        DWORD token = SCHANNEL_SHUTDOWN;
        SecBuffer buf = { sizeof(token), SECBUFFER_TOKEN, &token };
        SecBufferDesc desc = { SECBUFFER_VERSION, 1, &buf };
        ApplyControlToken(&ctx_, &desc);

        SecBuffer outBuf = { 0, SECBUFFER_TOKEN, NULL };
        SecBufferDesc outDesc = { SECBUFFER_VERSION, 1, &outBuf };
        ULONG attrs = 0;
        AcceptSecurityContext(&creds_, &ctx_, NULL,
            ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT |
            ASC_REQ_CONFIDENTIALITY | ASC_REQ_STREAM,
            SECURITY_NATIVE_DREP, &ctx_, &outDesc, &attrs, NULL);
        if (outBuf.pvBuffer && outBuf.cbBuffer > 0) {
            send(sock_, (char*)outBuf.pvBuffer, (int)outBuf.cbBuffer, 0);
            FreeContextBuffer(outBuf.pvBuffer);
        }
    }
};
