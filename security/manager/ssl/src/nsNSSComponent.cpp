/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * The contents of this file are subject to the Mozilla Public
 * License Version 1.1 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of
 * the License at http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation. All
 * Rights Reserved.
 *
 * Contributor(s):
 *   Hubbie Shaw
 *   Doug Turner <dougt@netscape.com>
 *   Mitch Stoltz <mstoltz@netscape.com>
 *   Brian Ryner <bryner@netscape.com>
 */

#include "nsNSSComponent.h"
#include "nsNSSCallbacks.h"

#include "nsNetUtil.h"
#include "nsAppDirectoryServiceDefs.h"
#include "nsDirectoryService.h"
#include "nsIStreamListener.h"
#include "nsIStringBundle.h"
#include "nsIDirectoryService.h"
#include "nsDirectoryServiceDefs.h"
#include "nsIProxyObjectManager.h"
#include "nsINSSDialogs.h"
#include "prlog.h"

#include "nss.h"
#include "pk11func.h"
#include "ssl.h"
#include "sslproto.h"
#include "secmod.h"
extern "C" {
#include "pkcs11.h"
}

#ifdef PR_LOGGING
PRLogModuleInfo* gPIPNSSLog = nsnull;
#endif

static NS_DEFINE_CID(kCStringBundleServiceCID, NS_STRINGBUNDLESERVICE_CID);
PRBool nsNSSComponent::mNSSInitialized = PR_FALSE;

#ifdef XP_MAC
extern OSErr ConvertMacPathToUnixPath(const char *macPath, char **unixPath);

OSErr ConvertMacPathToUnixPath(const char *macPath, char **unixPath)
{
  PRIntn len;
  char *cursor;
  
  len = PL_strlen(macPath);
  cursor = (char*)PR_Malloc(len+2);
  if (!cursor)
    return memFullErr;
    
  memcpy(cursor+1, macPath, len+1);
  *unixPath = cursor;
  *cursor = '/';
  while ((cursor = PL_strchr(cursor, ':')) != NULL) {
    *cursor = '/';
    cursor++;
  }
  return noErr;
}
#endif

#define PIPNSS_STRBUNDLE_URL "chrome://pipnss/locale/pipnss.properties"


nsNSSComponent::nsNSSComponent()
{
  NS_INIT_ISUPPORTS();
}

nsNSSComponent::~nsNSSComponent()
{
  if (mNSSInitialized)
    NSS_Shutdown();  
}

#ifdef XP_MAC
#ifdef DEBUG
#define LOADABLE_CERTS_MODULE "NSSckbiDebug.shlb"
#else
#define LOADABLE_CERTS_MODULE "NSSckbi.shlb"
#endif /*DEBUG*/ 
#endif /*XP_MAC*/

NS_IMETHODIMP
nsNSSComponent::PIPBundleFormatStringFromName(const PRUnichar *name,
                                              const PRUnichar **params,
                                              PRUint32 numParams,
                                              PRUnichar **outString)
{
  nsresult rv = NS_ERROR_FAILURE;

  if (mPIPNSSBundle && name) {
    rv = mPIPNSSBundle->FormatStringFromName(name, params, 
                                             numParams, outString);
  }
  return rv;
}

NS_IMETHODIMP
nsNSSComponent::GetPIPNSSBundleString(const PRUnichar *name,
                                      nsString &outString)
{
  PRUnichar *ptrv = nsnull;
  if (mPIPNSSBundle && name) {
    nsresult rv = mPIPNSSBundle->GetStringFromName(name, &ptrv);
    if (NS_SUCCEEDED(rv)) {
      outString = ptrv;
      return NS_OK;
    } else {
      outString.SetLength(0);
    }
    nsMemory::Free(ptrv);
  } else {
    outString.SetLength(0);
  }
  return NS_ERROR_FAILURE;
}

