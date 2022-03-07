#include <assert.h>
#include <string.h>

#include <gavran/db.h>
#include <gavran/internal.h>

#define KEY_TO_BUCKET(num, depth) (num & ((1UL << depth) - 1))

// tag::hash_page_decl[]
#define HASH_BUCKET_DATA_SIZE (63)
#define HASH_OVERFLOW_CHAIN_SIZE (16)

typedef struct hash_bucket {
  // <1>
  bool overflowed : 1;
  // <2>
  uint8_t bytes_used : 7;
  uint8_t data[HASH_BUCKET_DATA_SIZE];
} hash_bucket_t;
static_assert(sizeof(hash_bucket_t) == 64, "Bad size");

#define BUCKETS_IN_PAGE (PAGE_SIZE / sizeof(hash_bucket_t))
// end::hash_page_decl[]

// Taken from:
// https://gist.github.com/degski/6e2069d6035ae04d5d6f64981c995ec2#file-invertible_hash_functions-hpp-L43
implementation_detail uint64_t hash_permute_key(uint64_t x) {
  x = ((x >> 32) ^ x) * 0xD6E8FEB86659FD93;
  x = ((x >> 32) ^ x) * 0xD6E8FEB86659FD93;
  x = ((x >> 32) ^ x);
  return x;
}

static result_t hash_id_to_dir_root(
    txn_t* tx, uint64_t hash_id, page_t* hash_root) {
  page_t hash = {.page_num = hash_id};
  ensure(txn_get_page(tx, &hash));
  assert(hash.metadata->common.page_flags == page_flags_hash);
  hash_root->page_num = hash.metadata->hash.dir_page_num;
  ensure(txn_get_page(tx, hash_root));
  return success();
}

// tag::hash_create[]
result_t hash_create(txn_t* tx, uint64_t* hash_id) {
  page_t p = {.number_of_pages = 1};
  ensure(txn_allocate_page(tx, &p, 0));
  p.metadata->hash.page_flags   = page_flags_hash;
  p.metadata->hash.dir_page_num = p.page_num;
  *hash_id                      = p.page_num;
  return success();
}
// end::hash_create[]

// tag::hash_get_from_page[]
static bool hash_get_from_page(
    page_t* p, uint64_t hashed_key, hash_val_t* kvp) {
  hash_bucket_t* buckets = p->address;
  uint64_t location =
      (hashed_key >> p->metadata->hash.depth) % BUCKETS_IN_PAGE;
  for (size_t i = 0; i < HASH_OVERFLOW_CHAIN_SIZE; i++) {
    uint64_t idx = (location + i) % BUCKETS_IN_PAGE;
    uint8_t* end = buckets[idx].data + buckets[idx].bytes_used;
    uint8_t* cur = buckets[idx].data;
    while (cur < end) {
      uint64_t k, v;
      cur           = varint_decode(varint_decode(cur, &k), &v);
      uint8_t flags = *cur++;
      if (k == kvp->key) {
        kvp->val   = v;
        kvp->flags = flags;
        return true;
      }
    }
    if (buckets[idx].overflowed == false) break;
  }
  return false;
}
// end::hash_get_from_page[]

// tag::hash_page_get_next[]
implementation_detail bool hash_page_get_next(
    void* address, hash_val_t* it) {
  hash_bucket_t* buckets = address;
  uint64_t idx = it->iter_state.pos_in_page / sizeof(hash_bucket_t);
  if (idx >= BUCKETS_IN_PAGE) {
    it->has_val = false;
    return false;
  }
  uint16_t offset =
      it->iter_state.pos_in_page % sizeof(hash_bucket_t);
  while (offset >= buckets[idx].bytes_used) {
    idx++;
    offset = 0;
    if (idx >= BUCKETS_IN_PAGE) {
      it->has_val = false;
      return false;
    }
  }
  uint8_t* start = buckets[idx].data + offset;
  uint8_t* end =
      varint_decode(varint_decode(start, &it->key), &it->val);
  it->flags     = *end++;
  it->has_val   = true;
  uint32_t size = (uint32_t)(end - start);
  if (size + offset == buckets[idx].bytes_used) {
    it->iter_state.pos_in_page =
        (uint16_t)((idx + 1) * sizeof(hash_bucket_t));
  } else {
    it->iter_state.pos_in_page =
        (uint16_t)(idx * sizeof(hash_bucket_t) + offset + size);
  }
  return true;
}
// end::hash_page_get_next[]

