#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <assert.h>

#define DT_CACHE_NULL_DELTA SHRT_MIN
#define DT_CACHE_EMPTY_HASH -1
#define DT_CACHE_EMPTY_KEY  -1
#define DT_CACHE_EMPTY_DATA  NULL
#define DT_CACHE_INSERT_RANGE (1024*4)


typedef struct dt_cache_bucket_t
{
  int16_t  first_delta;
  int16_t  next_delta;
  int16_t  read;   // number of readers
  int16_t  write;  // number of writers (0 or 1)
  uint32_t lru;    // for garbage collection: lru list
  uint32_t mru;
  uint32_t hash;
  uint32_t key;
  void*    data;
}
dt_cache_bucket_t;

typedef struct dt_cache_segment_t
{
  uint32_t timestamp;
  uint32_t lock;
}
dt_cache_segment_t;

typedef struct dt_cache_t
{
  uint32_t segment_shift;
  uint32_t segment_mask;
  uint32_t bucket_mask;
  dt_cache_segment_t *segments;
  dt_cache_bucket_t  *table;

  uint32_t lru, mru;
  int cache_mask;
  int is_cacheline_alignment;
}
dt_cache_t;	

// TODO: dt_cache_gc() and call it in put implicitly
// TODO: set up lru list!

static inline void
dt_cache_lock(uint32_t *lock)
{
  while(__sync_val_compare_and_swap(lock, 0, 1));
}

static inline void
dt_cache_unlock(uint32_t *lock)
{
  const uint32_t res = __sync_val_compare_and_swap(lock, 1, 0);
  assert(res);
}

static uint32_t
nearest_power_of_two(const uint32_t value)
{
  uint32_t rc = 1;
  while(rc < value) rc <<= 1;
  return rc;
}

static uint32_t
calc_div_shift(const uint32_t value)
{
  uint32_t shift = 0;
  uint32_t curr = 1;
  while (curr < value)
  {
    curr <<= 1;
    shift ++;
  }
  return shift;
}

static dt_cache_bucket_t*
get_start_cacheline_bucket(const dt_cache_t *const cache, dt_cache_bucket_t *const bucket)
{
  return bucket - ((bucket - cache->table) & cache->cache_mask);
}

static void
remove_key(dt_cache_segment_t *segment,
           dt_cache_bucket_t *const from_bucket,
           dt_cache_bucket_t *const key_bucket,
           dt_cache_bucket_t *const prev_key_bucket,
           const uint32_t hash)
{
  key_bucket->hash = DT_CACHE_EMPTY_HASH;
  key_bucket->key  = DT_CACHE_EMPTY_KEY;
  key_bucket->data = DT_CACHE_EMPTY_DATA;

  if(prev_key_bucket == NULL)
  {
    if(key_bucket->next_delta == DT_CACHE_NULL_DELTA)
      from_bucket->first_delta = DT_CACHE_NULL_DELTA;
    else
      from_bucket->first_delta = (from_bucket->first_delta + key_bucket->next_delta);
  }
  else
  {
    if(key_bucket->next_delta == DT_CACHE_NULL_DELTA)
      prev_key_bucket->next_delta = DT_CACHE_NULL_DELTA;
    else
      prev_key_bucket->next_delta = (prev_key_bucket->next_delta + key_bucket->next_delta);
  }
  segment->timestamp ++;
  key_bucket->next_delta = DT_CACHE_NULL_DELTA;
}

static void
add_key_to_begining_of_list(dt_cache_bucket_t *const keys_bucket,
                            dt_cache_bucket_t *const free_bucket,
                            const uint32_t     hash,
                            const uint32_t     key,
                            void              *data)
{
  free_bucket->data = data;
  free_bucket->key  = key;
  free_bucket->hash = hash;

  if(keys_bucket->first_delta == 0)
  {
    if(keys_bucket->next_delta == DT_CACHE_NULL_DELTA)
      free_bucket->next_delta = DT_CACHE_NULL_DELTA;
    else
      free_bucket->next_delta = (int16_t)((keys_bucket + keys_bucket->next_delta) - free_bucket);
    keys_bucket->next_delta = (int16_t)(free_bucket - keys_bucket);
  }
  else
  {
    if(keys_bucket->first_delta == DT_CACHE_NULL_DELTA)
      free_bucket->next_delta = DT_CACHE_NULL_DELTA;
    else
      free_bucket->next_delta = (int16_t)((keys_bucket + keys_bucket->first_delta) - free_bucket);
    keys_bucket->first_delta = (int16_t)(free_bucket - keys_bucket);
  }
}

