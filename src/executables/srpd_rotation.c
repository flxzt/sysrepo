/**
 * @file srpd_rotation.c
 * @author Ondrej Kusnirik <Ondrej.Kusnirik@cesnet.cz>
 * @brief rotation utility for sysrepo-plugind
 *
 * @copyright
 * Copyright (c) 2018 - 2022 Deutsche Telekom AG.
 * Copyright (c) 2018 - 2022 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _GNU_SOURCE

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <libyang/libyang.h>
#include <sysrepo.h>

#include "bin_common.h"
#include "config.h"
#include "srpd_common.h"
#include "srpd_rotation.h"

int
srpd_rotation_init_cb(sr_session_ctx_t *session, void **private_data)
{
    int r = 0;
    srpd_rotation_opts_t *opts = NULL;

    opts = calloc(1, sizeof *opts);
    if (!opts) {
        SRPLG_LOG_ERR("srpd_rotation", "Memory allocation failed (%s:%d).", __FILE__, __LINE__);
        goto error;
    }

    /* create notification rotation change subscription */
    if ((r = sr_module_change_subscribe(session, "sysrepo-plugind", "/sysrepo-plugind:sysrepo-plugind/notif-datastore/rotation/enabled",
            srpd_rotation_change_cb, opts, 0, SR_SUBSCR_ENABLED | SR_SUBSCR_DONE_ONLY, &opts->subscr))) {
        SRPLG_LOG_ERR("srpd_rotation", "Failed to subscribe (%s)", r);
        goto error;
    }

    /* create notification rotation state data change subscription */
    if ((r = sr_oper_get_subscribe(session, "sysrepo-plugind", "/sysrepo-plugind:sysrepo-plugind/notif-datastore/rotation/rotated-files-count",
            srpd_get_rot_count_cb, opts, 0, &opts->subscr))) {
        SRPLG_LOG_ERR("srpd_rotation", "Failed to subscribe (%s)", r);
        goto error;
    }

    *private_data = opts;
    return 0;

error:
    if (opts) {
        sr_unsubscribe(opts->subscr);
        free(opts);
    }
    return r;
}

void
srpd_rotation_cleanup_cb(sr_session_ctx_t *session, void *private_data)
{
    int rc;
    srpd_rotation_opts_t *opts = (srpd_rotation_opts_t *)private_data;

    (void)session;

    sr_unsubscribe(opts->subscr);
    ATOMIC_STORE_RELAXED(opts->running, 0);
    if ((rc = pthread_join(opts->tid, NULL))) {
        SRPLG_LOG_ERR("srpd_rotation", "pthread_join failed (%s).", rc);
    }
    free(ATOMIC_PTR_LOAD_RELAXED(opts->output_folder));
    free(opts);
}

/**
 * @brief Get the path to notif folder.
 *
 * @return Path to notification folder.
 */
static char *
srpd_get_notif_path(void)
{
    char *path;

    if (SR_NOTIFICATION_PATH[0]) {
        path = strdup(SR_NOTIFICATION_PATH);
    } else {
        if (asprintf(&path, "%s/data/notif/", sr_get_repo_path()) == -1) {
            path = NULL;
        }
    }

    return path;
}

int
srpd_get_rot_count_cb(sr_session_ctx_t *session, uint32_t sub_id, const char *module_name, const char *path,
        const char *request_xpath, uint32_t request_id, struct lyd_node **parent, void *private_data)
{
    int rc = 0;
    const struct ly_ctx *ctx;
    char value[21];
    srpd_rotation_opts_t *opts = (srpd_rotation_opts_t *)private_data;

    (void)sub_id;
    (void)module_name;
    (void)path;
    (void)request_xpath;
    (void)request_id;

    sprintf(value, "%" PRIu64, ATOMIC_LOAD_RELAXED(opts->rotated_files_count));
    ctx = sr_session_acquire_context(session);
    if ((rc = lyd_new_path(*parent, ctx, "/sysrepo-plugind:sysrepo-plugind/notif-datastore/rotation/rotated-files-count",
            value, 0, NULL)) != LY_SUCCESS) {
        goto cleanup;
    }

cleanup:
    sr_session_release_context(session);
    return rc;
}

/**
 * @brief Checks whether the format of the file name
 * matches std format of notification file.
 *
 * @param[in] file_name File name to be checked.
 * @param[out] file_time1 First derived time from the file name.
 * @param[out] file_time2 Second derived time from the file name.
 * @return 0 on success.
 * @return 1 on failure.
 */