// tag::hash_append_to_page[]
static bool hash_append_to_page(hash_bucket_t* buckets,
    page_metadata_t* metadata, uint64_t hashed_key, uint8_t* buffer,
    size_t size) {
  uint64_t location =
      (hashed_key >> metadata->hash.depth) % BUCKETS_IN_PAGE;
  for (size_t i = 0; i < HASH_OVERFLOW_CHAIN_SIZE; i++) {
    uint64_t idx = (location + i) % BUCKETS_IN_PAGE;
    if (buckets[idx].bytes_used + size > HASH_BUCKET_DATA_SIZE) {
      buckets[idx].overflowed = true;
      continue;
    }
    memcpy(buckets[idx].data + buckets[idx].bytes_used, buffer, size);
    buckets[idx].bytes_used += size;
    metadata->hash.number_of_entries++;
    metadata->hash.bytes_used += size;
    return true;
  }
  return false;
}
// end::hash_append_to_page[]

// tag::hash_try_update_in_page[]
static void hash_remove_in_bucket(hash_bucket_t* bucket,
    uint8_t* start, uint8_t* end, uint8_t* cur) {
  // move other data to cover current one
  uint8_t size = (uint8_t)(cur - start);
  memmove(start, cur, (size_t)(end - cur));
  bucket->bytes_used -= size;
  // zero the remaining bytes
  memset(bucket->data + bucket->bytes_used, 0,
      HASH_BUCKET_DATA_SIZE - bucket->bytes_used);
}

static bool hash_try_update_in_page(hash_bucket_t* bucket,
    page_metadata_t* metadata, uint8_t* start, uint8_t* cur,
    uint8_t* buffer, uint8_t size) {
  // same size, can just overwrite
  if ((cur - start) == size) {
    memcpy(start, buffer, size);
    return true;
  }
  hash_remove_in_bucket(
      bucket, start, bucket->data + bucket->bytes_used, cur);
  metadata->hash.bytes_used -= (uint16_t)(cur - start);
  if (bucket->bytes_used + size <= HASH_BUCKET_DATA_SIZE) {
    memcpy(bucket->data + bucket->bytes_used, buffer, size);
    bucket->bytes_used += size;
    metadata->hash.bytes_used += size;
    return true;
  }
  metadata->hash.number_of_entries--;
  return false;
}
// end::hash_try_update_in_page[]

// tag::hash_set_in_page[]
static bool hash_set_in_page(page_t* p, uint64_t hashed_key,
    hash_val_t* set, hash_val_t* old) {
  uint8_t buffer[20];
  uint8_t* buf_end =
      varint_encode(set->val, varint_encode(set->key, buffer));
  *buf_end++             = set->flags;
  uint8_t size           = (uint8_t)(buf_end - buffer);
  hash_bucket_t* buckets = p->address;
  set->has_val           = true;
  if (old) {
    old->has_val = false;
  }
  uint64_t location =
      (hashed_key >> p->metadata->hash.depth) % BUCKETS_IN_PAGE;
  for (size_t i = 0; i < HASH_OVERFLOW_CHAIN_SIZE; i++) {
    uint64_t idx = (location + i) % BUCKETS_IN_PAGE;
    uint8_t* end = buckets[idx].data + buckets[idx].bytes_used;
    uint8_t* cur = buckets[idx].data;
    while (cur < end) {
      uint64_t k, v;
      uint8_t* start   = cur;
      cur              = varint_decode(varint_decode(cur, &k), &v);
      uint8_t old_flag = *cur++;
      if (k != set->key) continue;
      if (old) {
        old->has_val = true;
        old->key     = k;
        old->val     = v;
        old->flags   = old_flag;
      }
      if (v == set->val) return true;
      if (hash_try_update_in_page(
              buckets + idx, p->metadata, start, cur, buffer, size))
        return true;
      i = HASH_OVERFLOW_CHAIN_SIZE;  // exit outer loop
      break;
    }
    if (buckets[idx].overflowed == false) break;
  }
  // we now call it _knowing_ the value isn't here
  return hash_append_to_page(
      buckets, p->metadata, hashed_key, buffer, size);
}
// end::hash_set_in_page[]