static void
add_key_to_end_of_list(dt_cache_bucket_t *const keys_bucket,
                       dt_cache_bucket_t *const free_bucket,
                       const uint32_t     hash,
                       const uint32_t     key,
                       void              *data,
                       dt_cache_bucket_t *const last_bucket)
{
  free_bucket->data = data;
  free_bucket->key  = key;
  free_bucket->hash	= hash;
  free_bucket->next_delta = DT_CACHE_NULL_DELTA;

  if(last_bucket == NULL)
    keys_bucket->first_delta = (int16_t)(free_bucket - keys_bucket);
  else 
    last_bucket->next_delta = (int16_t)(free_bucket - last_bucket);
}

static void
optimize_cacheline_use(dt_cache_t         *cache,
                       dt_cache_segment_t *segment,
                       dt_cache_bucket_t  *const free_bucket)
{
  dt_cache_bucket_t *const start_cacheline_bucket = get_start_cacheline_bucket(cache, free_bucket);
  dt_cache_bucket_t *const end_cacheline_bucket = start_cacheline_bucket + cache->cache_mask;
  dt_cache_bucket_t *opt_bucket = start_cacheline_bucket;

  do
  {
    if(opt_bucket->first_delta != DT_CACHE_NULL_DELTA)
    {
      dt_cache_bucket_t *relocate_key_last = NULL;
      int curr_delta = opt_bucket->first_delta;
      dt_cache_bucket_t *relocate_key = opt_bucket + curr_delta;
      do
      {
        if( curr_delta < 0 || curr_delta > cache->cache_mask )
        {
          free_bucket->data = relocate_key->data;
          free_bucket->key  = relocate_key->key;
          free_bucket->hash = relocate_key->hash;

          if(relocate_key->next_delta == DT_CACHE_NULL_DELTA)
            free_bucket->next_delta = DT_CACHE_NULL_DELTA;
          else
            free_bucket->next_delta = (int16_t)( (relocate_key + relocate_key->next_delta) - free_bucket );

          if(relocate_key_last == NULL)
            opt_bucket->first_delta = (int16_t)( free_bucket - opt_bucket );
          else
            relocate_key_last->next_delta = (int16_t)( free_bucket - relocate_key_last );

          segment->timestamp ++;
          relocate_key->hash = DT_CACHE_EMPTY_HASH;
          relocate_key->key  = DT_CACHE_EMPTY_KEY;
          relocate_key->data = DT_CACHE_EMPTY_DATA;
          relocate_key->next_delta = DT_CACHE_NULL_DELTA;
          return;
        }

        if(relocate_key->next_delta == DT_CACHE_NULL_DELTA)
          break;
        relocate_key_last = relocate_key;
        curr_delta += relocate_key->next_delta;
        relocate_key += relocate_key->next_delta;
      }
      while(1);
    }
    ++opt_bucket;
  }
  while (opt_bucket <= end_cacheline_bucket);
}


void
dt_cache_init(dt_cache_t *cache, const int32_t capacity, const int32_t num_threads, int32_t cache_line_size, int32_t optimize_cacheline)
{
  const uint32_t adj_num_threads = nearest_power_of_two(num_threads);
  cache->cache_mask = cache_line_size / sizeof(dt_cache_bucket_t) - 1;
  cache->is_cacheline_alignment = optimize_cacheline;
  cache->segment_mask = adj_num_threads - 1;
  cache->segment_shift = calc_div_shift(nearest_power_of_two(num_threads/adj_num_threads)-1);
  const uint32_t adj_init_cap = nearest_power_of_two(capacity);
  const uint32_t num_buckets = adj_init_cap + DT_CACHE_INSERT_RANGE + 1;
  cache->bucket_mask = adj_init_cap - 1;
  cache->segment_shift = __builtin_clz(cache->bucket_mask) - __builtin_clz(cache->segment_mask);

  cache->segments = (dt_cache_segment_t *)malloc((cache->segment_mask + 1) * sizeof(dt_cache_segment_t));
  cache->table    = (dt_cache_bucket_t  *)malloc(num_buckets * sizeof(dt_cache_bucket_t));
  // cache->segments = (dt_cache_segment_t *)dt_alloc_align(64, (cache->segment_mask + 1) * sizeof(dt_cache_segment_t));
  // cache->table    = (dt_cache_bucket_t  *)dt_alloc_align(64, num_buckets * sizeof(dt_cache_bucket_t));

  for(int k=0;k<=cache->segment_mask;k++)
  {
    cache->segments[k].timestamp = 0;
    cache->segments[k].lock = 0;
  }
  for(int k=0;k<num_buckets;k++)
  {
    cache->table[k].first_delta = DT_CACHE_NULL_DELTA;
    cache->table[k].next_delta  = DT_CACHE_NULL_DELTA;
    cache->table[k].hash        = DT_CACHE_EMPTY_HASH;
    cache->table[k].key         = DT_CACHE_EMPTY_KEY;
    cache->table[k].data        = DT_CACHE_EMPTY_DATA;
    cache->table[k].read        = 0;
    cache->table[k].write       = 0;
    cache->table[k].lru         = 0;
    cache->table[k].mru         = 0;
  }
}

