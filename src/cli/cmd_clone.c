/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#include <git2.h>
#include <stdio.h>

#include "common.h"
#include "cmd.h"
#include "error.h"
#include "sighandler.h"
#include "progress.h"

#include "fs_path.h"
#include "futils.h"

#define COMMAND_NAME "clone"

static char *branch, *remote_path, *local_path, *depth;
static int quiet, checkout = 1, bare;
static bool local_path_exists;
static cli_progress progress = CLI_PROGRESS_INIT;

static const cli_opt_spec opts[] = {
	CLI_COMMON_OPT,

	{ CLI_OPT_TYPE_SWITCH,    "quiet",       'q', &quiet,       1,
	  CLI_OPT_USAGE_DEFAULT,   NULL,         "display the type of the object" },
	{ CLI_OPT_TYPE_SWITCH,    "no-checkout", 'n', &checkout,    0,
	  CLI_OPT_USAGE_DEFAULT,   NULL,         "don't checkout HEAD" },
	{ CLI_OPT_TYPE_SWITCH,    "bare",         0,  &bare,        1,
	  CLI_OPT_USAGE_DEFAULT,   NULL,         "don't create a working directory" },
	{ CLI_OPT_TYPE_VALUE,     "branch",      'b', &branch,      0,
	  CLI_OPT_USAGE_DEFAULT,  "name",        "branch to check out" },
	{ CLI_OPT_TYPE_VALUE,     "depth",       0,   &depth,       0,
	  CLI_OPT_USAGE_DEFAULT,  "depth",       "commit depth to check out " },
	{ CLI_OPT_TYPE_LITERAL },
	{ CLI_OPT_TYPE_ARG,       "repository",   0,  &remote_path, 0,
	  CLI_OPT_USAGE_REQUIRED, "repository",  "repository path" },
	{ CLI_OPT_TYPE_ARG,       "directory",    0,  &local_path,  0,
	  CLI_OPT_USAGE_DEFAULT,  "directory",    "directory to clone into" },
	{ 0 }
};

static void print_help(void)
{
	cli_opt_usage_fprint(stdout, PROGRAM_NAME, COMMAND_NAME, opts, 0);
	printf("\n");

	printf("Clone a repository into a new directory.\n");
	printf("\n");

	printf("Options:\n");

	cli_opt_help_fprint(stdout, opts);
}

static char *compute_local_path(const char *orig_path)
{
	const char *slash;
	char *local_path;

	if ((slash = strrchr(orig_path, '/')) == NULL &&
	    (slash = strrchr(orig_path, '\\')) == NULL)
		local_path = git__strdup(orig_path);
	else
		local_path = git__strdup(slash + 1);

	return local_path;
}

static int compute_depth(const char *depth)
{
	int64_t i;
	const char *endptr;

	if (!depth)
		return 0;

	if (git__strntol64(&i, depth, strlen(depth), &endptr, 10) < 0 || i < 0 || i > INT_MAX || *endptr) {
		fprintf(stderr, "fatal: depth '%s' is not valid.\n", depth);
		exit(128);
	}

	return (int)i;
}

static bool validate_local_path(const char *path)
{
	if (!git_fs_path_exists(path))
		return false;

	if (!git_fs_path_isdir(path) || !git_fs_path_is_empty_dir(path)) {
		fprintf(stderr, "fatal: destination path '%s' already exists and is not an empty directory.\n",
			path);
		exit(128);
	}

	return true;
}

static void cleanup(void)
{
	int rmdir_flags = GIT_RMDIR_REMOVE_FILES;

	cli_progress_abort(&progress);

	if (local_path_exists)
		rmdir_flags |= GIT_RMDIR_SKIP_ROOT;

	if (!git_fs_path_isdir(local_path))
		return;

	git_futils_rmdir_r(local_path, NULL, rmdir_flags);
}

static void interrupt_cleanup(void)
{
	cleanup();
	exit(130);
}