static int
srpd_format_check(const char *file_name, time_t *file_time1, time_t *file_time2)
{
    char *x = NULL, *endptr = NULL;
    time_t time1, time2;
    char buf[7];

    memset(buf, '\0', 7);

    if (file_name == NULL) {
        return EXIT_FAILURE;
    }
    x = strchr(file_name, '.');
    if (x == NULL) {
        return EXIT_FAILURE;
    }
    strncpy(buf, x + 1, 6);
    if (strcmp(buf, "notif.")) {
        return EXIT_FAILURE;
    }
    x = strchr(x + 1, '.');
    time1 = strtoul(x + 1, &endptr, 10);
    if (!strcmp(endptr, x + 1)) {
        return EXIT_FAILURE;
    }
    x = strchr(x + 1, '-');
    if (x == NULL) {
        return EXIT_FAILURE;
    }
    time2 = strtoul(x + 1, &endptr, 10);
    if (!strcmp(endptr, x + 1)) {
        return EXIT_FAILURE;
    }

    if (file_time1 != NULL) {
        *file_time1 = time1;
    }
    if (file_time2 != NULL) {
        *file_time2 = time2;
    }
    return 0;
}

static void *
srpd_rotation_loop(void *arg)
{
    int rc = 0;
    DIR *d = NULL;
    struct dirent *dir = NULL;
    time_t current_time, file_time2 = 0;
    srpd_rotation_opts_t *opts = (srpd_rotation_opts_t *)arg;
    char *arg1 = NULL, *arg2 = NULL, *remove_str = NULL, *notif_dir_name = NULL;

    notif_dir_name = srpd_get_notif_path();
    if (!notif_dir_name) {
        SRPLG_LOG_ERR("srpd_rotation", "Notif directory is NULL.");
        goto cleanup;
    }
    if (srpd_mkpath((char *)ATOMIC_PTR_LOAD_RELAXED(opts->output_folder), 0777, NULL) == -1) {
        SRPLG_LOG_ERR("srpd_rotation", "Archive directory could not be created");
        goto cleanup;
    }

    for ( ; ATOMIC_LOAD_RELAXED(opts->running); sleep(1)) {

        /* remember current time */
        time(&current_time);

        /* open directory */
        d = opendir(notif_dir_name);
        if (!d) {
            continue;
        }

        /* read whole directory */
        while ((dir = readdir(d)) && ATOMIC_LOAD_RELAXED(opts->running)) {

            /* skip current and parent directories */
            if (!strcmp(dir->d_name, ".") || !strcmp(dir->d_name, "..")) {
                continue;
            }

            /* check correct format of the file and retrieve file times */
            if (srpd_format_check(dir->d_name, NULL, &file_time2)) {
                continue;
            }

            /* check whether a file is older than configured time */
            if ((current_time >= (time_t)ATOMIC_LOAD_RELAXED(opts->rotation_time)) &&
                    (file_time2 < (current_time - (time_t)ATOMIC_LOAD_RELAXED(opts->rotation_time)))) {

                /* build zipping args */
                if (ATOMIC_LOAD_RELAXED(opts->compress)) {
                    if (asprintf(&arg1, "%s%s.zip", (char *)ATOMIC_PTR_LOAD_RELAXED(opts->output_folder), dir->d_name) == -1) {
                        goto cleanup;
                    }
                    if (asprintf(&arg2, "%s%s", notif_dir_name, dir->d_name) == -1) {
                        goto cleanup;
                    }

                    /* zip a file in output folder */
                    if ((rc = srpd_exec("srpd_rotation", SRPD_ZIP_BINARY, 3, SRPD_ZIP_BINARY, arg1, arg2))) {
                        SRPLG_LOG_ERR("srpd_rotation", "Zipping a file failed.");
                    } else {
                        ATOMIC_INC_RELAXED(opts->rotated_files_count);

                        if (asprintf(&remove_str, "%s%s", notif_dir_name, dir->d_name) == -1) {
                            goto cleanup;
                        }

                        /* remove a file from notif folder */
                        if (remove(remove_str)) {
                            SRPLG_LOG_ERR("srpd_rotation", "Removing a file %s failed.", remove_str);
                        }
                    }

                    /* build moving args */
                } else {
                    if (asprintf(&arg1, "%s%s", notif_dir_name, dir->d_name) == -1) {
                        goto cleanup;
                    }
                    if (asprintf(&arg2, "%s%s", (char *)ATOMIC_PTR_LOAD_RELAXED(opts->output_folder), dir->d_name) == -1) {
                        goto cleanup;
                    }

                    /* move a file to the output folder */
                    if ((rc = rename(arg1, arg2)) == -1) {
                        SRPLG_LOG_ERR("srpd_rotation", "Moving a file %s failed.", arg1);
                    } else {
                        ATOMIC_INC_RELAXED(opts->rotated_files_count);
                    }
                }

                /* reset commands for next folders */
                rc = 0;
                free(arg1);
                free(arg2);
                free(remove_str);
                arg1 = NULL;
                arg2 = NULL;
                remove_str = NULL;
            }
        }
        closedir(d);
        d = NULL;
    }

cleanup:
    free(arg1);
    free(arg2);
    free(remove_str);
    free(notif_dir_name);
    closedir(d);
    return NULL;
}

