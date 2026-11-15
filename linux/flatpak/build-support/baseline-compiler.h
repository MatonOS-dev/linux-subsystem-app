/* BoringSSL's optional vector_size paths include target("avx2") functions
 * even with OPENSSL_NO_ASM. Select its portable implementation in this static
 * baseline build. This header applies only to BoringSSL, never its consumers.
 */
#pragma GCC system_header
#undef __has_attribute
#define __has_attribute(attribute) 0