void
nsNSSComponent::InstallLoadableRoots()
{
  PRBool hasRoot = PR_FALSE;
  PK11SlotListElement *listElement;
  PK11SlotList *slotList = PK11_GetAllTokens(CKM_INVALID_MECHANISM, 
                                             PR_FALSE, PR_FALSE, nsnull); 
  if (slotList) {
    for (listElement=slotList->head; listElement != NULL; 
         listElement = listElement->next) {
      if (PK11_HasRootCerts(listElement->slot)) {
        hasRoot = PR_TRUE;
        break;
      }    
    }     
  }
  if (!hasRoot) {
    nsresult rv;
    nsString modName;
    rv = GetPIPNSSBundleString(NS_LITERAL_STRING("RootCertModuleName"),
                               modName);
    if (NS_FAILED(rv)) return;

    nsCOMPtr<nsILocalFile> mozFile;

    NS_WITH_SERVICE(nsIProperties, directoryService, NS_DIRECTORY_SERVICE_CONTRACTID, &rv);									
    if (NS_FAILED(rv)) {
        return ;
    }    
    										
    directoryService->Get( NS_XPCOM_CURRENT_PROCESS_DIR,
                           NS_GET_IID(nsIFile), 
                           getter_AddRefs(mozFile));
    
    if (!mozFile) {
      return;
    }
    char *fullModuleName = nsnull;
#ifdef XP_MAC
    char *unixModulePath=nsnull;
    mozFile->Append("Essential Files");
    mozFile->Append(LOADABLE_CERTS_MODULE);
    mozFile->GetPath(&fullModuleName);
    ConvertMacPathToUnixPath(fullModuleName, &unixModulePath);
    nsMemory::Free(fullModuleName);
    fullModuleName = unixModulePath;
#else
    char *processDir = nsnull;
    mozFile->GetPath(&processDir);
    fullModuleName = PR_GetLibraryName(processDir, "nssckbi");
    nsMemory::Free(processDir);
#endif
    /* If a module exists with the same name, delete it. */
    char *modNameCString = modName.ToNewCString();
    int modType;
    SECMOD_DeleteModule(modNameCString, &modType);
    SECMOD_AddNewModule(modNameCString, fullModuleName, 0, 0);
    nsMemory::Free(fullModuleName);
    nsMemory::Free(modNameCString);
  }
}

#define SHORT_PK11_STRING 33
#define LONG_PK11_STRING  65

char *
nsNSSComponent::GetPK11String(const PRUnichar *name, PRUint32 len)
{
  nsresult rv;
  nsString nsstr;
  char *tmpstr = NULL;
  char *str = NULL;
  int tmplen;
  str = (char *)PR_Malloc(len+1);
  rv = GetPIPNSSBundleString(name, nsstr);
  if (NS_FAILED(rv)) return NULL;
  tmpstr = nsstr.ToNewCString();
  if (!tmpstr) return NULL;
  tmplen = strlen(tmpstr);
  memcpy(str, tmpstr, tmplen);
  memset(str + tmplen, ' ', len - tmplen);
  str[len] = '\0';
  PR_Free(tmpstr);
  return str;
}

nsresult
nsNSSComponent::ConfigureInternalPKCS11Token()
{
  char *manufacturerID             = NULL;
  char *libraryDescription         = NULL;
  char *tokenDescription           = NULL;
  char *privateTokenDescription    = NULL;
  char *slotDescription            = NULL;
  char *privateSlotDescription     = NULL;
  char *fipsSlotDescription        = NULL;
  char *fipsPrivateSlotDescription = NULL; 

  manufacturerID = GetPK11String(NS_LITERAL_STRING("ManufacturerID"), 
                                 SHORT_PK11_STRING);
  if (manufacturerID == NULL) goto loser;
  libraryDescription = GetPK11String(NS_LITERAL_STRING("LibraryDescription"), 
                                     SHORT_PK11_STRING);
  if (libraryDescription == NULL) goto loser;
  tokenDescription = GetPK11String(NS_LITERAL_STRING("TokenDescription"), 
                                   SHORT_PK11_STRING);
  if (tokenDescription == NULL) goto loser;
  privateTokenDescription = 
                    GetPK11String(NS_LITERAL_STRING("PrivateTokenDescription"), 
                                  SHORT_PK11_STRING);
  if (privateTokenDescription == NULL) goto loser;
  slotDescription = GetPK11String(NS_LITERAL_STRING("SlotDescription"), 
                                  LONG_PK11_STRING);
  if (slotDescription == NULL) goto loser;
  privateSlotDescription = 
                     GetPK11String(NS_LITERAL_STRING("PrivateSlotDescription"), 
                                   LONG_PK11_STRING);
  if (privateSlotDescription == NULL) goto loser;
  fipsSlotDescription = GetPK11String(NS_LITERAL_STRING("FipsSlotDescription"),
                                      LONG_PK11_STRING);
  if (fipsSlotDescription == NULL) goto loser;
  fipsPrivateSlotDescription = 
                 GetPK11String(NS_LITERAL_STRING("FipsPrivateSlotDescription"), 
                               LONG_PK11_STRING);
  if (fipsPrivateSlotDescription == NULL) goto loser;

  PK11_ConfigurePKCS11(manufacturerID, libraryDescription, tokenDescription,
                       privateTokenDescription, slotDescription, 
                       privateSlotDescription, fipsSlotDescription, 
                       fipsPrivateSlotDescription, 0, 0);
  return NS_OK;
loser:
  PR_Free(manufacturerID);
  PR_Free(libraryDescription);
  PR_Free(tokenDescription);
  PR_Free(privateTokenDescription);
  PR_Free(slotDescription);
  PR_Free(privateSlotDescription);
  PR_Free(fipsSlotDescription);
  PR_Free(fipsPrivateSlotDescription); 
  return NS_ERROR_FAILURE;
}

