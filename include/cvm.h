#ifndef CVM_H
#define CVM_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CVMOptions {
    const char *entry_name;
    const char **imports;
    int import_count;
    const char **cclib_imports;
    int cclib_import_count;
    const char *program_path;
    int jit_mode;
    int verbose;
} CVMOptions;

int cvm_run_file(const char *cclib_path, const CVMOptions *options, int *exit_code);

#ifdef __cplusplus
}
#endif

#endif