// tag::hash_compact_buckets[]
static void hash_compact_buckets(
    hash_bucket_t* buckets, uint64_t start_idx, uint8_t depth) {
  size_t max_overflow = 0;
  for (size_t i = 0; i < HASH_OVERFLOW_CHAIN_SIZE; i++) {
    uint64_t idx = (start_idx + i) % BUCKETS_IN_PAGE;
    if (!buckets[idx].overflowed) break;
    max_overflow++;
  }
  while (max_overflow--) {
    uint64_t idx = (start_idx + max_overflow) % BUCKETS_IN_PAGE;

    uint8_t* end = buckets[idx].data + buckets[idx].bytes_used;
    uint8_t* cur = buckets[idx].data;
    bool remove_overflow = buckets[idx].overflowed == false;
    while (cur < end) {
      uint64_t k, v;
      uint8_t* start = cur;
      cur            = varint_decode(varint_decode(cur, &k), &v);
      cur++;  // flags
      uint64_t k_idx =
          (hash_permute_key(k) >> depth) % BUCKETS_IN_PAGE;
      if (k_idx == idx) continue;
      uint8_t size = (uint8_t)(cur - start);
      if (buckets[k_idx].bytes_used + size > HASH_BUCKET_DATA_SIZE) {
        remove_overflow = false;  // can't move to the right location
        continue;
      }
      memcpy(buckets[k_idx].data + buckets[k_idx].bytes_used, start,
          size);
      buckets[k_idx].bytes_used += size;
      hash_remove_in_bucket(buckets + idx, start, end, cur);
      end -= size;  // we remove the current value and moved mem
      cur = start;  // over it, so we need to continue from prev start
    }
    // can remove prev overflow? only if current has no overflow or
    // not part of overflow chain that may go further
    if (remove_overflow) {
      uint64_t prev_idx            = idx ? idx - 1 : BUCKETS_IN_PAGE;
      buckets[prev_idx].overflowed = false;
    }
  }
}
// end::hash_compact_buckets[]

// tag::hash_remove_from_page[]
static bool hash_remove_from_page(
    page_t* p, uint64_t hashed_key, hash_val_t* del) {
  assert(p->metadata->hash.depth < 64);
  hash_bucket_t* buckets = p->address;
  del->has_val           = false;
  uint64_t location =
      (hashed_key >> p->metadata->hash.depth) % BUCKETS_IN_PAGE;
  for (size_t i = 0; i < HASH_OVERFLOW_CHAIN_SIZE; i++) {
    uint64_t idx = (location + i) % BUCKETS_IN_PAGE;
    uint8_t* end = buckets[idx].data + buckets[idx].bytes_used;
    uint8_t* cur = buckets[idx].data;
    while (cur < end) {
      uint64_t k, v;
      uint8_t* start = cur;
      cur            = varint_decode(varint_decode(cur, &k), &v);
      uint8_t flags  = *cur++;
      if (k == del->key) {
        del->has_val = true;
        del->val     = v;
        del->flags   = flags;
        hash_remove_in_bucket(buckets + idx, start, end, cur);
        p->metadata->hash.number_of_entries--;
        p->metadata->hash.bytes_used -= (uint16_t)(cur - start);

        if (buckets[idx].overflowed) {
          hash_compact_buckets(
              buckets, idx, p->metadata->hash_dir.depth);
        }
        return true;
      }
    }
    if (buckets[idx].overflowed == false) break;
  }
  return false;
}
// end::hash_remove_from_page[]