int cmd_clone(int argc, char **argv)
{
	git_clone_options clone_opts = GIT_CLONE_OPTIONS_INIT;
	git_repository *repo = NULL;
	cli_opt invalid_opt;
	char *computed_path = NULL;
	int ret = 0;

	if (cli_opt_parse(&invalid_opt, opts, argv + 1, argc - 1, CLI_OPT_PARSE_GNU))
		return cli_opt_usage_error(COMMAND_NAME, opts, &invalid_opt);

	if (cli_opt__show_help) {
		print_help();
		return 0;
	}

	if (!remote_path) {
		ret = cli_error_usage("you must specify a repository to clone");
		goto done;
	}

	clone_opts.bare = !!bare;
	clone_opts.checkout_branch = branch;
	clone_opts.fetch_opts.depth = compute_depth(depth);
	clone_opts.fetch_opts.proxy_opts.type = GIT_PROXY_AUTO;

	if (!checkout)
		clone_opts.checkout_opts.checkout_strategy = GIT_CHECKOUT_NONE;

	if (!local_path)
		local_path = computed_path = compute_local_path(remote_path);

	local_path_exists = validate_local_path(local_path);

	cli_sighandler_set_interrupt(interrupt_cleanup);

	if (!local_path_exists &&
	    git_futils_mkdir(local_path, 0777, 0) < 0) {
		ret = cli_error_git();
		goto done;
	}

	if (!quiet) {
		clone_opts.fetch_opts.callbacks.sideband_progress = cli_progress_fetch_sideband;
		clone_opts.fetch_opts.callbacks.transfer_progress = cli_progress_fetch_transfer;
		clone_opts.fetch_opts.callbacks.payload = &progress;

		clone_opts.checkout_opts.progress_cb = cli_progress_checkout;
		clone_opts.checkout_opts.progress_payload = &progress;

		printf("Cloning into '%s'...\n", local_path);
	}

	if (git_clone(&repo, remote_path, local_path, &clone_opts) < 0) {
		cleanup();
		ret = cli_error_git();
		goto done;
	}

	cli_progress_finish(&progress);

	// Code below for testing resume in native libgit2
	git_repository *repo2 = NULL;
	int error = git_repository_open_ext(&repo2, local_path, 0, NULL);
	// HEAD state info
	bool is_detached = git_repository_head_detached(repo2) == 1;
	bool is_unborn = git_repository_head_unborn(repo2) == 1;

	// Collect status (staged/unstaged/untracked)
	git_status_options opts = GIT_STATUS_OPTIONS_INIT;

	opts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
	opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED // include untracked files
	                                              // // |
	                                              // GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX
	                                              // // detect renames
	                                              // HEAD->index - not
	                                              // required currently and
	                                              // impacts performance
	             | GIT_STATUS_OPT_SORT_CASE_SENSITIVELY;

	git_status_list *status_list = NULL;
	ret = git_status_list_new(&status_list, repo2, &opts);

	size_t staged = 0, unstaged = 0, untracked = 0, conflicted = 0;
	const size_t n = git_status_list_entrycount(status_list);

	for (size_t i = 0; i < n; ++i) {
		const git_status_entry *e = git_status_byindex(status_list, i);
		if (!e)
			continue;
		unsigned s = e->status;

		// Staged (index) changes
		if (s & (GIT_STATUS_INDEX_NEW | GIT_STATUS_INDEX_MODIFIED |
		         GIT_STATUS_INDEX_DELETED | GIT_STATUS_INDEX_RENAMED |
		         GIT_STATUS_INDEX_TYPECHANGE))
			++staged;

		// Unstaged (workdir) changes
		if (s & (GIT_STATUS_WT_MODIFIED | GIT_STATUS_WT_DELETED |
		         GIT_STATUS_WT_RENAMED | GIT_STATUS_WT_TYPECHANGE))
			++unstaged;

		// Untracked
		if (s & GIT_STATUS_WT_NEW)
			++untracked;

		// Conflicted
		if (s & GIT_STATUS_CONFLICTED)
			++conflicted;
	}

	// Print summary (mirrors your original stream output)
	printf("HEAD state      : %s\n",
	       is_unborn ? "unborn (no commits)" :
                           (is_detached ? "detached" : "attached"));
	printf("Staged changes  : %zu\n", staged);
	printf("Unstaged changes: %zu\n", unstaged);
	printf("Untracked files : %zu", untracked);
	if (conflicted) {
		printf(" (%zu paths flagged)", conflicted);
	}
	printf("\n");
done:
	cli_progress_dispose(&progress);
	git__free(computed_path);
	git_repository_free(repo);
	return ret;
}
