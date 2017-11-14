#include <windows.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <winternl.h>
#include <strsafe.h>
#include "AppxSignature.hpp"
#include "FileStream.hpp"
#include "SignatureValidator.hpp"

namespace xPlat
{
    struct unique_local_alloc_deleter {
        void operator()(HLOCAL h) const {
            LocalFree(h);
        };
    };
    
    struct unique_cert_context_deleter {
        void operator()(PCCERT_CONTEXT p) const {
            CertFreeCertificateContext(p);
        };
    };

    struct unique_cert_chain_deleter {
        void operator()(PCCERT_CHAIN_CONTEXT p) const {
            CertFreeCertificateChain(p);
        };
    };
    struct unique_hash_handle_deleter {
        void operator()(BCRYPT_HASH_HANDLE h) const {
            BCryptDestroyHash(h);
        };
    };

    struct unique_alg_handle_deleter {
        void operator()(BCRYPT_ALG_HANDLE h) const {
            BCryptCloseAlgorithmProvider(h, 0);
        };
    };

    struct unique_cert_store_handle_deleter {
        void operator()(HCERTSTORE h) const {
            CertCloseStore(h, 0);
        };
    };

    struct unique_crypt_msg_handle_deleter {
        void operator()(HCRYPTMSG h) const {
            CryptMsgClose(h);
        };
    };
    
    typedef std::unique_ptr<void, unique_local_alloc_deleter> unique_local_alloc_handle;
    typedef std::unique_ptr<const CERT_CONTEXT, unique_cert_context_deleter> unique_cert_context;
    typedef std::unique_ptr<const CERT_CHAIN_CONTEXT, unique_cert_chain_deleter> unique_cert_chain_context;
    typedef std::unique_ptr<void, unique_hash_handle_deleter> unique_hash_handle;
    typedef std::unique_ptr<void, unique_alg_handle_deleter> unique_alg_handle;
    typedef std::unique_ptr<void, unique_cert_store_handle_deleter> unique_cert_store_handle;
    typedef std::unique_ptr<void, unique_crypt_msg_handle_deleter> unique_crypt_msg_handle;