// tag::hash_get[]
result_t hash_get(txn_t* tx, hash_val_t* kvp) {
  page_t hash_root = {0};
  ensure(hash_id_to_dir_root(tx, kvp->hash_id, &hash_root));
  uint64_t hashed_key = hash_permute_key(kvp->key);
  if (hash_root.metadata->common.page_flags == page_flags_hash) {
    kvp->has_val = hash_get_from_page(&hash_root, hashed_key, kvp);
  } else {
    uint64_t index =
        KEY_TO_BUCKET(hashed_key, hash_root.metadata->hash_dir.depth);
    assert(index <= hash_root.metadata->hash_dir.number_of_buckets);
    uint64_t* buckets = hash_root.address;
    page_t hash_page  = {.page_num = buckets[index]};
    ensure(txn_get_page(tx, &hash_page));
    kvp->has_val = hash_get_from_page(&hash_page, hashed_key, kvp);
  }
  return success();
}
// end::hash_get[]

// tag::hash_split_page_entries[]
static result_t hash_split_page_entries(
    void* address, uint8_t depth, page_t* pages[2]) {
  hash_val_t it                       = {0};
  uint64_t mask                       = 1 << (depth - 1);
  pages[0]->metadata->hash.page_flags = page_flags_hash;
  pages[0]->metadata->hash.depth      = depth;
  memcpy(pages[1]->metadata, pages[0]->metadata,
      sizeof(page_metadata_t));
  while (hash_page_get_next(address, &it)) {
    uint64_t hashed_key = hash_permute_key(it.key);
    if (hashed_key & mask) {
      ensure(hash_set_in_page(pages[1], hashed_key, &it, 0));
    } else {
      ensure(hash_set_in_page(pages[0], hashed_key, &it, 0));
    }
  }
  return success();
}
// end::hash_split_page_entries[]

// tag::hash_create_directory[]
static result_t hash_create_directory(txn_t* tx, page_t* existing) {
  page_t dir = {.number_of_pages = 1};
  ensure(txn_allocate_page(tx, &dir, existing->page_num));
  dir.metadata->hash_dir.page_flags = page_flags_hash_directory;
  dir.metadata->hash_dir.depth      = 1;
  dir.metadata->hash_dir.number_of_buckets = 2;
  dir.metadata->hash_dir.number_of_entries =
      existing->metadata->hash.number_of_entries;

  page_t right     = {.number_of_pages = 1};
  page_t* pages[2] = {existing, &right};
  ensure(txn_allocate_page(tx, pages[1], existing->page_num));

  void* buffer;
  ensure(txn_alloc_temp(tx, PAGE_SIZE, &buffer));
  memcpy(buffer, existing->address, PAGE_SIZE);
  memset(existing->address, 0, PAGE_SIZE);
  memset(existing->metadata, 0, sizeof(page_metadata_t));
  ensure(hash_split_page_entries(buffer, 1, pages));
  uint64_t* dir_pages                   = dir.address;
  dir_pages[0]                          = existing->page_num;
  dir_pages[1]                          = right.page_num;
  existing->metadata->hash.dir_page_num = dir.page_num;
  return success();
}
// end::hash_create_directory[]

