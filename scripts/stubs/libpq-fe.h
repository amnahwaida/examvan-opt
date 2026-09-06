// Minimal stub of libpq-fe.h — syntax-check only, NOT a real implementation.
// Dipakai scripts/check-docker-paths.sh untuk men-compile jalur
// #ifdef HAS_LIBPQ di mesin dev tanpa libpq-dev (paritas flag Docker build).
#pragma once
#include <stddef.h>

typedef struct PGconn PGconn;
typedef struct PGresult PGresult;

typedef enum { CONNECTION_OK, CONNECTION_BAD } ConnStatusType;
typedef enum {
  PGRES_EMPTY_QUERY, PGRES_COMMAND_OK, PGRES_TUPLES_OK,
  PGRES_COPY_OUT, PGRES_COPY_IN, PGRES_BAD_RESPONSE,
  PGRES_NONFATAL_ERROR, PGRES_FATAL_ERROR
} ExecStatusType;

void PQfinish(PGconn* conn);
void PQclear(PGresult* res);
ConnStatusType PQstatus(const PGconn* conn);
ExecStatusType PQresultStatus(const PGresult* res);
int PQntuples(const PGresult* res);
int PQnfields(const PGresult* res);
char* PQgetvalue(const PGresult* res, int row, int col);
int PQgetisnull(const PGresult* res, int row, int col);
int PQnparams(const PGresult* res);
PGresult* PQexec(PGconn* conn, const char* command);
PGresult* PQexecParams(PGconn* conn, const char* command, int nParams,
                       const int* paramTypes, const char* const* paramValues,
                       const int* paramLengths, const int* paramFormats,
                       int resultFormat);
PGconn* PQconnectdb(const char* conninfo);
char* PQerrorMessage(const PGconn* conn);
char* PQresultErrorMessage(const PGresult* res);
char* PQcmdTuples(PGresult* res);
char* PQcmdStatus(PGresult* res);
int PQconsumeInput(PGconn* conn);
int PQisBusy(PGconn* conn);
PGresult* PQgetResult(PGconn* conn);
int PQsendQuery(PGconn* conn, const char* query);
void PQreset(PGconn* conn);
typedef void (*PQnoticeProcessor)(void* arg, const char* message);
PQnoticeProcessor PQsetNoticeProcessor(PGconn* conn, PQnoticeProcessor proc, void* arg);