 static PCCERT_CHAIN_CONTEXT GetCertChainContext(
        _In_ byte* signatureBuffer,
        _In_ ULONG cbSignatureBuffer)
    {
        PCCERT_CHAIN_CONTEXT certChainContext;
        HCERTSTORE certStoreT;
        HCRYPTMSG signedMessageT;
        DWORD queryContentType = 0;
        DWORD queryFormatType = 0;
        BOOL result;

        CRYPT_DATA_BLOB signatureBlob = { 0 };
        signatureBlob.cbData = cbSignatureBuffer;
        signatureBlob.pbData = signatureBuffer;

        // Get the cert content
        result = CryptQueryObject(
            CERT_QUERY_OBJECT_BLOB,
            &signatureBlob,
            CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED,
            CERT_QUERY_FORMAT_FLAG_BINARY,
            0,      // Reserved parameter
            NULL,   // No encoding info needed
            NULL,
            NULL,
            &certStoreT,
            &signedMessageT,
            NULL);

        if (!result)
            throw xPlat::Exception(xPlat::Error::AppxSignatureInvalid);

        unique_cert_store_handle certStore(certStoreT);
        unique_crypt_msg_handle signedMessage(signedMessageT);

        // Get the signer size and information from the signed data message
        // The properties of the signer info will be used to uniquely identify the signing certificate in the certificate store
        CMSG_SIGNER_INFO* signerInfo = NULL;
        DWORD signerInfoSize = 0;
        result = CryptMsgGetParam(signedMessage.get(), CMSG_SIGNER_INFO_PARAM, 0, NULL, &signerInfoSize);
        if (!result)
            throw xPlat::Exception(xPlat::Error::AppxSignatureInvalid);

        // Check that the signer info size is within reasonable bounds; under the max length of a string for the issuer field
        if (signerInfoSize == 0 || signerInfoSize >= STRSAFE_MAX_CCH)
            throw xPlat::Exception(xPlat::Error::AppxSignatureInvalid);

        std::vector<byte> signerInfoBuffer(signerInfoSize);

        signerInfo = reinterpret_cast<CMSG_SIGNER_INFO*>(signerInfoBuffer.data());
        result = CryptMsgGetParam(signedMessage.get(), CMSG_SIGNER_INFO_PARAM, 0, signerInfo, &signerInfoSize);
        if (!result)
            throw xPlat::Exception(xPlat::Error::AppxSignatureInvalid);

        // Get the signing certificate from the certificate store based on the issuer and serial number of the signer info
        CERT_INFO certInfo;
        certInfo.Issuer = signerInfo->Issuer;
        certInfo.SerialNumber = signerInfo->SerialNumber;

        unique_cert_context signingCertContext(CertGetSubjectCertificateFromStore(
            certStore.get(),
            X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
            &certInfo));

        if (signingCertContext.get() == NULL)
            throw xPlat::Exception(xPlat::Error::AppxSignatureInvalid);

        CERT_CHAIN_PARA certChainParameters = { 0 };
        certChainParameters.cbSize = sizeof(CERT_CHAIN_PARA);
        certChainParameters.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;

        // Do not connect online for URL retrievals.
        // Note that this check does not respect the lifetime signing EKU on the signing certificate
        DWORD certChainFlags = CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL;

        // Get the signing certificate chain context
        result = CertGetCertificateChain(
            HCCE_LOCAL_MACHINE,
            signingCertContext.get(),
            NULL,   // Use the current system time for CRL validation
            certStore.get(),
            &certChainParameters,
            certChainFlags,
            NULL,   // Reserved parameter; must be NULL
            &certChainContext);

        if (!result)
            throw xPlat::Exception(xPlat::Error::AppxSignatureInvalid);

        return certChainContext;
    }

static bool GetEnhancedKeyUsage(
        PCCERT_CONTEXT pCertContext,
        std::vector<std::string>& values)
    {
        HRESULT hr = S_OK;
        DWORD cbExtensionUsage = 0;
        DWORD cbPropertyUsage = 0;
        DWORD i;
        std::vector<byte> extensionUsage(0);
        std::vector<byte> propertyUsage(0);
        
        bool result = CertGetEnhancedKeyUsage(
            pCertContext,
            CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG,
            NULL,
            &cbExtensionUsage);

        if (result)
        {
            extensionUsage.resize(cbExtensionUsage);
            
            result = CertGetEnhancedKeyUsage(
                pCertContext,
                CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG,
                reinterpret_cast<PCERT_ENHKEY_USAGE>(extensionUsage.data()),
                &cbExtensionUsage);
        }

        if (!result && GetLastError() != CRYPT_E_NOT_FOUND)
            throw xPlat::Exception(xPlat::Error::AppxSignatureInvalid);
                
        result = CertGetEnhancedKeyUsage(
            pCertContext,
            CERT_FIND_PROP_ONLY_ENHKEY_USAGE_FLAG,
            NULL,
            &cbPropertyUsage);
        
        if (result)
        {
            propertyUsage.resize(cbPropertyUsage);

            result = CertGetEnhancedKeyUsage(
                pCertContext,
                CERT_FIND_PROP_ONLY_ENHKEY_USAGE_FLAG,
                reinterpret_cast<PCERT_ENHKEY_USAGE>(propertyUsage.data()),
                &cbPropertyUsage);
        }

        if (!result && GetLastError() != CRYPT_E_NOT_FOUND)
            throw xPlat::Exception(xPlat::Error::AppxSignatureInvalid);

        result = false;

        //get OIDS from the extension or property
        if (extensionUsage.size() > 0)
        {
            PCERT_ENHKEY_USAGE pExtensionUsageT = reinterpret_cast<PCERT_ENHKEY_USAGE>(extensionUsage.data());
            for (i = 0; i < pExtensionUsageT->cUsageIdentifier; i++)
            {
                std::string sz(pExtensionUsageT->rgpszUsageIdentifier[i]);
                values.push_back(sz);
            }
            result = (pExtensionUsageT->cUsageIdentifier > 0);
        }
        else
        if (propertyUsage.size() > 0)
        {
            PCERT_ENHKEY_USAGE pPropertyUsageT = reinterpret_cast<PCERT_ENHKEY_USAGE>(propertyUsage.data());
            for (i = 0; i < pPropertyUsageT->cUsageIdentifier; i++)
            {
                std::string sz(pPropertyUsageT->rgpszUsageIdentifier[i]);
                values.push_back(sz);
            }
            result = (pPropertyUsageT->cUsageIdentifier > 0);
        }
        return result;
    }