// tag::hash_expand_directory[]
static result_t hash_expand_directory(txn_t* tx, page_t* dir) {
  uint32_t cur_buckets = dir->metadata->hash_dir.number_of_buckets;
  page_t new           = {.number_of_pages =
                    TO_PAGES(cur_buckets * 2 * sizeof(uint64_t))};
  ensure(txn_allocate_page(tx, &new, dir->page_num));
  // copy the current directory *twice*
  memcpy(new.address, dir->address, cur_buckets * sizeof(uint64_t));
  memcpy(new.address + cur_buckets * sizeof(uint64_t), dir->address,
      cur_buckets * sizeof(uint64_t));

  memcpy(new.metadata, dir->metadata, sizeof(page_metadata_t));
  new.metadata->hash_dir.depth++;
  new.metadata->hash_dir.number_of_buckets *= 2;
  ensure(txn_free_page(tx, dir));
  memcpy(dir, &new, sizeof(page_t));
  return success();
}
// end::hash_expand_directory[]

// tag::hash_split_page[]
static result_t hash_split_page(
    txn_t* tx, page_t* page, page_t* dir, hash_val_t* set) {
  if (page->metadata->hash.depth == dir->metadata->hash.depth) {
    ensure(hash_expand_directory(tx, dir));
  }
  uint32_t bit = 1 << page->metadata->hash.depth;

  page_t new_page      = {.number_of_pages = 1};
  page_t* pages_ptr[2] = {page, &new_page};
  ensure(txn_allocate_page(tx, &new_page, page->page_num));
  uint8_t new_depth = page->metadata->hash.depth + 1;

  void* buffer;
  ensure(txn_alloc_temp(tx, PAGE_SIZE, &buffer));
  memcpy(buffer, page->address, PAGE_SIZE);
  memset(page->address, 0, PAGE_SIZE);
  memset(page->metadata, 0, sizeof(page_metadata_t));

  ensure(hash_split_page_entries(buffer, new_depth, pages_ptr));
  uint64_t* buckets = dir->address;
  for (size_t i = 0; i < dir->metadata->hash_dir.number_of_buckets;
       i++) {
    if (buckets[i] == page->page_num) buckets[i] = 0;
  }

  for (size_t i = hash_permute_key(set->key) & (bit - 1);
       i < dir->metadata->hash_dir.number_of_buckets; i += bit) {
    buckets[i] = pages_ptr[(i & bit) == bit]->page_num;
  }

  page_metadata_t* hash_metadata;
  ensure(txn_modify_metadata(tx, set->hash_id, &hash_metadata));
  hash_metadata->hash.dir_page_num = dir->page_num;
  return success();
}
// end::hash_split_page[]

// tag::hash_set_small[]
static result_t hash_set_small(txn_t* tx, page_t* p,
    uint64_t hashed_key, hash_val_t* set, hash_val_t* old) {
  ensure(txn_modify_page(tx, p));
  if (hash_set_in_page(p, hashed_key, set, old)) {
    return success();
  }
  ensure(hash_create_directory(tx, p));
  ensure(hash_set(tx, set, old));
  return success();
}
// end::hash_set_small[]

// tag::hash_set[]
result_t hash_set(txn_t* tx, hash_val_t* set, hash_val_t* old) {
  page_t hash_root = {0};
  ensure(hash_id_to_dir_root(tx, set->hash_id, &hash_root));
  ensure(txn_modify_page(tx, &hash_root));

  uint64_t hashed_key = hash_permute_key(set->key);
  if (hash_root.metadata->common.page_flags == page_flags_hash) {
    // <1>
    ensure(hash_set_small(tx, &hash_root, hashed_key, set, old));
    return success();
  }
  // <2>
  uint64_t index =
      KEY_TO_BUCKET(hashed_key, hash_root.metadata->hash_dir.depth);
  assert(index <= hash_root.metadata->hash_dir.number_of_buckets);
  uint64_t* buckets = hash_root.address;
  page_t hash_page  = {.page_num = buckets[index]};
  ensure(txn_modify_page(tx, &hash_page));
  uint32_t old_entries = hash_page.metadata->hash.number_of_entries;
  // <3>
  if (hash_set_in_page(&hash_page, hashed_key, set, old)) {
    // <4>
    if (old_entries != hash_page.metadata->hash.number_of_entries) {
      hash_root.metadata->hash_dir.number_of_entries++;
    }
    return success();
  }
  // <5>
  ensure(hash_split_page(tx, &hash_page, &hash_root, set));
  ensure(hash_set(tx, set, old));
  return success();
}
// end::hash_set[]

