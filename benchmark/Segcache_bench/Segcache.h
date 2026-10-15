#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
void _seg_init(size_t capacity, size_t thread_num, const char *config);
bool _seg_set(char *key, size_t k_len, char *value, size_t v_len, uint32_t etime);
bool _seg_add(char *key, size_t k_len, char *value, size_t v_len, uint32_t etime);
//-1:TTL out, -2:Not found, 1:Found
int _seg_get(char *key, size_t k_len, char *value, size_t *value_len);
void _seg_update_time(size_t time);
size_t _seg_get_time();
void _seg_get_instance(int i);