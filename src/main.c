#include "cvm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void) {
    fprintf(stderr,
            "Usage: cvm <input.cclib> [--entry name] [--import <shared-lib>] [--import-cclib <path>] [--jit|--lazy-jit] [--jit-profile] [--jit-hot-threshold N] [--jit-loop-threshold N] [--version] [-v]\n");
}

static int parse_u64(const char *text, unsigned long long *out) {
    char *end = NULL;
    unsigned long long v;
    if (!text || !*text || !out) {
        return -1;
    }
    errno = 0;
    v = strtoull(text, &end, 10);
    if (errno != 0 || !end || *end != '\0') {
        return -1;
    }
    *out = v;
    return 0;
}

static void print_version(void) {
    printf("cvm: CHance Virtual Machine version 0.1.0\n");
    printf("cvm: License: OpenAzure License\n");
    printf("cvm: Compiled on %s %s\n", __DATE__, __TIME__);
    printf("cvm: Created by Nathan Hornby (AzureianGH)\n");
}

int main(int argc, char **argv) {
    const char *input = NULL;
    const char *entry = "main";
    const char *imports[64];
    const char *cclib_imports[64];
    int import_count = 0;
    int cclib_import_count = 0;
    int jit_mode = 0;
    int jit_profile = 0;
    unsigned long long jit_hot_threshold = 128;
    unsigned long long jit_loop_hot_threshold = 10000;
    int verbose = 0;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "--version") == 0) {
            print_version();
            return 0;
        }
        if (strcmp(arg, "--entry") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            entry = argv[++i];
        } else if (strcmp(arg, "--import") == 0) {
            if (i + 1 >= argc || import_count >= (int)(sizeof(imports) / sizeof(imports[0]))) {
                usage();
                return 2;
            }
            imports[import_count++] = argv[++i];
        } else if (strcmp(arg, "--import-cclib") == 0) {
            if (i + 1 >= argc || cclib_import_count >= (int)(sizeof(cclib_imports) / sizeof(cclib_imports[0]))) {
                usage();
                return 2;
            }
            cclib_imports[cclib_import_count++] = argv[++i];
        } else if (strcmp(arg, "--jit") == 0) {
            jit_mode = 1;
        } else if (strcmp(arg, "--lazy-jit") == 0) {
            jit_mode = 2;
        } else if (strcmp(arg, "--jit-profile") == 0) {
            jit_profile = 1;
        } else if (strcmp(arg, "--jit-hot-threshold") == 0) {
            if (i + 1 >= argc || parse_u64(argv[i + 1], &jit_hot_threshold) != 0) {
                usage();
                return 2;
            }
            ++i;
        } else if (strcmp(arg, "--jit-loop-threshold") == 0) {
            if (i + 1 >= argc || parse_u64(argv[i + 1], &jit_loop_hot_threshold) != 0) {
                usage();
                return 2;
            }
            ++i;
        } else if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            verbose = 1;
        } else if (arg[0] == '-') {
            usage();
            return 2;
        } else if (!input) {
            input = arg;
        } else {
            usage();
            return 2;
        }
    }

    if (!input) {
        usage();
        return 2;
    }

    CVMOptions options;
    options.entry_name = entry;
    options.imports = imports;
    options.import_count = import_count;
    options.cclib_imports = cclib_imports;
    options.cclib_import_count = cclib_import_count;
    options.program_path = argv[0];
    options.jit_mode = jit_mode;
    options.jit_profile = jit_profile;
    options.jit_hot_threshold = jit_hot_threshold;
    options.jit_loop_hot_threshold = jit_loop_hot_threshold;
    options.verbose = verbose;

    int code = 1;
    int rc = cvm_run_file(input, &options, &code);
    if (rc != 0) {
        return rc;
    }
    return code;
}