int
srpd_rotation_change_cb(sr_session_ctx_t *session, uint32_t sub_id, const char *module_name, const char *xpath,
        sr_event_t event, uint32_t request_id, void *private_data)
{
    int rc = 0, t_creat = 0;
    char *time_unit = NULL;
    time_t time_value;
    sr_change_iter_t *iter = NULL;
    sr_change_oper_t oper;
    const struct lyd_node *node;
    srpd_rotation_opts_t *opts = (srpd_rotation_opts_t *)private_data;
    char *temp = NULL, *dir_str = NULL;

    (void)sub_id;
    (void)module_name;
    (void)xpath;
    (void)event;
    (void)request_id;

    if ((rc = sr_get_changes_iter(session, "/sysrepo-plugind:sysrepo-plugind/notif-datastore/rotation/enabled//.",
            &iter)) != SR_ERR_OK) {
        goto cleanup;
    }

    while ((rc = sr_get_change_tree_next(session, iter, &oper, &node, NULL, NULL, NULL)) == SR_ERR_OK) {

        if (!strcmp(node->schema->name, "enabled")) {
            if (oper == SR_OP_CREATED) {
                t_creat = 1;
            } else if (oper == SR_OP_DELETED) {
                ATOMIC_STORE_RELAXED(opts->running, 0);
                if ((rc = pthread_join(opts->tid, NULL))) {
                    SRPLG_LOG_ERR("srpd_rotation", "pthread_join failed (%s).", sr_strerror(rc));
                }
                /* continue and free iter */
            }

        } else if (!strcmp(node->schema->name, "older-than")) {
            time_value = strtoul(lyd_get_value(node), &time_unit, 10);

            /* convert bigger units to days */
            switch (time_unit[0]) {
            case 'Y':           /* time in years */
                time_value *= 365;
                break;
            case 'M':           /* time in months */
                time_value *= 30;
                break;
            case 'W':           /* time in weeks */
                time_value *= 7;
                break;
            }

            /* convert days and smaller units to seconds */
            switch (time_unit[0]) {
            default:
            case 'D':           /* time in days */
                time_value *= 24;
            /* fallthrough */
            case 'h':           /* time in hours */
                time_value *= 60;
            /* fallthrough */
            case 'm':           /* time in minutes */
                time_value *= 60;
            /* fallthrough */
            case 's':           /* time in seconds */
                break;
            }

            ATOMIC_STORE_RELAXED(opts->rotation_time, time_value);

        } else if (!strcmp(node->schema->name, "output-dir")) {
            /* safe update of the output_folder (srpd_rotation_loop() reads output_folder!) */
            temp = ATOMIC_PTR_LOAD_RELAXED(opts->output_folder);
            dir_str = strdup(lyd_get_value(node));
            if (!dir_str) {
                SRPLG_LOG_ERR("srpd_rotation", "strdup() failed (%s:%d)", __FILE__, __LINE__);
                goto cleanup;
            }
            ATOMIC_PTR_STORE_RELAXED(opts->output_folder, dir_str);
            free(temp);
            temp = NULL;

        } else if (!strcmp(node->schema->name, "compress")) {
            if (!strcmp(lyd_get_value(node), "true")) {
                ATOMIC_STORE_RELAXED(opts->compress, 1);
            } else {
                ATOMIC_STORE_RELAXED(opts->compress, 0);
            }
        }
    }

    /* check whether a thread should be created */
    if (t_creat) {
        assert(ATOMIC_PTR_LOAD_RELAXED(opts->output_folder));
        ATOMIC_STORE_RELAXED(opts->running, 1);
        if ((rc = pthread_create(&(opts->tid), NULL, &srpd_rotation_loop, opts))) {
            SRPLG_LOG_ERR("srpd_rotation", "Sysrepo-plugind change config notif callback failed to create thread: %s.",
                    sr_strerror(rc));
        }
        t_creat = 0;
    }

cleanup:
    free(temp);
    sr_free_change_iter(iter);
    return rc;
}