// tag::hash_maybe_shrink_directory[]
static result_t hash_maybe_shrink_directory(
    txn_t* tx, page_t* dir, hash_val_t* del) {
  uint64_t* buckets = dir->address;
  uint32_t depth    = dir->metadata->hash_dir.depth;
  for (size_t i = 0; i < dir->metadata->hash_dir.number_of_buckets;
       i++) {
    page_metadata_t* bucket_metadata;
    ensure(txn_get_metadata(tx, buckets[i], &bucket_metadata));
    // <1>
    if (bucket_metadata->hash.depth == depth) {
      return success();  // the depth is needed, cannot shrink
    }
  }
  uint32_t bucket_count =
      dir->metadata->hash_dir.number_of_buckets / 2;
  page_t new_dir = {
      .number_of_pages = TO_PAGES(bucket_count * sizeof(uint64_t))};
  ensure(txn_allocate_page(tx, &new_dir, dir->page_num));
  memcpy(new_dir.metadata, dir->metadata, sizeof(page_metadata_t));
  new_dir.metadata->hash_dir.number_of_buckets /= 2;
  new_dir.metadata->hash_dir.depth--;
  memcpy(
      new_dir.address, dir->address, sizeof(uint64_t) * bucket_count);
  ensure(txn_free_page(tx, dir));
  page_metadata_t* hash_metadata;
  ensure(txn_modify_metadata(tx, del->hash_id, &hash_metadata));
  hash_metadata->hash.dir_page_num = new_dir.page_num;
  return success();
}
// end::hash_maybe_shrink_directory[]

// tag::hash_convert_directory_to_hash[]
static bool hash_merge_pages_work(
    page_t* p1, page_t* p2, page_t* dst, uint64_t* hashed_key) {
  hash_val_t it = {0};
  while (hash_page_get_next(p1->address, &it)) {
    *hashed_key = hash_permute_key(it.key);
    if (!hash_set_in_page(dst, *hashed_key, &it, 0)) return false;
  }
  memset(&it, 0, sizeof(hash_val_t));
  while (hash_page_get_next(p2->address, &it)) {
    *hashed_key = hash_permute_key(it.key);
    if (!hash_set_in_page(dst, *hashed_key, &it, 0)) return false;
  }
  return true;
}
static result_t hash_convert_directory_to_hash(txn_t* tx,
    hash_val_t* kvp, page_t* page, uint64_t sibling_page_num,
    page_t* dir) {
  page_t sibling = {.page_num = sibling_page_num};
  ensure(txn_modify_page(tx, &sibling));
  uint64_t hashed_key = 0;
  void* buffer;
  ensure(txn_alloc_temp(tx, PAGE_SIZE, &buffer));
  memset(buffer, 0, PAGE_SIZE);
  page_metadata_t temp_metadata = {
      .hash = {.page_flags = page_flags_hash,
          .dir_page_num    = kvp->hash_id}};
  page_t dst = {.address = buffer, .metadata = &temp_metadata};
  ensure(hash_merge_pages_work(page, &sibling, &dst, &hashed_key));
  ensure(txn_free_page(tx, dir));
  if (page->page_num == kvp->hash_id) {
    ensure(txn_free_page(tx, &sibling));
    memcpy(page->address, buffer, PAGE_SIZE);
  } else {
    ensure(txn_free_page(tx, page));
    memcpy(sibling.address, buffer, PAGE_SIZE);
  }
  page_t hash_root = {.page_num = kvp->hash_id};
  ensure(txn_modify_page(tx, &hash_root));
  memcpy(hash_root.metadata, &temp_metadata, sizeof(page_metadata_t));
  return success();
}
// end::hash_convert_directory_to_hash[]