nsresult
nsNSSComponent::InitializePIPNSSBundle()
{
  nsresult rv;
  nsCOMPtr<nsIStringBundleService> bundleService(do_GetService(NS_STRINGBUNDLE_CONTRACTID, &rv));
  if (NS_FAILED(rv) || !bundleService) 
    return NS_ERROR_FAILURE;
  
  bundleService->CreateBundle(PIPNSS_STRBUNDLE_URL, nsnull,
                              getter_AddRefs(mPIPNSSBundle));
  if (!mPIPNSSBundle)
    rv = NS_ERROR_FAILURE;

  return rv;
}

nsresult
nsNSSComponent::InitializeNSS()
{
  nsresult rv;
  nsXPIDLCString profileStr;
  nsCOMPtr<nsIFile> profilePath;

  if (mNSSInitialized) {
    PR_ASSERT(!"Trying to initialize NSS twice"); // We should never try to 
                                                  // initialize NSS more than
                                                  // once in a process.
    return NS_ERROR_FAILURE;
  }

  PR_LOG(gPIPNSSLog, PR_LOG_DEBUG, ("NSS Initialization beginning\n"));
  rv = NS_GetSpecialDirectory(NS_APP_USER_PROFILE_50_DIR,
                              getter_AddRefs(profilePath));
  if (NS_FAILED(rv)) {
    PR_LOG(gPIPNSSLog, PR_LOG_ERROR, ("Unable to get profile directory\n"));
    return rv;
  }
    
  PK11_SetPasswordFunc(PK11PasswordPrompt);
#ifdef XP_MAC
  // On the Mac we place all NSS DBs in the Security
  // Folder in the profile directory.
  profilePath->Append("Security");
  profilePath->Create(nsIFile::DIRECTORY_TYPE, 0); //This is for Mac, don't worry about
                                                   //permissions.
#endif 

  rv = profilePath->GetPath(getter_Copies(profileStr));
  if (NS_FAILED(rv)) 
    return rv;
     
  NSS_InitReadWrite(profileStr);
  NSS_SetDomesticPolicy();
  //  SSL_EnableCipher(SSL_RSA_WITH_NULL_MD5, SSL_ALLOWED);
    
  // XXX should use prefs
  SSL_OptionSetDefault(SSL_ENABLE_SSL2, PR_TRUE);
  SSL_OptionSetDefault(SSL_ENABLE_SSL3, PR_TRUE);
  SSL_OptionSetDefault(SSL_ENABLE_TLS, PR_TRUE);
  PR_LOG(gPIPNSSLog, PR_LOG_DEBUG, ("NSS Initialization done\n"));
  return NS_OK;
}

