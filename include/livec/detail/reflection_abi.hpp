#ifndef LIVEC_DETAIL_REFLECTION_ABI_HPP
#define LIVEC_DETAIL_REFLECTION_ABI_HPP

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct livec_function_info {
    /* Returned strings are borrowed and remain valid until the next reload. */
    const char *name;
    const char *signature;
    const char *source_file;
    unsigned line;
    uint64_t version;
    void (*entry_point)(void);
} livec_function_info;

typedef struct livec_variable_info {
    /* Returned strings are borrowed and remain valid until the next reload. */
    const char *name;
    const char *type;
    const char *source_file;
    unsigned line;
    size_t size;
    int is_const;
    /* Cast to the declared C type before reading or writing. */
    void *address;
} livec_variable_info;

typedef struct livec_type_info {
    const char *id;
    const char *name;
    const char *source_file;
    const char *kind;
    size_t size;
    size_t alignment;
} livec_type_info;

typedef struct livec_field_info {
    const char *name;
    const char *type;
    size_t offset_bits;
    size_t size_bits;
    int is_const;
    int is_bitfield;
} livec_field_info;

/* Return the number of currently defined project functions/variables. */
size_t livec_reflect_function_count(void);
size_t livec_reflect_variable_count(void);

/* Return 1 when the requested item exists and 0 for an invalid index/miss. */
int livec_reflect_function_at(size_t index, livec_function_info *out);
int livec_reflect_variable_at(size_t index, livec_variable_info *out);

/* source_file may be NULL to match by name alone. */
int livec_reflect_function_find(const char *name, const char *source_file,
                                livec_function_info *out);
int livec_reflect_function_find_signature(const char *name, const char *signature,
                                          const char *source_file,
                                          livec_function_info *out);
int livec_reflect_variable_find(const char *name, const char *source_file,
                                livec_variable_info *out);

size_t livec_reflect_type_count(void);
int livec_reflect_type_at(size_t index, livec_type_info *out);
int livec_reflect_type_find(const char *name, const char *source_file,
                            livec_type_info *out);
size_t livec_reflect_type_field_count(const char *type_id);
int livec_reflect_type_field_at(const char *type_id, size_t index,
                                livec_field_info *out);
size_t livec_reflect_type_method_count(const char *type_id);
int livec_reflect_type_method_at(const char *type_id, size_t index,
                                 livec_function_info *out);

#ifdef __cplusplus
}
#endif

#endif