// tag::hash_merge_pages[]
static result_t hash_merge_pages(txn_t* tx, hash_val_t* kvp,
    page_t* page, page_t* sibling, page_t* dir) {
  uint8_t new_depth =
      MIN(page->metadata->hash.depth, sibling->metadata->hash.depth) -
      1;
  page_metadata_t merged_metadata = {
      .hash = {.page_flags = page_flags_hash,
          .depth           = new_depth,
          .dir_page_num    = dir->page_num}};
  void* buffer;
  ensure(txn_alloc_temp(tx, PAGE_SIZE, &buffer));
  memset(buffer, 0, PAGE_SIZE);
  page_t dst = {.address = buffer, .metadata = &merged_metadata};
  // <1>
  uint64_t hashed_key;
  ensure(hash_merge_pages_work(page, sibling, &dst, &hashed_key));
  // <2>
  uint64_t new_page_id;
  if (kvp->hash_id == page->page_num) {
    ensure(txn_free_page(tx, sibling));
    memcpy(page->address, buffer, PAGE_SIZE);
    memcpy(page->metadata, &merged_metadata, sizeof(page_metadata_t));
    new_page_id = page->page_num;
  } else {
    ensure(txn_free_page(tx, page));
    memcpy(sibling->address, buffer, PAGE_SIZE);
    memcpy(
        sibling->metadata, &merged_metadata, sizeof(page_metadata_t));
    new_page_id = sibling->page_num;
  }
  // <3>
  uint64_t* buckets = dir->address;
  size_t bit        = 1UL << merged_metadata.hash.depth;
  for (size_t i = hashed_key & (bit - 1);
       i < dir->metadata->hash_dir.number_of_buckets; i += bit) {
    buckets[i] = new_page_id;
  }
  return success();
}
// end::hash_merge_pages[]

// tag::hash_maybe_merge_pages[]
static result_t hash_maybe_merge_pages(txn_t* tx, uint64_t index,
    page_t* page, page_t* dir, hash_val_t* del) {
  uint64_t sibling_index =
      index ^ (1UL << (page->metadata->hash.depth - 1));
  uint64_t* buckets = dir->address;
  page_t sibling    = {.page_num = buckets[sibling_index]};
  ensure(txn_get_page(tx, &sibling));
  uint16_t joined_size = sibling.metadata->hash.bytes_used +
                         page->metadata->hash.bytes_used;
  // <1>
  if (joined_size > (PAGE_SIZE / 4) * 3) {  // no point in merging
    return success();
  }
  // <2>
  if (dir->metadata->hash_dir.number_of_buckets == 2) {
    ensure(hash_convert_directory_to_hash(
        tx, del, page, buckets[sibling_index], dir));
    return success();
  }
  // <3>
  ensure(hash_merge_pages(tx, del, page, &sibling, dir));

  // <4>
  ensure(hash_maybe_shrink_directory(tx, dir, del));
  return success();
}
// end::hash_maybe_merge_pages[]

// tag::hash_del[]
result_t hash_del(txn_t* tx, hash_val_t* del) {
  page_t hash_root = {0};
  ensure(hash_id_to_dir_root(tx, del->hash_id, &hash_root));
  uint64_t hashed_key = hash_permute_key(del->key);
  ensure(txn_modify_page(tx, &hash_root));
  if (hash_root.metadata->common.page_flags == page_flags_hash) {
    hash_remove_from_page(&hash_root, hashed_key, del);
    return success();
  }
  uint64_t index =
      KEY_TO_BUCKET(hashed_key, hash_root.metadata->hash_dir.depth);
  assert(index <= hash_root.metadata->hash_dir.number_of_buckets);
  uint64_t* buckets = hash_root.address;
  page_t hash_page  = {.page_num = buckets[index]};
  ensure(txn_modify_page(tx, &hash_page));
  if (!hash_remove_from_page(&hash_page, hashed_key, del)) {
    return success();  // entry does not exists
  }
  hash_root.metadata->hash_dir.number_of_entries--;
  ensure(
      hash_maybe_merge_pages(tx, index, &hash_page, &hash_root, del));
  return success();
}
// end::hash_del[]