NS_IMETHODIMP
nsNSSComponent::Init()
{
  nsresult rv = NS_OK;
#ifdef PR_LOGGING
  if (!gPIPNSSLog)
    gPIPNSSLog = PR_NewLogModule("pipnss");
#endif

  PR_LOG(gPIPNSSLog, PR_LOG_DEBUG, ("Beginning NSS initialization\n"));
  rv = InitializeNSS();
  if (NS_FAILED(rv)) {
    PR_LOG(gPIPNSSLog, PR_LOG_ERROR, ("Unable to Initialize NSS.\n"));
    return rv;
  }

  rv = InitializePIPNSSBundle();
  if (NS_FAILED(rv)) {
    PR_LOG(gPIPNSSLog, PR_LOG_ERROR, ("Unable to create pipnss bundle.\n"));
    return rv;
  }      
  ConfigureInternalPKCS11Token();
  InstallLoadableRoots();
  return rv;
}

/* nsISupports Implementation for the class */
NS_IMPL_THREADSAFE_ISUPPORTS5(nsNSSComponent,
                              nsISecurityManagerComponent,
                              nsIContentHandler,
                              nsISignatureVerifier,
                              nsIEntropyCollector,
                              nsINSSComponent);


NS_IMETHODIMP
nsNSSComponent::DisplaySecurityAdvisor()
{
  return NS_ERROR_FAILURE; // not implemented
}

typedef enum {
    PSMCertificateUnknown = -1,
    PSMCertificateCACert,
    PSMCertificateServerCert,
    PSMCertificateUserCert,
    PSMCertificateEmailCert
} PSMCertificateType;

class CertDownloader : public nsIStreamListener
{
public:
  CertDownloader() {NS_ASSERTION(PR_FALSE, "don't use this constructor."); }
  CertDownloader(PSMCertificateType type);
  virtual ~CertDownloader();
  
  NS_DECL_ISUPPORTS
  NS_DECL_NSISTREAMOBSERVER
  NS_DECL_NSISTREAMLISTENER
protected:
  char* mByteData;
  PRInt32 mBufferOffset;
  PRInt32 mContentLength;
  PSMCertificateType mType;
};


CertDownloader::CertDownloader(PSMCertificateType type)
  : mByteData(nsnull),
    mType(type)
{
  NS_INIT_ISUPPORTS();
}

CertDownloader::~CertDownloader()
{
  if (mByteData)
    nsMemory::Free(mByteData);
}

NS_IMPL_ISUPPORTS1(CertDownloader, nsIStreamListener);

NS_IMETHODIMP
CertDownloader::OnStartRequest(nsIChannel* channel, nsISupports* context)
{
  channel->GetContentLength(&mContentLength);
  if (mContentLength == -1)
    return NS_ERROR_FAILURE;
  
  mBufferOffset = 0;
  mByteData = (char*) nsMemory::Alloc(mContentLength);
  if (!mByteData)
    return NS_ERROR_OUT_OF_MEMORY;
  
  return NS_OK;
}


NS_IMETHODIMP
CertDownloader::OnDataAvailable(nsIChannel* channel,
                                nsISupports* context,
                                nsIInputStream *aIStream,
                                PRUint32 aSourceOffset,
                                PRUint32 aLength)
{
  if (!mByteData)
    return NS_ERROR_OUT_OF_MEMORY;
  
  PRUint32 amt;
  nsresult err;
  
  do {
    err = aIStream->Read(mByteData+mBufferOffset,
                         mContentLength-mBufferOffset, &amt);
    if (amt == 0) break;
    if (NS_FAILED(err)) return err;
    
    aLength -= amt;
    mBufferOffset += amt;
    
  } while (aLength > 0);
  
  return NS_OK;
}


NS_IMETHODIMP
CertDownloader::OnStopRequest(nsIChannel* channel,
                              nsISupports* context,
                              nsresult aStatus,
                              const PRUnichar* aMsg)
{
  return NS_OK;
}


/* other mime types that we should handle sometime:
   
   application/x-pkcs7-crl
   application/x-pkcs7-mime
   application/pkcs7-signature
   application/pre-encrypted
   
*/

