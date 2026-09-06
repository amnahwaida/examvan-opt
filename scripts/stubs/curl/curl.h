// Minimal stub of curl/curl.h — syntax-check only, NOT a real implementation.
// Dipakai scripts/check-docker-paths.sh untuk men-compile jalur
// #ifdef HAS_LIBCURL di mesin dev tanpa libcurl4-openssl-dev.
// Cakupan: hanya simbol yang dipakai sumber examvan (lihat grep CURLOPT_ src/).
#pragma once
#include <stddef.h>

typedef void CURL;
typedef long long curl_off_t;
typedef struct curl_slist curl_slist;

#define CURL_ERROR_SIZE 256

typedef enum {
  CURLE_OK = 0,
  CURLE_UNSUPPORTED_PROTOCOL,
  CURLE_FAILED_INIT,
  CURLE_URL_MALFORMAT,
  CURLE_COULDNT_RESOLVE_PROXY,
  CURLE_COULDNT_RESOLVE_HOST,
  CURLE_COULDNT_CONNECT,
  CURLE_OPERATION_TIMEDOUT
} CURLcode;

typedef enum {
  CURLOPT_URL,
  CURLOPT_POSTFIELDS,
  CURLOPT_TIMEOUT_MS,
  CURLOPT_TIMEOUT,
  CURLOPT_CONNECTTIMEOUT,
  CURLOPT_WRITEFUNCTION,
  CURLOPT_WRITEDATA,
  CURLOPT_READFUNCTION,
  CURLOPT_READDATA,
  CURLOPT_UPLOAD,
  CURLOPT_INFILESIZE_LARGE,
  CURLOPT_HTTPHEADER,
  CURLOPT_SSL_VERIFYPEER,
  CURLOPT_SSL_VERIFYHOST,
  CURLOPT_FOLLOWLOCATION,
  CURLOPT_USERAGENT,
  CURLOPT_NOPROGRESS,
  CURLOPT_CUSTOMREQUEST,
  CURLOPT_NOBODY,
  CURLOPT_ERRORBUFFER,
  CURLOPT_USERNAME,
  CURLOPT_PASSWORD,
  CURLOPT_USE_SSL,
  CURLOPT_MAIL_FROM,
  CURLOPT_MAIL_RCPT,
  CURLOPT_CONNECT_ONLY
} CURLoption;

typedef enum {
  CURLINFO_RESPONSE_CODE
} CURLINFO;

typedef enum {
  CURLUSESSL_NONE,
  CURLUSESSL_TRY,
  CURLUSESSL_CONTROL,
  CURLUSESSL_ALL
} curl_usessl;

CURL* curl_easy_init(void);
void curl_easy_cleanup(CURL* curl);
CURLcode curl_easy_setopt(CURL* curl, CURLoption option, ...);
CURLcode curl_easy_perform(CURL* curl);
CURLcode curl_easy_getinfo(CURL* curl, CURLINFO info, ...);
char* curl_easy_escape(CURL* curl, const char* string, int length);
char* curl_easy_unescape(CURL* curl, const char* string, int length, int* outlength);
const char* curl_easy_strerror(CURLcode code);
void curl_free(void* p);
curl_slist* curl_slist_append(curl_slist* list, const char* data);
void curl_slist_free_all(curl_slist* list);
CURLcode curl_global_init(long flags);
void curl_global_cleanup(void);