// tag::hash_get_entries_count[]
result_t hash_get_entries_count(
    txn_t* tx, uint64_t hash_id, uint64_t* number_of_entries) {
  page_t hash_root = {0};
  ensure(hash_id_to_dir_root(tx, hash_id, &hash_root));
  if (hash_root.metadata->common.page_flags == page_flags_hash) {
    *number_of_entries = hash_root.metadata->hash.number_of_entries;
  } else {
    *number_of_entries =
        hash_root.metadata->hash_dir.number_of_entries;
  }
  return success();
}
// end::hash_get_entries_count[]

// tag::hash_get_next[]
result_t hash_get_next(
    txn_t* tx, pages_map_t** state, hash_val_t* it) {
  page_t hash_root = {0};
  ensure(hash_id_to_dir_root(tx, it->hash_id, &hash_root));
  if (hash_root.metadata->common.page_flags == page_flags_hash) {
    it->has_val = hash_page_get_next(hash_root.address, it);
    return success();
  }
  uint64_t* buckets = hash_root.address;
  do {
    page_t hash_page = {
        .page_num = buckets[it->iter_state.page_index]};
    ensure(txn_get_page(tx, &hash_page));
    if (hash_page_get_next(hash_page.address, it)) return success();
    ensure(pagesmap_put_new(state, &hash_page));
    it->iter_state.pos_in_page = 0;
    do {
      if (++it->iter_state.page_index >=
          hash_root.metadata->hash_dir.number_of_buckets) {
        it->has_val = false;
        return success();
      }
      hash_page.page_num = buckets[it->iter_state.page_index];
      if (pagesmap_lookup(*state, &hash_page) == false)
        break;  // didn't see this page, yet
    } while (true);
  } while (true);
  return success();
}
// end::hash_get_next[]

// tag::hash_drop_one[]
implementation_detail result_t hash_drop_one(
    txn_t* tx, uint64_t hash_id) {
  page_t hash_root = {0};
  ensure(hash_id_to_dir_root(tx, hash_id, &hash_root));
  if (hash_root.metadata->common.page_flags == page_flags_hash) {
    ensure(txn_free_page(tx, &hash_root));
    return success();
  }
  pages_map_t* pages;
  ensure(pagesmap_new(8, &pages));
  defer(free, pages);
  uint64_t* buckets = hash_root.address;
  for (size_t i = 0;
       i < hash_root.metadata->hash_dir.number_of_buckets; i++) {
    page_t hash_page = {.page_num = buckets[i]};
    if (pagesmap_lookup(pages, &hash_page)) {
      continue;
    }
    ensure(pagesmap_put_new(&pages, &hash_page));
    ensure(txn_free_page(tx, &hash_page));
  }
  ensure(txn_free_page(tx, &hash_root));
  return success();
}
// end::hash_drop_one[]

// tag::hash_drop_with_nesting[]
static result_t hash_drop_nested(txn_t* tx, uint64_t hash_id) {
  while (hash_id) {
    page_t hash = {.page_num = hash_id};
    ensure(txn_get_page(tx, &hash));
    assert(hash.metadata->common.page_flags == page_flags_hash);
    hash_id = hash.metadata->hash.nested.next;
    ensure(hash_drop_one(tx, hash.page_num));
  }
  return success();
}
result_t hash_drop(txn_t* tx, uint64_t hash_id) {
  ensure(hash_drop_nested(tx, hash_id));
  return success();
}
// end::hash_drop_with_nesting[]
