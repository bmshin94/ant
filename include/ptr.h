#ifndef ANT_NATIVE_PTR_H
#define ANT_NATIVE_PTR_H

#include "types.h"
#include "internal.h" // IWYU pragma: keep

static inline void *js_get_native(ant_value_t obj, uint32_t tag) {
  ant_object_t *o = js_obj_ptr(obj);
  
  if (!o || !tag) return NULL;
  if (o->native_tag == tag) return o->native_ptr;

  ant_object_sidecar_t *sidecar = ant_object_sidecar(o);
  if (!sidecar) return NULL;
  
  for (uint8_t i = 0; i < sidecar->native_count; i++)
    if (sidecar->native_entries[i].tag == tag) return sidecar->native_entries[i].ptr;

  return NULL;
}

static inline void js_clear_native(ant_value_t obj, uint32_t tag) {
  ant_object_t *o = js_obj_ptr(obj);
  if (!o || !tag) return;

  ant_object_sidecar_t *sidecar = ant_object_sidecar(o);
  if (o->native_tag == tag) {
    if (sidecar && sidecar->native_count > 0) {
      sidecar->native_count--;
      o->native_ptr = sidecar->native_entries[sidecar->native_count].ptr;
      o->native_tag = sidecar->native_entries[sidecar->native_count].tag;
    } else {
      o->native_ptr = NULL;
      o->native_tag = 0;
    }
    return;
  }

  if (!sidecar) return;
  for (uint8_t i = 0; i < sidecar->native_count; i++) {
    if (sidecar->native_entries[i].tag != tag) continue;
    sidecar->native_count--;
    sidecar->native_entries[i] = sidecar->native_entries[sidecar->native_count];
    return;
  }
}

static inline void js_set_native(ant_value_t obj, void *ptr, uint32_t tag) {
  ant_object_t *o = js_obj_ptr(obj);
  if (!o || !tag) return;
  
  if (o->native_tag == 0 || o->native_tag == tag) {
    o->native_ptr = ptr;
    o->native_tag = tag;
    return;
  }

  ant_object_sidecar_t *sidecar = ant_object_ensure_sidecar(o);
  if (!sidecar) return;
  
  for (uint8_t i = 0; i < sidecar->native_count; i++) {
    if (sidecar->native_entries[i].tag != tag) continue;
    sidecar->native_entries[i].ptr = ptr;
    return;
  }

  if (sidecar->native_count >= sidecar->native_cap) {
    uint8_t next_cap = sidecar->native_cap ? (uint8_t)(sidecar->native_cap * 2) : 2;
    ant_native_entry_t *next = realloc(sidecar->native_entries, next_cap * sizeof(*next));
    
    if (!next) return;
    sidecar->native_entries = next;
    sidecar->native_cap = next_cap;
  }

  sidecar->native_entries[sidecar->native_count++] = (ant_native_entry_t){ ptr, tag };
}

static inline bool js_check_native_tag(ant_value_t obj, uint32_t tag) {
  ant_object_t *o = js_obj_ptr(obj);
  
  if (!o || !tag) return false;
  if (o->native_tag == tag) return true;

  ant_object_sidecar_t *sidecar = ant_object_sidecar(o);
  if (!sidecar) return false;
  
  for (uint8_t i = 0; i < sidecar->native_count; i++)
    if (sidecar->native_entries[i].tag == tag) return true;

  return false;
}

#endif
