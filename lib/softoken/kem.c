/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "blapi.h"
#include "kem.h"
#include "pkcs11i.h"
#include "pkcs11n.h"
#include "secerr.h"
#include "secitem.h"
#include "secport.h"
#include "softoken.h"

/* change to the largest KEM Secret Bytes value supported */
#define MAX_SHARED_SECRET_BYTES KYBER_SHARED_SECRET_BYTES

KyberParams
sftk_kyber_PK11ParamToInternal(CK_ML_KEM_PARAMETER_SET_TYPE pk11ParamSet)
{
    switch (pk11ParamSet) {
#ifndef NSS_DISABLE_KYBER
        case CKP_NSS_KYBER_768_ROUND3:
            return params_kyber768_round3;
#endif
        case CKP_NSS_ML_KEM_768:
        case CKP_ML_KEM_768:
            return params_ml_kem768;
        default:
            return params_kyber_invalid;
    }
}

SECItem *
sftk_kyber_AllocPubKeyItem(KyberParams params, SECItem *pubkey)
{
    switch (params) {
#ifndef NSS_DISABLE_KYBER
        case params_kyber768_round3:
        case params_kyber768_round3_test_mode:
#endif
        case params_ml_kem768:
        case params_ml_kem768_test_mode:
            return SECITEM_AllocItem(NULL, pubkey, KYBER768_PUBLIC_KEY_BYTES);
        default:
            return NULL;
    }
}

SECItem *
sftk_kyber_AllocPrivKeyItem(KyberParams params, SECItem *privkey)
{
    switch (params) {
#ifndef NSS_DISABLE_KYBER
        case params_kyber768_round3:
        case params_kyber768_round3_test_mode:
#endif
        case params_ml_kem768:
        case params_ml_kem768_test_mode:
            return SECITEM_AllocItem(NULL, privkey, KYBER768_PRIVATE_KEY_BYTES);
        default:
            return NULL;
    }
}

SECItem *
sftk_kyber_AllocCiphertextItem(KyberParams params, SECItem *ciphertext)
{
    switch (params) {
#ifndef NSS_DISABLE_KYBER
        case params_kyber768_round3:
        case params_kyber768_round3_test_mode:
#endif
        case params_ml_kem768:
        case params_ml_kem768_test_mode:
            return SECITEM_AllocItem(NULL, ciphertext, KYBER768_CIPHERTEXT_BYTES);
        default:
            return NULL;
    }
}

static PRBool
sftk_kem_ValidateMechanism(CK_MECHANISM_PTR pMechanism)
{
    if (!pMechanism) {
        return PR_FALSE;
    }
    switch (pMechanism->mechanism) {
#ifndef NSS_DISABLE_KYBER
        case CKM_NSS_KYBER:
#endif
        case CKM_NSS_ML_KEM:
        case CKM_ML_KEM:
            return PR_TRUE;
        default:
            return PR_FALSE;
    }
}

/* this is a generic call the returns the paramSet as a CKU_LONG. if the
 * param set is in a different attribute or mechanism structure, that would
 * be based on the mechanism. The meaning of the paramter set is alway
 * mechanism specific */
static CK_RV
sftk_kem_getParamSet(CK_MECHANISM_PTR pMechanism, SFTKObject *key,
                     CK_ULONG *paramSet)
{
    CK_RV crv = CKR_MECHANISM_INVALID;

    switch (pMechanism->mechanism) {
#ifndef NSS_DISABLE_KYBER
        case CKM_NSS_KYBER:
#endif
        case CKM_NSS_ML_KEM:
            if ((pMechanism->pParameter) &&
                pMechanism->ulParameterLen == sizeof(CK_ML_KEM_PARAMETER_SET_TYPE)) {
                PR_STATIC_ASSERT(sizeof(CK_ML_KEM_PARAMETER_SET_TYPE) == sizeof(CK_LONG));
                *paramSet = *(CK_ULONG *)pMechanism->pParameter;
                crv = CKR_OK;
                break;
            }
            crv = sftk_GetULongAttribute(key, CKA_NSS_PARAMETER_SET, paramSet);
            break;
        case CKM_ML_KEM:
            crv = sftk_GetULongAttribute(key, CKA_PARAMETER_SET, paramSet);
            break;
        default:
            break;
    }
    return crv;
}

