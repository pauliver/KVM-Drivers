// certificate_manager.cpp - TLS Certificate management for secure connections
#include <windows.h>
#include <wincrypt.h>

#ifndef PKCS12_EXPORT_EXTENDED_PROPERTIES
#define PKCS12_EXPORT_EXTENDED_PROPERTIES 0x00000010
#endif
#include <cryptuiapi.h>
#include <iostream>
#include <string>
#include <vector>
#include <memory>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "cryptui.lib")

class CertificateManager {
public:
    CertificateManager() : certStore_(nullptr) {}
    
    ~CertificateManager() {
        if (certStore_) {
            CertCloseStore(certStore_, 0);
        }
    }
    
    // Initialize certificate store
    bool Initialize(LPCWSTR storeName = L"MY") {
        certStore_ = CertOpenStore(
            CERT_STORE_PROV_SYSTEM,
            0,
            NULL,
            CERT_SYSTEM_STORE_CURRENT_USER,
            storeName
        );
        
        if (!certStore_) {
            std::cerr << "[CertManager] Failed to open certificate store" << std::endl;
            return false;
        }
        
        return true;
    }
    
    // Create self-signed certificate for testing
    bool CreateSelfSignedCertificate(
        LPCWSTR subjectName,
        LPCWSTR friendlyName,
        int validYears = 1
    ) {
        std::cout << "[CertManager] Creating self-signed certificate for: ";
        std::wcout << subjectName << std::endl;
        
        // Create key provider
        CRYPT_KEY_PROV_INFO keyProvInfo = {};
        keyProvInfo.pwszContainerName = (LPWSTR)L"KVM-Container";
        keyProvInfo.pwszProvName = (LPWSTR)MS_ENH_RSA_AES_PROV;
        keyProvInfo.dwProvType = PROV_RSA_AES;
        keyProvInfo.dwFlags = CRYPT_MACHINE_KEYSET;
        keyProvInfo.cProvParam = 0;
        keyProvInfo.rgProvParam = NULL;
        keyProvInfo.dwKeySpec = AT_KEYEXCHANGE;
        
        // Create subject name
        std::wstring subject = L"CN=";
        subject += subjectName;
        
        CERT_NAME_BLOB subjectBlob = {};
        if (!CertStrToName(
            X509_ASN_ENCODING,
            subject.c_str(),
            CERT_X500_NAME_STR,
            NULL,
            NULL,
            &subjectBlob.cbData,
            NULL
        )) {
            std::cerr << "[CertManager] Failed to convert subject name" << std::endl;
            return false;
        }
        
        std::vector<BYTE> subjectData(subjectBlob.cbData);
        subjectBlob.pbData = subjectData.data();
        
        if (!CertStrToName(
            X509_ASN_ENCODING,
            subject.c_str(),
            CERT_X500_NAME_STR,
            NULL,
            subjectBlob.pbData,
            &subjectBlob.cbData,
            NULL
        )) {
            std::cerr << "[CertManager] Failed to create subject blob" << std::endl;
            return false;
        }
        
        // Set validity period
        SYSTEMTIME startTime, endTime;
        GetSystemTime(&startTime);
        endTime = startTime;
        endTime.wYear += validYears;
        
        // Create certificate
        PCCERT_CONTEXT cert = CertCreateSelfSignCertificate(
            NULL,
            &subjectBlob,
            0,
            &keyProvInfo,
            NULL,
            &startTime,
            &endTime,
            NULL
        );
        
        if (!cert) {
            std::cerr << "[CertManager] Failed to create self-signed certificate" << std::endl;
            return false;
        }
        
        // Add friendly name
        CERT_NAME_VALUE friendlyNameValue = {};
        friendlyNameValue.dwValueType = CERT_RDN_UNICODE_STRING;
        friendlyNameValue.Value.pbData = (BYTE*)friendlyName;
        friendlyNameValue.Value.cbData = (DWORD)(wcslen(friendlyName) * sizeof(wchar_t));
        
        if (!CertSetCertificateContextProperty(
            cert,
            CERT_FRIENDLY_NAME_PROP_ID,
            0,
            &friendlyNameValue
        )) {
            std::cerr << "[CertManager] Warning: Failed to set friendly name" << std::endl;
        }
        
        // Add enhanced key usage for TLS
        // 1.3.6.1.5.5.7.3.1 = Server Authentication
        // 1.3.6.1.5.5.7.3.2 = Client Authentication
        char serverAuth[] = "1.3.6.1.5.5.7.3.1";
        char clientAuth[] = "1.3.6.1.5.5.7.3.2";
        
        CERT_ENHKEY_USAGE eku = {};
        eku.cUsageIdentifier = 2;
        LPSTR usages[] = { serverAuth, clientAuth };
        eku.rgpszUsageIdentifier = usages;
        
        CRYPT_DATA_BLOB ekuBlob = {};
        ekuBlob.cbData = sizeof(eku);
        ekuBlob.pbData = (BYTE*)&eku;
        
        if (!CertSetCertificateContextProperty(
            cert,
            CERT_ENHKEY_USAGE_PROP_ID,
            0,
            &ekuBlob
        )) {
            std::cerr << "[CertManager] Warning: Failed to set EKU" << std::endl;
        }
        
        // Add to store
        if (!CertAddCertificateContextToStore(
            certStore_,
            cert,
            CERT_STORE_ADD_REPLACE_EXISTING,
            NULL
        )) {
            std::cerr << "[CertManager] Failed to add certificate to store" << std::endl;
            CertFreeCertificateContext(cert);
            return false;
        }
        
        std::cout << "[CertManager] Self-signed certificate created successfully" << std::endl;
        CertFreeCertificateContext(cert);
        return true;
    }
    
