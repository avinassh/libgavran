#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <gavran/db.h>
#include <gavran/internal.h>
#include <gavran/test.h>

// tag::tests14[]
describe(hash) {
  before_each() {
    errors_clear();
    system("mkdir -p /tmp/db");
    system("rm -f /tmp/db/*");
  }

  it("can write and read multiple values") {
    db_t db;
    db_options_t options = {.minimum_size = 4 * 1024 * 1024};
    assert(db_create("/tmp/db/try", &options, &db));
    defer(db_close, db);

    uint64_t hash_id;
    {
      txn_t w;
      assert(txn_create(&db, TX_WRITE, &w));
      defer(txn_close, w);
      assert(hash_create(&w, &hash_id));
      for (size_t i = 0; i < 16; i++) {
        hash_val_t hv_write = {
            .hash_id = hash_id, .key = i, .val = i + 3};
        assert(hash_set(&w, &hv_write, 0));
        hash_id = hv_write.hash_id;
      }

      for (size_t i = 0; i < 16; i++) {
        hash_val_t hv_read = {.hash_id = hash_id, .key = i};
        assert(hash_get(&w, &hv_read) && hv_read.has_val);
        assert(i + 3 == hv_read.val);
      }
    }
  }

  it("can get and set value from hash") {
    db_t db;
    db_options_t options = {.minimum_size = 4 * 1024 * 1024};
    assert(db_create("/tmp/db/try", &options, &db));
    defer(db_close, db);

    uint64_t hash_id;
    {
      txn_t w;
      assert(txn_create(&db, TX_WRITE, &w));
      defer(txn_close, w);
      assert(hash_create(&w, &hash_id));
      hash_val_t hv_write = {.hash_id = hash_id, .key = 2, .val = 3};
      assert(hash_set(&w, &hv_write, 0));
      hash_val_t hv_read = {.hash_id = hash_id, .key = 2};
      assert(hash_get(&w, &hv_read));  // read a written value
      assert(hv_read.val == 3);
      hash_val_t hv_old = {.hash_id = hash_id};
      hv_write.val      = 4;
      assert(hash_set(&w, &hv_write, &hv_old));
      assert(hv_old.val == 3);  // get old on update
      assert(hash_del(&w, &hv_old));
      assert(hv_old.val == 4);  // get old on delete
      assert(hash_get(&w, &hv_read));
      assert(hv_read.has_val == false);
      assert(txn_commit(&w));
    }
  }

  it("can write & read a LOT of values") {
    db_t db;
    db_options_t options = {.minimum_size = 4 * 1024 * 1024};
    assert(db_create("/tmp/db/try", &options, &db));
    defer(db_close, db);

    {
      txn_t w;
      assert(txn_create(&db, TX_WRITE, &w));
      defer(txn_close, w);
      uint64_t hash_id;
      assert(hash_create(&w, &hash_id));
      bool hash_id_changed = false;
      hash_val_t hv_write  = {.hash_id = hash_id};
      for (size_t i = 0; i < 12 * 1024; i++) {
        hv_write.key = i * 1024;
        hv_write.val = i;
        assert(hash_set(&w, &hv_write, 0));
        if (hv_write.hash_id_changed) {
          hash_id         = hv_write.hash_id;
          hash_id_changed = true;
        }
        hash_val_t hv_read = {.hash_id = hash_id};
        assert(hash_get(&w, &hv_read) && hv_read.has_val &&
               hv_read.val == 0);
      }

      /* Uncomment to and run:
        > dot -Tsvg /tmp/db/hash.graphwiz > hash.svg

      FILE* f = fopen("/tmp/db/hash.graphwiz", "wt");
      assert(print_hash_table(f, &w, hash_id));
      fclose(f);
      */

      assert(hash_id_changed);
      hash_val_t hv_read = {.hash_id = hash_id};
      for (size_t i = 0; i < 12 * 1024; i++) {
        hv_read.key = i * 1024;
        assert(hash_get(&w, &hv_read) && hv_read.has_val);
        assert(hv_read.val == i);
      }
      assert(txn_commit(&w));
    }
  }

  it("can write and delete LOTS of values") {
    db_t db;
    db_options_t options = {.minimum_size = 4 * 1024 * 1024};
    assert(db_create("/tmp/db/try", &options, &db));
    defer(db_close, db);

    uint64_t hash_id;
    {
      txn_t w;
      assert(txn_create(&db, TX_WRITE, &w));
      defer(txn_close, w);
      assert(hash_create(&w, &hash_id));
      for (size_t i = 0; i < 12 * 1024; i++) {
        hash_val_t hv_write = {
            .hash_id = hash_id, .key = i * 513, .val = i + 3};
        assert(hash_set(&w, &hv_write, 0));
        hash_id = hv_write.hash_id;
      }
      for (size_t i = 0; i < 12 * 1024; i++) {
        hash_val_t hv_read = {.hash_id = hash_id, .key = i * 513};
        assert(hash_get(&w, &hv_read) && hv_read.has_val);
        assert(hv_read.val == i + 3);
      }
      for (size_t i = 0; i < 12 * 1024; i += 2) {
        hash_val_t hv_read = {.hash_id = hash_id, .key = i * 513};
        assert(hash_get(&w, &hv_read) && hv_read.has_val);
        assert(hv_read.val == i + 3);

        hash_val_t hv_del = {.hash_id = hash_id, .key = i * 513};
        assert(hash_del(&w, &hv_del) && hv_del.has_val);
        assert(hv_del.val == i + 3);
        hash_id = hv_del.hash_id;
      }

      for (size_t i = 1; i < 12 * 1024; i += 2) {
        hash_val_t hv_read = {.hash_id = hash_id, .key = i * 513};
        assert(hash_get(&w, &hv_read) && hv_read.has_val);
        assert(hv_read.val == i + 3);
      }

      // check iteration
      pages_map_t* map;
      assert(pagesmap_new(8, &map));
      defer(free, map);
      hash_val_t it = {.hash_id = hash_id};
      size_t count  = 0;
      while (true) {
        assert(hash_get_next(&w, &map, &it));
        if (it.has_val == false) break;
        count++;
        assert(it.key / 513 == it.val - 3);
      }
      assert(count == 6 * 1024);
    }
  }
}
// end::tests14[]