static CK_ULONG
sftk_kem_CiphertextLen(CK_MECHANISM_PTR pMechanism, CK_ULONG paramSet)
{
    /* the switch here is redundant now, but we will eventually have
     * other unrelated KEM operations and the meaning of paramSet
     * is dependent of the mechanism type (in general). Since there
     * is not overlap between the Vendor specific mechanisms here
     * and the stand ones, we'll just accept them all here. */
    switch (pMechanism->mechanism) {
#ifndef NSS_DISABLE_KYBER
        case CKM_NSS_KYBER:
#endif
        case CKM_NSS_ML_KEM:
        case CKM_ML_KEM:
            switch (paramSet) {
#ifndef NSS_DISABLE_KYBER
                case CKP_NSS_KYBER_768_ROUND3:
#endif
                case CKP_NSS_ML_KEM_768:
                case CKP_ML_KEM_768:
                    return KYBER768_CIPHERTEXT_BYTES;
                default:
                    break;
            }
        default:
            break;
    }
    return 0;
}

/* C_Encapsulate takes a public encapsulation key hPublicKey, a secret
 * phKey, and outputs a ciphertext (i.e. encapsulaton) of this secret in
 * pCiphertext. */
CK_RV
NSC_EncapsulateKey(CK_SESSION_HANDLE hSession,
                   CK_MECHANISM_PTR pMechanism,
                   CK_OBJECT_HANDLE hPublicKey,
                   CK_ATTRIBUTE_PTR pTemplate,
                   CK_ULONG ulAttributeCount,
                   /* out */ CK_BYTE_PTR pCiphertext,
                   /* out */ CK_ULONG_PTR pulCiphertextLen,
                   /* out */ CK_OBJECT_HANDLE_PTR phKey)
{
    SFTKSession *session = NULL;
    SFTKSlot *slot = NULL;

    SFTKObject *key = NULL;

    SFTKObject *encapsulationKeyObject = NULL;
    SFTKAttribute *encapsulationKey = NULL;

    CK_RV crv;
    SFTKFreeStatus status;
    CK_ULONG paramSet = 0; /* use a generic U_LONG so we can handle
                            * different param set types based on the
                            * Mechanism value */
    KyberParams kyberParams;

    CHECK_FORK();

    if (!pMechanism || !phKey || !pulCiphertextLen) {
        return CKR_ARGUMENTS_BAD;
    }

    if (!sftk_kem_ValidateMechanism(pMechanism)) {
        return CKR_MECHANISM_INVALID;
    }
    *phKey = CK_INVALID_HANDLE;

    session = sftk_SessionFromHandle(hSession);
    if (session == NULL) {
        return CKR_SESSION_HANDLE_INVALID;
    }
    slot = sftk_SlotFromSessionHandle(hSession);
    if (slot == NULL) {
        crv = CKR_SESSION_HANDLE_INVALID;
        goto cleanup;
    }

    key = sftk_NewObject(slot);
    if (key == NULL) {
        crv = CKR_HOST_MEMORY;
        goto cleanup;
    }
    for (unsigned long int i = 0; i < ulAttributeCount; i++) {
        crv = sftk_AddAttributeType(key, sftk_attr_expand(&pTemplate[i]));
        if (crv != CKR_OK) {
            goto cleanup;
        }
    }

    encapsulationKeyObject = sftk_ObjectFromHandle(hPublicKey, session);
    if (encapsulationKeyObject == NULL) {
        crv = CKR_KEY_HANDLE_INVALID;
        goto cleanup;
    }
    encapsulationKey = sftk_FindAttribute(encapsulationKeyObject, CKA_VALUE);
    if (encapsulationKey == NULL) {
        crv = CKR_KEY_HANDLE_INVALID;
        goto cleanup;
    }

    crv = sftk_kem_getParamSet(pMechanism, encapsulationKeyObject, &paramSet);
    if (crv != CKR_OK) {
        goto cleanup;
    }

    CK_ULONG ciphertextLen = sftk_kem_CiphertextLen(pMechanism, paramSet);
    if (!pCiphertext || *pulCiphertextLen < ciphertextLen || ciphertextLen == 0) {
        *pulCiphertextLen = ciphertextLen;
        crv = CKR_KEY_SIZE_RANGE;
        goto cleanup;
    }

    SECItem ciphertext = { siBuffer, pCiphertext, ciphertextLen };
    SECItem pubKey = { siBuffer, encapsulationKey->attrib.pValue, encapsulationKey->attrib.ulValueLen };

    /* The length of secretBuf can be increased if we ever support other KEMs
     * by changing the define at the top of this file */
    uint8_t secretBuf[MAX_SHARED_SECRET_BYTES] = { 0 };
    SECItem secret = { siBuffer, secretBuf, sizeof secretBuf };

    switch (pMechanism->mechanism) {
#ifndef NSS_DISABLE_KYBER
        case CKM_NSS_KYBER:
#endif
        case CKM_NSS_ML_KEM:
        case CKM_ML_KEM:
            PORT_Assert(secret.len >= KYBER_SHARED_SECRET_BYTES);
            kyberParams = sftk_kyber_PK11ParamToInternal(paramSet);
            SECStatus rv = Kyber_Encapsulate(kyberParams, /* seed */ NULL,
                                             &pubKey, &ciphertext, &secret);
            if (rv != SECSuccess) {
                crv = (PORT_GetError() == SEC_ERROR_INVALID_ARGS) ? CKR_ARGUMENTS_BAD : CKR_FUNCTION_FAILED;
                goto cleanup;
            }

            crv = sftk_forceAttribute(key, CKA_VALUE, sftk_item_expand(&secret));
            if (crv != CKR_OK) {
                goto cleanup;
            }

            crv = sftk_handleObject(key, session);
            if (crv != CKR_OK) {
                goto cleanup;
            }

            /* We wrote the ciphertext out directly in Kyber_Encapsulate */
            *phKey = key->handle;
            *pulCiphertextLen = ciphertext.len;
            break;
        default:
            crv = CKR_MECHANISM_INVALID;
            goto cleanup;
    }

cleanup:
    if (session) {
        sftk_FreeSession(session);
    }
    if (key) {
        status = sftk_FreeObject(key);
        if (status == SFTK_DestroyFailure) {
            return CKR_DEVICE_ERROR;
        }
    }
    if (encapsulationKeyObject) {
        status = sftk_FreeObject(encapsulationKeyObject);
        if (status == SFTK_DestroyFailure) {
            return CKR_DEVICE_ERROR;
        }
    }
    if (encapsulationKey) {
        sftk_FreeAttribute(encapsulationKey);
    }
    return crv;
}