NS_IMETHODIMP
nsNSSComponent::HandleContent(const char * aContentType,
                              const char * aCommand,
                              const char * aWindowTarget,
                              nsISupports* aWindowContext,
                              nsIChannel * aChannel)
{
  // We were called via CI.  We better protect ourselves and addref.
  NS_ADDREF_THIS();
  
  nsresult rv = NS_OK;
  if (!aChannel) return NS_ERROR_NULL_POINTER;
  
  PSMCertificateType type;
  
  if (!nsCRT::strcasecmp(aContentType, "application/x-x509-ca-cert"))
    type = PSMCertificateCACert;
  else if (!nsCRT::strcasecmp(aContentType, "application/x-x509-server-cert"))
    type = PSMCertificateServerCert;
  else if (!nsCRT::strcasecmp(aContentType, "application/x-x509-user-cert"))
    type = PSMCertificateUserCert;
  else if (!nsCRT::strcasecmp(aContentType, "application/x-x509-email-cert"))
    type = PSMCertificateEmailCert;
  else
    type = PSMCertificateUnknown;
  
  if (type != PSMCertificateUnknown) {
    nsCOMPtr<nsIURI> uri;
    rv = aChannel->GetURI(getter_AddRefs(uri));
    if (NS_FAILED(rv)) return rv;
    
    nsCOMPtr<nsIChannel> channel;
    rv = NS_OpenURI(getter_AddRefs(channel), uri);
    if (NS_FAILED(rv)) return rv;
    
    return channel->AsyncRead(new CertDownloader(type),
                              NS_STATIC_CAST(nsISecurityManagerComponent*,this));
  }
  
  return NS_ERROR_NOT_IMPLEMENTED;
}


//---------------------------------------------
// Functions Implenenting NSISignatureVerifier
//---------------------------------------------
NS_IMETHODIMP
nsNSSComponent::HashBegin(PRUint32 alg, PRUint32* id)
{
  return NS_OK; /* not sure what the implications of this are */
}

NS_IMETHODIMP
nsNSSComponent::HashUpdate(PRUint32 id, const char* buf, PRUint32 buflen)
{
  return NS_OK; /* not sure what the implications of this are */
}

NS_IMETHODIMP
nsNSSComponent::HashEnd(PRUint32 id, unsigned char** hash, 
                        PRUint32* hashLen, PRUint32 maxLen)
{
  return NS_OK; /* not sure what the implications of this are */
}

NS_IMETHODIMP
nsNSSComponent::CreatePrincipalFromSignature(const char* aRSABuf,
                                             PRUint32 aRSABufLen,
                                             nsIPrincipal** aPrincipal)
{
  PRInt32 errorCode;
  return VerifySignature(aRSABuf, aRSABufLen, nsnull, 0, &errorCode,
                         aPrincipal);
}

NS_IMETHODIMP
nsNSSComponent::GetPassword(char **aRet)
{
  // This functionality is only used in wallet.
  // This interface can go away once we get rid of PSM 1.x.
  *aRet = nsnull;
  return NS_OK;
}

NS_IMETHODIMP
nsNSSComponent::VerifySignature(const char* aRSABuf, PRUint32 aRSABufLen,
                                const char* aPlaintext, PRUint32 aPlaintextLen,
                                PRInt32* aErrorCode,
                                nsIPrincipal** aPrincipal)
{
  return NS_OK;
}

NS_IMETHODIMP
nsNSSComponent::RandomUpdate(void *entropy, PRInt32 bufLen)
{
  PK11_RandomUpdate(entropy, bufLen);
  return NS_OK;
}

static const char *kNSSDialogsContractId = NS_NSSDIALOGS_CONTRACTID;

nsresult 
getNSSDialogs(void **_result, REFNSIID aIID)
{
  nsresult rv;
  nsISupports *result;
  nsCOMPtr<nsISupports> proxiedResult;

  rv = nsServiceManager::GetService(kNSSDialogsContractId, 
                                    NS_GET_IID(nsINSSDialogs),
                                    &result);
  if (NS_FAILED(rv)) 
    return rv;

  nsCOMPtr<nsIProxyObjectManager> proxyman(do_GetService(NS_XPCOMPROXY_CONTRACTID));
  if (!proxyman)
    return NS_ERROR_FAILURE;
 
  proxyman->GetProxyForObject(NS_UI_THREAD_EVENTQ,
                              aIID, result, PROXY_SYNC,
                              getter_AddRefs(proxiedResult));

  rv = proxiedResult->QueryInterface(aIID, _result);

  NS_RELEASE(result);

  return rv;
}