void
dt_cache_cleanup(dt_cache_t *cache)
{
  free(cache->table);
  free(cache->segments);
}


int32_t
dt_cache_contains(const dt_cache_t *const cache, const uint32_t key)
{
  // calculate hash from the key:
  const uint32_t hash = key;
  const dt_cache_segment_t *segment = cache->segments + ((hash >> cache->segment_shift) & cache->segment_mask);

  uint32_t start_timestamp;
  do
  {
    start_timestamp = segment->timestamp;
    const dt_cache_bucket_t *curr_bucket = cache->table + (hash & cache->bucket_mask);
    int16_t next_delta = curr_bucket->first_delta;
    while(next_delta != DT_CACHE_NULL_DELTA)
    {
      curr_bucket += next_delta;
      if(hash == curr_bucket->hash && key == curr_bucket->key) return 1;
      next_delta = curr_bucket->next_delta;
    }
  }
  while(start_timestamp != segment->timestamp);

  return 0;
}


uint32_t
dt_cache_size(const dt_cache_t *const cache)
{
  uint32_t cnt = 0;
  const uint32_t num = cache->bucket_mask + DT_CACHE_INSERT_RANGE;
  for(int k=0;k<num;k++)
  {
    if(cache->table[k].hash != DT_CACHE_EMPTY_HASH) cnt++;
  }
  return cnt;
}

#if 0 // not sure we need this diagnostic tool:
  float
dt_cache_percent_keys_in_cache_line(const dt_cache_t *const cache)
{
  uint32_t total_in_cache = 0;
  uint32_t total = 0;
  for(int k=0;k<=cache->bucket_mask;k++)
  {
    const dt_cache_bucket_t *curr_bucket = cache->table + k;
    if(curr_bucket->first_delta != DT_CACHE_NULL_DELTA)
    {
      const dt_cache_bucket_t *start_cache_line_bucket = get_start_cacheline_bucket(curr_bucket);
      const dt_cache_bucket_t *check_bucket = curr_bucket + curr_bucket->first_delta;
      int curr_dist = curr_bucket->first_delta;
      do
      {
        total++;
        if(check_bucket - start_cache_line_bucket >= 0 && check_bucket - start_cache_line_bucket <= cache->cache_mask) total_in_cache++;
        if(check_bucket->next_delta == DT_CACHE_NULL_DELTA) break;
        curr_dist += check_bucket->next_delta;
        check_bucket += check_bucket->next_delta;
      }
      while(1);
    }
  }
  return (float)total_in_cache/(float)total*100.0f;
}
#endif