CK_RV
NSC_DecapsulateKey(CK_SESSION_HANDLE hSession,
                   CK_MECHANISM_PTR pMechanism,
                   CK_OBJECT_HANDLE hPrivateKey,
                   CK_ATTRIBUTE_PTR pTemplate,
                   CK_ULONG ulAttributeCount,
                   CK_BYTE_PTR pCiphertext,
                   CK_ULONG ulCiphertextLen,
                   /* out */ CK_OBJECT_HANDLE_PTR phKey)
{
    SFTKSession *session = NULL;
    SFTKSlot *slot = NULL;

    SFTKObject *key = NULL;

    SFTKObject *decapsulationKeyObject = NULL;
    SFTKAttribute *decapsulationKey = NULL;
    CK_ULONG paramSet = 0; /* use a generic U_LONG so we can handle
                            * different param set types based on the
                            * Mechanism value */

    CK_RV crv;
    SFTKFreeStatus status;
    KyberParams kyberParams;

    CHECK_FORK();

    if (!pMechanism || !pCiphertext || !pTemplate || !phKey) {
        return CKR_ARGUMENTS_BAD;
    }

    if (!sftk_kem_ValidateMechanism(pMechanism)) {
        return CKR_MECHANISM_INVALID;
    }
    *phKey = CK_INVALID_HANDLE;

    session = sftk_SessionFromHandle(hSession);
    if (session == NULL) {
        return CKR_SESSION_HANDLE_INVALID;
    }
    slot = sftk_SlotFromSessionHandle(hSession);
    if (slot == NULL) {
        crv = CKR_SESSION_HANDLE_INVALID;
        goto cleanup;
    }

    key = sftk_NewObject(slot);
    if (key == NULL) {
        crv = CKR_HOST_MEMORY;
        goto cleanup;
    }
    for (unsigned long int i = 0; i < ulAttributeCount; i++) {
        crv = sftk_AddAttributeType(key, sftk_attr_expand(&pTemplate[i]));
        if (crv != CKR_OK) {
            goto cleanup;
        }
    }

    decapsulationKeyObject = sftk_ObjectFromHandle(hPrivateKey, session);
    if (decapsulationKeyObject == NULL) {
        crv = CKR_KEY_HANDLE_INVALID;
        goto cleanup;
    }
    decapsulationKey = sftk_FindAttribute(decapsulationKeyObject, CKA_VALUE);
    if (decapsulationKey == NULL) {
        crv = CKR_KEY_HANDLE_INVALID;
        goto cleanup;
    }

    crv = sftk_kem_getParamSet(pMechanism, decapsulationKeyObject, &paramSet);
    if (crv != CKR_OK) {
        goto cleanup;
    }

    CK_ULONG ciphertextLen = sftk_kem_CiphertextLen(pMechanism, paramSet);
    if (!pCiphertext || ulCiphertextLen != ciphertextLen || ciphertextLen == 0) {
        crv = CKR_ARGUMENTS_BAD;
        goto cleanup;
    }

    SECItem privKey = { siBuffer, decapsulationKey->attrib.pValue,
                        decapsulationKey->attrib.ulValueLen };
    SECItem ciphertext = { siBuffer, pCiphertext, ulCiphertextLen };

    /* The length of secretBuf can be increased if we ever support other KEMs
     * by changing the define at the top of this file */
    uint8_t secretBuf[MAX_SHARED_SECRET_BYTES] = { 0 };
    SECItem secret = { siBuffer, secretBuf, sizeof secretBuf };

    switch (pMechanism->mechanism) {
#ifndef NSS_DISABLE_KYBER
        case CKM_NSS_KYBER:
#endif
        case CKM_NSS_ML_KEM:
        case CKM_ML_KEM:
            kyberParams = sftk_kyber_PK11ParamToInternal(paramSet);
            PORT_Assert(secret.len >= KYBER_SHARED_SECRET_BYTES);
            SECStatus rv = Kyber_Decapsulate(kyberParams, &privKey,
                                             &ciphertext, &secret);
            if (rv != SECSuccess) {
                crv = (PORT_GetError() == SEC_ERROR_INVALID_ARGS) ? CKR_ARGUMENTS_BAD : CKR_FUNCTION_FAILED;
                goto cleanup;
            }

            crv = sftk_forceAttribute(key, CKA_VALUE, sftk_item_expand(&secret));
            if (crv != CKR_OK) {
                goto cleanup;
            }

            crv = sftk_handleObject(key, session);
            if (crv != CKR_OK) {
                goto cleanup;
            }
            *phKey = key->handle;
            break;
        default:
            crv = CKR_MECHANISM_INVALID;
            goto cleanup;
    }

cleanup:
    if (session) {
        sftk_FreeSession(session);
    }
    if (key) {
        status = sftk_FreeObject(key);
        if (status == SFTK_DestroyFailure) {
            return CKR_DEVICE_ERROR;
        }
    }
    if (decapsulationKeyObject) {
        status = sftk_FreeObject(decapsulationKeyObject);
        if (status == SFTK_DestroyFailure) {
            return CKR_DEVICE_ERROR;
        }
    }
    if (decapsulationKey) {
        sftk_FreeAttribute(decapsulationKey);
    }
    return crv;
}