    // Find certificate by subject name
    PCCERT_CONTEXT FindCertificate(LPCWSTR subjectName) {
        if (!certStore_) return nullptr;
        
        std::wstring searchName = L"CN=";
        searchName += subjectName;
        
        PCCERT_CONTEXT cert = nullptr;
        
        while ((cert = CertEnumCertificatesInStore(certStore_, cert)) != nullptr) {
            // Get subject name
            DWORD nameSize = CertGetNameString(
                cert,
                CERT_NAME_SIMPLE_DISPLAY_TYPE,
                0,
                NULL,
                NULL,
                0
            );
            
            if (nameSize > 0) {
                std::vector<wchar_t> name(nameSize);
                CertGetNameString(
                    cert,
                    CERT_NAME_SIMPLE_DISPLAY_TYPE,
                    0,
                    NULL,
                    name.data(),
                    nameSize
                );
                
                if (wcsstr(name.data(), subjectName) != nullptr) {
                    return cert;  // Return found certificate (caller must free)
                }
            }
        }
        
        return nullptr;
    }
    
    // Export certificate to file
    bool ExportCertificate(LPCWSTR subjectName, LPCWSTR exportPath, bool includePrivateKey) {
        PCCERT_CONTEXT cert = FindCertificate(subjectName);
        if (!cert) {
            std::cerr << "[CertManager] Certificate not found" << std::endl;
            return false;
        }
        
        // Prepare export
        CRYPT_DATA_BLOB exportBlob = {};
        DWORD exportFlags = includePrivateKey ? 
            EXPORT_PRIVATE_KEYS | PKCS12_EXPORT_EXTENDED_PROPERTIES : 0;
        
        // Get export size
        if (!PFXExportCertStoreEx(
            certStore_,
            &exportBlob,
            L"",  // Password
            NULL,
            exportFlags
        )) {
            std::cerr << "[CertManager] Failed to get export size" << std::endl;
            CertFreeCertificateContext(cert);
            return false;
        }
        
        // Allocate and export
        exportBlob.pbData = (BYTE*)malloc(exportBlob.cbData);
        if (!exportBlob.pbData) {
            CertFreeCertificateContext(cert);
            return false;
        }
        
        if (!PFXExportCertStoreEx(
            certStore_,
            &exportBlob,
            L"",
            NULL,
            exportFlags
        )) {
            std::cerr << "[CertManager] Failed to export certificate" << std::endl;
            free(exportBlob.pbData);
            CertFreeCertificateContext(cert);
            return false;
        }
        
        // Write to file
        HANDLE file = CreateFile(
            exportPath,
            GENERIC_WRITE,
            0,
            NULL,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            NULL
        );
        
        if (file == INVALID_HANDLE_VALUE) {
            std::cerr << "[CertManager] Failed to create export file" << std::endl;
            free(exportBlob.pbData);
            CertFreeCertificateContext(cert);
            return false;
        }
        
        DWORD written;
        WriteFile(file, exportBlob.pbData, exportBlob.cbData, &written, NULL);
        CloseHandle(file);
        
        free(exportBlob.pbData);
        CertFreeCertificateContext(cert);
        
        std::wcout << L"[CertManager] Certificate exported to: " << exportPath << std::endl;
        return true;
    }
    