    static BOOL IsMicrosoftTrustedChain(_In_ PCCERT_CHAIN_CONTEXT certChainContext)
    {
        // Validate that the certificate chain is rooted in one of the well-known MS root certs
        CERT_CHAIN_POLICY_PARA policyParameters = { 0 };
        policyParameters.cbSize = sizeof(CERT_CHAIN_POLICY_PARA);
        CERT_CHAIN_POLICY_STATUS policyStatus = { 0 };
        policyStatus.cbSize = sizeof(CERT_CHAIN_POLICY_STATUS);

        policyParameters.dwFlags = MICROSOFT_ROOT_CERT_CHAIN_POLICY_CHECK_APPLICATION_ROOT_FLAG;

        return CertVerifyCertificateChainPolicy(
            CERT_CHAIN_POLICY_MICROSOFT_ROOT,
            certChainContext,
            &policyParameters,
            &policyStatus);      
    }

    static BOOL IsAuthenticodeTrustedChain(_In_ PCCERT_CHAIN_CONTEXT certChainContext)
    {
        // Validate that the certificate chain is rooted in one of the well-known MS root certs
        CERT_CHAIN_POLICY_PARA policyParameters = { 0 };
        policyParameters.cbSize = sizeof(CERT_CHAIN_POLICY_PARA);
        CERT_CHAIN_POLICY_STATUS policyStatus = { 0 };
        policyStatus.cbSize = sizeof(CERT_CHAIN_POLICY_STATUS);

        //policyParameters.dwFlags = MICROSOFT_ROOT_CERT_CHAIN_POLICY_CHECK_APPLICATION_ROOT_FLAG;

        return CertVerifyCertificateChainPolicy(
            CERT_CHAIN_POLICY_AUTHENTICODE,
            certChainContext,
            &policyParameters,
            &policyStatus);
    }

    static BOOL IsCACert(_In_ PCCERT_CONTEXT pCertContext)
    {
        CERT_BASIC_CONSTRAINTS2_INFO *basicConstraintsT = NULL;
        PCERT_EXTENSION certExtension = NULL;
        DWORD cbDecoded = 0;
        bool retValue = FALSE;

        certExtension = CertFindExtension(
            szOID_BASIC_CONSTRAINTS2,
            pCertContext->pCertInfo->cExtension,
            pCertContext->pCertInfo->rgExtension);

        if (certExtension &&
            CryptDecodeObjectEx(
                X509_ASN_ENCODING,
                X509_BASIC_CONSTRAINTS2,
                certExtension->Value.pbData,
                certExtension->Value.cbData,
                CRYPT_DECODE_ALLOC_FLAG,
                NULL/*pDecodePara*/,
                (LPVOID*)&basicConstraintsT,
                &cbDecoded))
        {
            unique_local_alloc_handle basicConstraints(basicConstraintsT);
            retValue = basicConstraintsT->fCA ? TRUE : FALSE;
        }
        return retValue;
    }

    static BOOL IsCertificateSelfSigned(PCCERT_CONTEXT pContext,
        DWORD dwEncoding,
        DWORD dwFlags)
    {
        if (!(pContext) ||
            (dwFlags != 0))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return(FALSE);
        }

        if (!(CertCompareCertificateName(dwEncoding,
            &pContext->pCertInfo->Issuer,
            &pContext->pCertInfo->Subject)))
        {
            return(FALSE);
        }

        DWORD   dwFlag = CERT_STORE_SIGNATURE_FLAG;
        if (!(CertVerifySubjectCertificateContext(pContext, pContext, &dwFlag)) ||
            (dwFlag & CERT_STORE_SIGNATURE_FLAG))
        {
            return(FALSE);
        }

        return(TRUE);
    }