// if found, the data void* is returned. if not, it is set to be
// the given *data and a new hash table entry is created, which can be
// found using the given key later on.
// 
// TODO: do you get it with which locks? need to drop them?
// TODO: r/w lock and drop later!
void*
dt_cache_put(dt_cache_t *cache, const uint32_t key, void *data)
{
  // just to support different keys:
  const uint32_t hash = key;
  dt_cache_segment_t *segment = cache->segments + ((hash >> cache->segment_shift) & cache->segment_mask);

  dt_cache_lock(&segment->lock);

  dt_cache_bucket_t *const start_bucket = cache->table + (hash & cache->bucket_mask);
  dt_cache_bucket_t *last_bucket = NULL;
  dt_cache_bucket_t *compare_bucket = start_bucket;
  int16_t next_delta = compare_bucket->first_delta;
  while(next_delta != DT_CACHE_NULL_DELTA)
  {
    compare_bucket += next_delta;
    if(hash == compare_bucket->hash && (key == compare_bucket->key))
    {
      void *rc = compare_bucket->data;
      dt_cache_unlock(&segment->lock);
      return rc;
    }
    last_bucket = compare_bucket;
    next_delta = compare_bucket->next_delta;
  }

  if(cache->is_cacheline_alignment)
  {
    dt_cache_bucket_t *free_bucket = start_bucket;
    dt_cache_bucket_t *start_cacheline_bucket = get_start_cacheline_bucket(cache, start_bucket);
    dt_cache_bucket_t *end_cacheline_bucket   = start_cacheline_bucket + cache->cache_mask;
    do
    {
      if(free_bucket->hash == DT_CACHE_EMPTY_HASH)
      {
        add_key_to_begining_of_list(start_bucket, free_bucket, hash, key, data);
        dt_cache_unlock(&segment->lock);
        return DT_CACHE_EMPTY_DATA;
      }
      ++free_bucket;
      if(free_bucket > end_cacheline_bucket)
        free_bucket = start_cacheline_bucket;
    }
    while(start_bucket != free_bucket);
  }

  // place key in arbitrary free forward bucket
  dt_cache_bucket_t *max_bucket = start_bucket + (SHRT_MAX-1);
  dt_cache_bucket_t *last_table_bucket = cache->table + cache->bucket_mask;
  if(max_bucket > last_table_bucket)
    max_bucket = last_table_bucket;
  dt_cache_bucket_t *free_max_bucket = start_bucket + (cache->cache_mask + 1);
  while (free_max_bucket <= max_bucket)
  {
    if(free_max_bucket->hash == DT_CACHE_EMPTY_HASH)
    {
      add_key_to_end_of_list(start_bucket, free_max_bucket, hash, key, data, last_bucket);
      dt_cache_unlock(&segment->lock);
      return DT_CACHE_EMPTY_DATA;
    }
    ++free_max_bucket;
  }

  // place key in arbitrary free backward bucket
  dt_cache_bucket_t *min_bucket = start_bucket - (SHRT_MAX-1);
  if(min_bucket < cache->table)
    min_bucket = cache->table;
  dt_cache_bucket_t *free_min_bucket = start_bucket - (cache->cache_mask + 1);
  while (free_min_bucket >= min_bucket)
  {
    if(free_min_bucket->hash == DT_CACHE_EMPTY_HASH)
    {
      add_key_to_end_of_list(start_bucket, free_min_bucket, hash, key, data, last_bucket);
      dt_cache_unlock(&segment->lock);
      return DT_CACHE_EMPTY_DATA;
    }
    --free_min_bucket;
  }
  fprintf(stderr, "[cache] failed to find a free spot for new data!\n");
  exit(1);
  return DT_CACHE_EMPTY_DATA;
}


void*
dt_cache_remove(dt_cache_t *cache, const uint32_t key)
{
  const uint32_t hash = key;
  dt_cache_segment_t *segment = cache->segments + ((hash >> cache->segment_shift) & cache->segment_mask);
  dt_cache_lock(&segment->lock);

  dt_cache_bucket_t *const start_bucket = cache->table + (hash & cache->bucket_mask);
  dt_cache_bucket_t *last_bucket = NULL;
  dt_cache_bucket_t *curr_bucket = start_bucket;
  int16_t next_delta = curr_bucket->first_delta;
  while(1);
  {
    if(next_delta == DT_CACHE_NULL_DELTA)
    {
      dt_cache_unlock(&segment->lock);
      return DT_CACHE_EMPTY_DATA;
    }
    curr_bucket += next_delta;

    if( hash == curr_bucket->hash && key == curr_bucket->key)
    {
      void *rc = curr_bucket->data;
      remove_key(segment, start_bucket, curr_bucket, last_bucket, hash);
      if(cache->is_cacheline_alignment)
        optimize_cacheline_use(cache, segment, curr_bucket);
      dt_cache_unlock(&segment->lock);
      return rc;
    }
    last_bucket = curr_bucket;
    next_delta = curr_bucket->next_delta;
  }
  return DT_CACHE_EMPTY_DATA;
}