/* PKCS #11 final spec moved som the the arguments around (to make
 * NSC_EncapsulateKey and NSC_DecapsulateKey match, keep the old version for
 * the vendor defined for backward compatibility */
CK_RV
NSC_Encapsulate(CK_SESSION_HANDLE hSession,
                CK_MECHANISM_PTR pMechanism,
                CK_OBJECT_HANDLE hPublicKey,
                CK_ATTRIBUTE_PTR pTemplate,
                CK_ULONG ulAttributeCount,
                /* out */ CK_OBJECT_HANDLE_PTR phKey,
                /* out */ CK_BYTE_PTR pCiphertext,
                /* out */ CK_ULONG_PTR pulCiphertextLen)
{
    return NSC_EncapsulateKey(hSession, pMechanism, hPublicKey,
                              pTemplate, ulAttributeCount,
                              pCiphertext, pulCiphertextLen, phKey);
}

CK_RV
NSC_Decapsulate(CK_SESSION_HANDLE hSession,
                CK_MECHANISM_PTR pMechanism,
                CK_OBJECT_HANDLE hPrivateKey,
                CK_BYTE_PTR pCiphertext,
                CK_ULONG ulCiphertextLen,
                CK_ATTRIBUTE_PTR pTemplate,
                CK_ULONG ulAttributeCount,
                /* out */ CK_OBJECT_HANDLE_PTR phKey)
{
    return NSC_DecapsulateKey(hSession, pMechanism, hPrivateKey, pTemplate,
                              ulAttributeCount, pCiphertext, ulCiphertextLen,
                              phKey);
}