static PCCERT_CONTEXT GetCertContext(BYTE *signatureBuffer, ULONG cbSignatureBuffer)
    {
        HRESULT hr = S_OK;
        BOOL result;
        DWORD dwExpectedContentType = CERT_QUERY_CONTENT_FLAG_CERT |
            CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED |
            CERT_QUERY_CONTENT_FLAG_PKCS7_UNSIGNED;
        HCERTSTORE certStoreHandleT = NULL;
        DWORD dwContentType = 0;

        //get cert blob out
        PCCERT_CONTEXT pCertContext = NULL;
        CERT_BLOB blob;
        blob.pbData = signatureBuffer;
        blob.cbData = cbSignatureBuffer;

        //get cert context from strCertificate;
        result = CryptQueryObject(
            CERT_QUERY_OBJECT_BLOB,
            &blob,
            dwExpectedContentType,
            CERT_QUERY_FORMAT_FLAG_ALL,
            0,
            NULL,
            &dwContentType,
            NULL,
            &certStoreHandleT,
            NULL,
            NULL);

        if (!result)
            throw xPlat::Exception(xPlat::Error::AppxSignatureInvalid);

        unique_cert_store_handle certStoreHandle(certStoreHandleT);

        if (dwContentType == CERT_QUERY_CONTENT_CERT)
        {
            //get the certificate context
            pCertContext = CertEnumCertificatesInStore(certStoreHandle.get(), NULL);
        }
        else //pkcs7
        {
            //get the end entity
            while (NULL != (pCertContext = CertEnumCertificatesInStore(certStoreHandle.get(), pCertContext)))
            {
                if (IsCertificateSelfSigned(pCertContext, pCertContext->dwCertEncodingType, 0) ||
                    IsCACert(pCertContext))
                {
                    continue;
                }
                else
                {
                    //end entity cert
                    break;
                }
            }
        }
        return pCertContext;
    }
    
    static BOOL DoesSignatureCertContainStoreEKU(
        _In_ byte* rawSignatureBuffer,
        _In_ ULONG dataSize)
    {
        unique_cert_context certificateContext(GetCertContext(rawSignatureBuffer, dataSize));
        
        std::vector<std::string> oids;
        bool result = GetEnhancedKeyUsage(certificateContext.get(), oids);

        if (result)
        {
            std::size_t count = oids.size();
            for (std::size_t i = 0; i < count; i++)
            {
                if (0 == oids.at(i).compare(OID::WindowsStore))
                {   return TRUE;
                }
            }
        }
        return FALSE;
    }

   // Best effort to determine whether the signature file is associated with a store cert
     static BOOL IsStoreOrigin(byte* signatureBuffer, ULONG cbSignatureBuffer)
    {
        BOOL retValue = FALSE;
        if (DoesSignatureCertContainStoreEKU(signatureBuffer, cbSignatureBuffer))
        {
            unique_cert_chain_context certChainContext(GetCertChainContext(signatureBuffer, cbSignatureBuffer));
            retValue = IsMicrosoftTrustedChain(certChainContext.get());
        }
        return retValue;
    }

    // Best effort to determine whether the signature file is associated with a store cert
    static BOOL IsAuthenticodeOrigin(byte* signatureBuffer, ULONG cbSignatureBuffer)
    {
        BOOL retValue = FALSE;
        {
            unique_cert_chain_context certChainContext(GetCertChainContext(signatureBuffer, cbSignatureBuffer));
            retValue = IsAuthenticodeTrustedChain(certChainContext.get());
        }
        return retValue;
    }

    bool SignatureValidator::Validate(
        /*in*/ APPX_VALIDATION_OPTION option, 
        /*in*/ IStream *stream, 
        /*inout*/ std::map<xPlat::AppxSignatureObject::DigestName, xPlat::AppxSignatureObject::Digest>& digests)
    {
        // If the caller wants to skip signature validation altogether, just bug out early. We will not read the digests
        if (option & APPX_VALIDATION_OPTION_SKIPSIGNATURE) { return false; }

        LARGE_INTEGER li = {0};
        ULARGE_INTEGER uli = {0};
        ThrowHrIfFailed(stream->Seek(li, StreamBase::Reference::END, &uli));
        ThrowErrorIf(Error::AppxSignatureInvalid, (uli.QuadPart <= sizeof(P7X_FILE_ID) || uli.QuadPart > (2 << 20)), "stream is too big");
        ThrowHrIfFailed(stream->Seek(li, StreamBase::Reference::START, &uli));

        DWORD fileID = 0;
        ThrowHrIfFailed(stream->Read(&fileId, sizeof(fileID), nullptr));
        ThrowErrorIf(Error::AppxSignatureInvalid, (fileID != P7X_FILE_ID), "unexpected p7x header");

        std::uint32_t streamSize = uli.u.LowPart - sizeof(fileId);
        std::vector<std::uint8_t> buffer(streamSize);
        ULONG actualRead = 0;
        ThrowHrIfFailed(stream->Read(buffer.data(), streamSize, &actualRead));
        ThrowErrorIf(Error::AppxSignatureInvalid, (actualRead != streamSize), "read error");

        //TODO: this code does not read the digests yet
        ThrowErrorIfNot(Error::AppxSignatureInvalid, (
            IsStoreOrigin(buffer.data(), buffer.size()) ||
            IsAuthenticodeOrigin(buffer.data(), buffer.size()) ||
            (option & APPX_VALIDATION_OPTION_ALLOWUNKNOWNORIGIN)
        ), "Signature origin check failed");
        return true;
    }
}