    // Import certificate from file
    bool ImportCertificate(LPCWSTR importPath, LPCWSTR password) {
        // Read file
        HANDLE file = CreateFile(
            importPath,
            GENERIC_READ,
            FILE_SHARE_READ,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL
        );
        
        if (file == INVALID_HANDLE_VALUE) {
            std::cerr << "[CertManager] Failed to open import file" << std::endl;
            return false;
        }
        
        DWORD fileSize = GetFileSize(file, NULL);
        std::vector<BYTE> fileData(fileSize);
        DWORD read;
        ReadFile(file, fileData.data(), fileSize, &read, NULL);
        CloseHandle(file);
        
        // Parse PFX
        CRYPT_DATA_BLOB pfxBlob = {};
        pfxBlob.cbData = fileSize;
        pfxBlob.pbData = fileData.data();
        
        HCERTSTORE pfxStore = PFXImportCertStore(
            &pfxBlob,
            password,
            CRYPT_EXPORTABLE
        );
        
        if (!pfxStore) {
            std::cerr << "[CertManager] Failed to import PFX" << std::endl;
            return false;
        }
        
        // Copy certificates to our store
        PCCERT_CONTEXT cert = nullptr;
        int imported = 0;
        
        while ((cert = CertEnumCertificatesInStore(pfxStore, cert)) != nullptr) {
            if (CertAddCertificateContextToStore(
                certStore_,
                cert,
                CERT_STORE_ADD_REPLACE_EXISTING,
                NULL
            )) {
                imported++;
            }
        }
        
        CertCloseStore(pfxStore, 0);
        
        std::cout << "[CertManager] Imported " << imported << " certificate(s)" << std::endl;
        return imported > 0;
    }
    
    // Delete certificate
    bool DeleteCertificate(LPCWSTR subjectName) {
        PCCERT_CONTEXT cert = FindCertificate(subjectName);
        if (!cert) {
            std::cerr << "[CertManager] Certificate not found for deletion" << std::endl;
            return false;
        }
        
        bool result = CertDeleteCertificateFromStore(cert);
        if (result) {
            std::cout << "[CertManager] Certificate deleted" << std::endl;
        } else {
            std::cerr << "[CertManager] Failed to delete certificate" << std::endl;
        }
        
        return result;
    }
    
    // List all certificates
    void ListCertificates() {
        if (!certStore_) return;
        
        std::cout << "[CertManager] Certificates in store:" << std::endl;
        
        PCCERT_CONTEXT cert = nullptr;
        int count = 0;
        
        while ((cert = CertEnumCertificatesInStore(certStore_, cert)) != nullptr) {
            // Get subject
            DWORD subjectSize = CertGetNameString(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, NULL, 0);
            std::vector<wchar_t> subject(subjectSize);
            CertGetNameString(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, subject.data(), subjectSize);
            
            // Get issuer
            DWORD issuerSize = CertGetNameString(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, CERT_NAME_ISSUER_FLAG, NULL, NULL, 0);
            std::vector<wchar_t> issuer(issuerSize);
            CertGetNameString(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, CERT_NAME_ISSUER_FLAG, NULL, issuer.data(), issuerSize);
            
            // Get expiration
            FILETIME ftExpiry = cert->pCertInfo->NotAfter;
            SYSTEMTIME stExpiry;
            FileTimeToSystemTime(&ftExpiry, &stExpiry);
            
            std::wcout << L"  [" << ++count << L"] " << subject.data() << std::endl;
            std::wcout << L"      Issuer: " << issuer.data() << std::endl;
            std::wcout << L"      Expires: " << stExpiry.wYear << L"-" << stExpiry.wMonth << L"-" << stExpiry.wDay << std::endl;
            std::cout << std::endl;
        }
        
        if (count == 0) {
            std::cout << "  (No certificates found)" << std::endl;
        }
    }
    
    // Get certificate for TLS (Schannel)
    PCCERT_CONTEXT GetCertificateForTLS(LPCWSTR subjectName) {
        return FindCertificate(subjectName);
    }

private:
    HCERTSTORE certStore_;
};

// C interface for integration
extern "C" {
    __declspec(dllexport) void* CertManager_Create() {
        return new CertificateManager();
    }
    
    __declspec(dllexport) bool CertManager_Init(void* mgr) {
        return ((CertificateManager*)mgr)->Initialize();
    }
    
    __declspec(dllexport) bool CertManager_CreateSelfSigned(void* mgr, const wchar_t* subject, const wchar_t* friendlyName, int years) {
        return ((CertificateManager*)mgr)->CreateSelfSignedCertificate(subject, friendlyName, years);
    }
    
    __declspec(dllexport) bool CertManager_Export(void* mgr, const wchar_t* subject, const wchar_t* path, bool includePrivateKey) {
        return ((CertificateManager*)mgr)->ExportCertificate(subject, path, includePrivateKey);
    }
    
    __declspec(dllexport) bool CertManager_Import(void* mgr, const wchar_t* path, const wchar_t* password) {
        return ((CertificateManager*)mgr)->ImportCertificate(path, password);
    }
    
    __declspec(dllexport) bool CertManager_Delete(void* mgr, const wchar_t* subject) {
        return ((CertificateManager*)mgr)->DeleteCertificate(subject);
    }
    
    __declspec(dllexport) void CertManager_List(void* mgr) {
        ((CertificateManager*)mgr)->ListCertificates();
    }
    
    __declspec(dllexport) void CertManager_Destroy(void* mgr) {
        delete (CertificateManager*)mgr;
    }
}
