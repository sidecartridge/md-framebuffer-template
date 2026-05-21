/**
 * File: download.h
 * Author: Diego Parrilla Santamaría
 * Date: January 20205, February 2026
 * Copyright: 2025-2026 - GOODDATA LABS SL
 * Description: Header for download wrapper
 */

#ifndef DOWNLOAD_H
#define DOWNLOAD_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aconfig.h"
#include "constants.h"
#include "debug.h"
#include "ff.h"
#include "httpc/httpc.h"
#include "memfunc.h"
#include "network.h"

#define DOWNLOAD_BUFFLINE_SIZE 256
#define DOWNLOAD_FILENAME_SIZE 64
#define DOWNLOAD_HOSTNAME_SIZE 128
#define DOWNLOAD_PROTOCOL_SIZE 16
#define DOWNLOAD_POLLING_INTERVAL_MS 100

typedef enum {
  DOWNLOAD_STATUS_IDLE,
  DOWNLOAD_STATUS_REQUESTED,
  DOWNLOAD_STATUS_NOT_STARTED,
  DOWNLOAD_STATUS_STARTED,
  DOWNLOAD_STATUS_IN_PROGRESS,
  DOWNLOAD_STATUS_COMPLETED,
  DOWNLOAD_STATUS_FAILED
} download_status_t;

typedef enum {
  DOWNLOAD_POLL_CONTINUE,
  DOWNLOAD_POLL_ERROR,
  DOWNLOAD_POLL_COMPLETED
} download_poll_t;

typedef enum {
  DOWNLOAD_OK,
  DOWNLOAD_BASE64_ERROR,
  DOWNLOAD_PARSEJSON_ERROR,
  DOWNLOAD_PARSEMD5_ERROR,
  DOWNLOAD_CANNOTOPENFILE_ERROR,
  DOWNLOAD_CANNOTCLOSEFILE_ERROR,
  DOWNLOAD_FORCEDABORT_ERROR,
  DOWNLOAD_CANNOTSTARTDOWNLOAD_ERROR,
  DOWNLOAD_CANNOTREADFILE_ERROR,
  DOWNLOAD_CANNOTPARSEURL_ERROR,
  DOWNLOAD_MD5MISMATCH_ERROR,
  DOWNLOAD_CANNOTRENAMEFILE_ERROR,
  DOWNLOAD_CANNOTCREATE_CONFIG,
  DOWNLOAD_CANNOTDELETECONFIGSECTOR_ERROR
} download_err_t;

typedef struct {
  char protocol[DOWNLOAD_PROTOCOL_SIZE];
  char host[DOWNLOAD_HOSTNAME_SIZE];
  char uri[DOWNLOAD_BUFFLINE_SIZE];
} download_url_components_t;

typedef struct {
  char url[DOWNLOAD_BUFFLINE_SIZE];
  char filename[DOWNLOAD_FILENAME_SIZE];
} download_file_t;

/**
 * @brief Initiates the download by parsing the current URL, opening a temporary
 * file, and starting the HTTP client request for the file. Checks and prepares
 * the file system environment (e.g., clearing read-only attributes, handling
 * locked files) before initiating the asynchronous download.
 *
 * @return A download_err_t code indicating a successful start or a specific
 * error.
 */
download_err_t download_start(void);

/**
 * @brief Polls the download process by invoking the asynchronous context
 * routines. Processes incoming data packets and HTTP events. Periodically waits
 * for a defined interval to allow the download to progress until the process is
 * complete.
 *
 * @return DOWNLOAD_POLL_CONTINUE if download is in progress,
 * DOWNLOAD_POLL_COMPLETED when finished.
 */
download_poll_t download_poll(void);

/**
 * @brief Finalizes the download process by closing the temporary file and
 * releasing resources. Performs error handling during file closure and cleans
 * up HTTPS configurations if used.
 *
 * @return A download_err_t code indicating success or the specific error
 * encountered.
 */
download_err_t download_finish(void);

/**
 * @brief Renames the temporary download file to its final filename.
 *
 * Generates the final file path based on application configuration and deletes
 * any pre-existing file. Ensures integrity by checking the result of the rename
 * operation.
 *
 * @return A download_err_t code indicating a successful rename or an error code
 * if renaming fails.
 */
download_err_t download_confirm(void);

/**
 * @brief Retrieves the current status of the download process.
 *
 * @return The current download_status_t representing the state.
 */
download_status_t download_getStatus(void);

/**
 * @brief Updates the status of the download process.
 *
 * @param status New download_status_t value to set.
 */
void download_setStatus(download_status_t status);

/**
 * @brief Retrieves the current file path used in the download process.
 *
 * This path may represent the temporary file or a user-defined URL for
 * downloading.
 *
 * @return A pointer to a null-terminated string with the current file path.
 */
const char *download_getFilepath(void);

/**
 * @brief Sets the file path for the download process.
 *
 * Copies the supplied path into internal storage ensuring proper
 * null-termination.
 *
 * @param path A null-terminated string containing the new file path.
 */
void download_setFilepath(const char *path);

/**
 * @brief Provides access to the parsed components of the download URL.
 *
 * This includes protocol, hostname, and URI as extracted from the user-supplied
 * URL.
 *
 * @return A pointer to a download_url_components_t structure.
 */
const download_url_components_t *download_getUrlComponents(void);

#endif  // DOWNLOAD_H