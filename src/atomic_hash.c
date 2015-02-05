/* 
 * atomic_hash.c
 *
 * 2012-2015 Copyright (c) 
 * Fred Huang, <divfor@gmail.com>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Cooked from the CURL-project examples with thanks to the 
 * great CURL-project authors and contributors.
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <poll.h>
#include <math.h>
#include <assert.h>
#include <sys/time.h>
#include "atomic_hash.h"

#if defined (MPQ3HASH) || defined (NEWHASH)
#define NKEY 3
#elif defined (CITY3HASH_128) || defined (MURMUR3HASH_128) || defined (MD5HASH)
#define NKEY 4
#endif

#define NMHT 2
#define NCLUSTER 4
#define NSEAT (NMHT*NKEY*NCLUSTER)
#define NNULL 0xFFFFFFFF
#define MAXTAB NNULL
#define MINTAB 64
#define COLLISION 10000
#define MAXBLOCKS 1024

#define memword __attribute__((aligned(sizeof(void *))))
#define atomic_add1(v) __sync_fetch_and_add(&(v), 1)
#define atomic_sub1(v) __sync_fetch_and_sub(&(v), 1)
#define add1(v) __sync_fetch_and_add(&(v), 1)
//#define add1(v) (v++)
#define cas(dst, old, new) __sync_bool_compare_and_swap((dst), (old), (new))
#define ip(mp, type, i) (((type *)(mp->ba[(i) >> mp->shift]))[(i) & mp->mask])
#define i2p(mp, type, i) (i == NNULL ? NULL : &(ip(mp, type, i)))
//#define i2p(mp, type, i) (&(ip(mp, type, i)))
#define unhold_bucket(hv, v) do { if ((hv).y && !(hv).x) (hv).x = (v).x; } while(0)
//#define unhold_bucket(hv, v) while ((hv).y && !cas (&(hv).x, 0, (v).x))
#define hold_bucket_otherwise_return_0(hv, v) do { unsigned long __l = (1<<24); \
          while (!cas(&(hv).x, (v).x, 0)) { \
            while (!(hv).x) { if (!(hv).y) return 0; poll(0, 0, 1); } \
            if (--__l == 0) { add1 (h->stats.escapes); return 0; }} \
          if ((hv).y != (v).y || !(hv).y) { unhold_bucket (hv, v); return 0; } \
          } while (0)

//            if ((__l & 0xff) == 0) { poll (0, 0, 0); } 
//            while (!(hv).x) if ((hv).y) __asm__("pause"); else return 0;} 
#if 0
typedef struct { unsigned long c[8]; } hc_t;
unsigned long final[8] = {0};
unsigned long *counterp[MAX_THREADS] = { NULL };
DEFINE_SPINLOCK(final_mutex);

/* Per thread:
__thread hc_t *mypc = NULL;
 if (posix_memalign((void **)&mypc, 64, 64)))
  return 1;
*/

void inc_count (unsigned long *p)
{
   *p++;
}
void read_count(unsigned long sum[])
{
  int i, t;
  unsigned long *p;
  spin_lock (&final_mutex);
  for (i = 0; i < 8; i++)
    sum[i] = final[i];
  for (t = 0; t < MAX_THREADS; t++)
    if ((p = counterp[t]) != NULL)
      for (i = 0; i < 8; i++)
         sum[i] += p[i];
  spin_unlock(&final_mutex);
}
void counter_register_thread(void *p)
{
  int idx = gettid () % MAX_THREADS;
  assert (counterp[idx] == NULL);
  spin_lock(&final_mutex);
  counterp[idx] = p;
  spin_unlock(&final_mutex);
}
void counter_unregister_thread(void *p)
{
  spin_lock(&final_mutex);
  pick_to_finalcounters (p, finalcount);
  counterp[gettid() % MAX_THREADS] = NULL;
  spin_unlock(&final_mutex);
}
#endif



inline unsigned long
nowms ()
{
  struct timeval tv;
  gettimeofday (&tv, NULL);
  return (tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

mem_pool_t *
create_mem_pool (size_t max_nodes, size_t node_size, size_t max_blocks)
{
  unsigned long n, m;
  mem_pool_t *pmp;

  if (max_nodes < 1)
    return NULL;
  if (posix_memalign((void **)(&pmp), 64, sizeof(*pmp)))
    return NULL;
  memset (pmp, 0, sizeof(*pmp));

  pmp->curr_blocks = 0;
  pmp->node_size = node_size;
  m = max_blocks < 1 ? 1024 : max_blocks;
  pmp->max_blocks = m + 1; /* mutiple m to make overfill possible */
  for (pmp->shift = 0, n = 1; max_nodes > m * n; n *= 2)
    pmp->shift++;
  pmp->blk_node_num = n;
  pmp->mask = n - 1;
  if (!posix_memalign((void **)(&pmp->ba), 64, pmp->max_blocks * sizeof(*pmp->ba)))
    {
      memset (pmp->ba, 0, pmp->max_blocks * sizeof(*pmp->ba));
      return pmp;
    }
  free (pmp);
  return NULL;
}

int
destroy_mem_pool (mem_pool_t *pmp)
{
  unsigned int i;
  if (!pmp)
    return -1;
  for (i = 0; i < pmp->max_blocks; i++)
    if (pmp->ba[i])
      {
	free (pmp->ba[i]);
	pmp->ba[i] = NULL;
	pmp->curr_blocks--;
      }
  free (pmp->ba);
  pmp->ba = NULL;
  free (pmp);
  return 0;
}

inline nid *
new_mem_block (mem_pool_t *pmp, volatile cas_t * recv_queue)
{
  nid i, m, sft, sz, head = 0;
  memword cas_t n, x, *pn;
  void *p;

  if (!pmp || !(p = calloc (pmp->blk_node_num, pmp->node_size)))
    return NULL;
  for (i = pmp->curr_blocks; i < pmp->max_blocks; i++)
    if (cas (&pmp->ba[i], NULL, p)) break;
  if (i == pmp->max_blocks)
    {
      free (p);
      return NULL;
    }
  add1 (pmp->curr_blocks);
  sz = pmp->node_size;
  sft = pmp->shift;
  m = pmp->mask;
  head = i * (m + 1);
  for (i = 0; i < m; i++)
    *(nid *) (p + i * sz) = head + i + 1;
  pn = (cas_t *) (p + m * sz);
  pn->mi = NNULL;
  pn->rfn = 0;
  x.mi = head;
  do
    {
      n.all = recv_queue->all;
      pn->mi = n.mi;
      x.rfn = n.rfn + 1;
    }
  while (!cas (&recv_queue->all, n.all, x.all));
  return (nid *) (p + m * sz);
}

int
init_htab (htab_t *ht, unsigned int num, double ratio)
{
  unsigned long i, nb;
  double r;
  nb = num * ratio;
  for (i = 134217728; nb > i; i *= 2);
  nb = (nb >= 134217728) ? i : nb;
  ht->nb = (i > MAXTAB) ? MAXTAB : ((nb < MINTAB) ? MINTAB : nb);
  ht->n = num;			/* for array tab: n <- 0, nb <- 256, r <- tunning */
  r = (ht->n == 0 ? ratio : ht->nb * 1.0 / ht->n);
  if (!(ht->b = calloc (ht->nb, sizeof (*ht->b))))
    return -1;
  for (i = 0; i < ht->nb; i++)
    ht->b[i] = NNULL;
  printf ("expected nb[%u] = n[%u] * r[%f]\n", (unsigned int) (num * ratio),
	  num, ratio);
  printf ("actual   nb[%ld] = n[%ld] * r[%f]\n", ht->nb, ht->n, r);
  return 0;
}

hash_t *
atomic_hash_create (size_t max_nodes, unsigned long lookup_reset_ttl, callback dtors[])
{
  const double collision = COLLISION; /* collision control, larger is better */
  const size_t max_blocks = MAXBLOCKS;
  hash_t *h;
  htab_t *ht1, *ht2, *at1; /* bucket array 1, 2 and collision array */
  double K, r1, r2;
  unsigned long j, n1, n2;
  if (max_nodes < 2 || max_nodes > MAXTAB)
    {
      printf ("max_nodes range: 2 ~ %ld\n", (unsigned long) MAXTAB);
      return NULL;
    }
  if (posix_memalign((void **)(&h), 64, sizeof(*h)))
    return NULL;
  memset (h, 0, sizeof(*h));


#if defined (MPQ3HASH)
#include "hash_mpq.h"
  init_crypt_table (ct);
  h->hash_func = mpq3hash;
#elif defined (NEWHASH)
#include "hash_newhash.h"
  h->hash_func = newhash;
#elif defined (MD5HASH)
#include "hash_md5.h"
  h->hash_func = md5hash;
#elif defined (MURMUR3HASH_128)
#include "hash_murmur3.h"
  h->hash_func = MurmurHash3_x64_128;
#elif defined (CITY3HASH_128)
#include "hash_city.h"
  h->hash_func = cityhash_128;
#endif
  if (dtors)
    for (j = 0; j < MAX_CALLBACK; j++)
       h->dtor[j] = dtors[j];
  h->reset_expire = lookup_reset_ttl;
  h->nmht = NMHT;
  h->ncmp = NCMP;
  h->nkey = NKEY;                 /* uint32_t # of hash function's output */
  h->npos = h->nkey * NCLUSTER;   /* pos # in one hash table */
  h->nseat = h->npos * h->nmht;   /* pos # in all hash tables */
  h->freelist.mi = NNULL;

  ht1 = &h->ht[0];
  ht2 = &h->ht[1];
  at1 = &h->ht[NMHT];
/* n1 -> n2 -> 1/tuning
 * nb1 = n1 * r1, r1 = ((n1+2)/tuning/K^2)^(K^2 - 1)
 * nb2 = n2 * r2 == nb1 / K == ((n2+2)/tuning/K))^(K - 1)
*/
  printf ("init bucket array 1:\n");
  K = h->npos + 1;
  n1 = max_nodes;
  r1 = pow ((n1 * collision / (K*K)), (1.0 / (K*K-1)));
  if (init_htab (ht1, n1, r1) < 0)
    goto calloc_exit;

  printf ("init bucket array 2:\n");
  n2 = (n1 + 2.0) / (K * pow (r1, K-1));
  r2 = pow (((n2 + 2.0) * collision / K), 1.0/(K-1));
  if (init_htab (ht2, n2, r2) < 0)
    goto calloc_exit;

  printf ("init collision array:\n");
  if (init_htab (at1, 0, collision) < 0)
    goto calloc_exit;

//  h->mp = create_mem_pool (max_nodes, sizeof (node_t), max_blocks);
  h->mp = create_mem_pool (ht1->nb + ht2->nb + at1->nb, sizeof (node_t), max_blocks);
//  h->mp = create_mem_pool (n1 + n2, sizeof (node_t), max_blocks);
  if (!h->mp)
    goto calloc_exit;
  
  h->stats.max_nodes = h->mp->max_blocks * h->mp->blk_node_num;
  h->stats.mem_htabs = ((ht1->nb + ht2->nb) * sizeof (nid)) >> 10;
  h->stats.mem_nodes = (h->stats.max_nodes * h->mp->node_size) >> 10;
  return h;

calloc_exit:
  for (j = 0; j < h->nmht; j++)
    if (h->ht[j].b)
      free (h->ht[j].b);
  destroy_mem_pool (h->mp);
  free (h);
  return NULL;
}

int
atomic_hash_stats (hash_t *h, unsigned int escaped_milliseconds)
{
  const hstats_t *t = &h->stats;
  const htab_t *ht1 = &h->ht[0], *ht2 = &h->ht[1];
  htab_t *p;
  mem_pool_t *m = h->mp;
  unsigned long j, nadd, ndup, nget, ndel, nop, ncur, op = 0, mem = 0;
  double d = 1024.0;
  char *b = "    ";
  mem = (m->curr_blocks * m->blk_node_num * m->node_size) / d;
  printf ("\n");
  printf ("mem_blocks:\t%u/%u, %ux%u bytes per block\n",
          m->curr_blocks, m->max_blocks, m->blk_node_num, m->node_size);
  printf ("mem_actual:\thtabs[%.2f]MB, nodes[%.2f]MB, total[%.2f]MB\n",
	  t->mem_htabs / d, mem / d, (t->mem_htabs + mem) / d);
  printf ("mem_limits:\thtabs[%.2f]MB, nodes[%.2f]MB, total[%.2f]MB\n",
	  t->mem_htabs / d, t->mem_nodes / d,
	  (t->mem_htabs + t->mem_nodes) / d);
  printf ("n1[%ld]/n2[%ld]=[%.3f],  nb1[%ld]/nb2[%ld]=[%.2f]\n",
           ht1->n, ht2->n, ht1->n*1.0/ht2->n, ht1->nb, ht2->nb,
           ht1->nb*1.0/ht2->nb);
  printf ("r1[%f],  r2[%f],  performance_wall[%.1f%%]\n", ht1->nb*1.0/ht1->n,
           ht2->nb*1.0/ht2->n, ht1->n*100.0/(ht1->nb + ht2->nb));
  nop = ncur = nadd = ndup = nget = ndel = 0;
  printf ("---------------------------------------------------------------------------\n");
  printf ("tab n_cur %s%sn_add %s%sn_dup %s%sn_get %s%sn_del\n", b, b, b, b, b, b, b, b);
  for (j = 0; j <= NMHT && (p = &h->ht[j]); j++)
    {
      ncur += p->ncur;
      nadd += p->nadd;
      ndup += p->ndup;
      nget += p->nget;
      ndel += p->ndel;
      printf ("%-4ld%-14ld%-14ld%-14ld%-14ld%-14ld\n",
	      j, p->ncur, p->nadd, p->ndup, p->nget, p->ndel);
    }
  op = ncur + nadd + ndup + nget + ndel + t->get_nohit + t->del_nohit
    + t->add_nosit + t->add_nomem + t->escapes;
  printf ("sum %-14ld%-14ld%-14ld%-14ld%-14ld\n", ncur, nadd, ndup, nget, ndel);
  printf ("---------------------------------------------------------------------------\n");
  printf ("del_nohit %sget_nohit %sadd_nosit %sadd_nomem %sexpires %sescapes\n", b, b, b, b, b);
  printf ("%-14ld%-14ld%-14ld%-14ld%-12ld%-12ld\n", t->del_nohit, t->get_nohit,
	  t->add_nosit, t->add_nomem, t->expires, t->escapes);
  printf ("---------------------------------------------------------------------------\n");
  if (escaped_milliseconds > 0)
    printf ("escaped_time=%.3fs, op=%ld, ops=%.2fM/s\n", escaped_milliseconds * 1.0 / 1000,
	    op, (double) op / 1000.0 / escaped_milliseconds);
  printf ("\n");
  fflush (stdout);
  return 0;
}

int
atomic_hash_destroy (hash_t *h)
{
  unsigned int j;
  if (!h)
    return -1;
  for (j = 0; j < h->nmht; j++)
    free (h->ht[j].b);
  destroy_mem_pool (h->mp);
  free (h);
  return 0;
}

inline nid
new_node (hash_t *h)
{
  memword cas_t n, m;
  while (h->freelist.mi != NNULL || new_mem_block (h->mp, &h->freelist))
    {
      n.all = h->freelist.all;
      if (n.mi == NNULL)
	continue;
      m.mi = ((cas_t *) (i2p (h->mp, node_t, n.mi)))->mi;
      m.rfn = n.rfn + 1;
      if (cas (&h->freelist.all, n.all, m.all))
	return n.mi;
    }
  add1 (h->stats.add_nomem);
  return NNULL;
}

inline void
free_node (hash_t *h, nid mi)
{
  memword cas_t n, m;
  cas_t *p = (cas_t *) (i2p (h->mp, node_t, mi));
  p->rfn = 0;
  m.mi = mi;
  do
    {
      n.all = h->freelist.all;
      m.rfn = n.rfn + 1;
      p->mi = n.mi;
    }
  while (!cas (&h->freelist.all, n.all, m.all));
}

inline void 
set_hash_node (node_t *p, hv v, void *data, unsigned long expire)
{
  p->v = v;
  p->expire = expire;
  p->data = data;
}

inline int likely_equal (hv w, hv v)
{
  return w.y == v.y;
}

/* only called in atomic_hash_get */
inline int
try_get (hash_t *h, hv v, node_t *p, nid *seat, nid mi, int idx, void *arg)
{
  hold_bucket_otherwise_return_0 (p->v, v);
  if (*seat != mi)
    {
      unhold_bucket (p->v, v);
      return 0;
    }
  if (h->dtor[DTOR_TRY_GET] && !h->dtor[DTOR_TRY_GET](p->data, arg))
    {
      if (cas (seat, mi, NNULL))
        atomic_sub1 (h->ht[idx].ncur);
      memset (p, 0, sizeof (*p));
      add1 (h->ht[idx].nget);
      free_node (h, mi);
      return 1;
    }
  if (p->expire > 0 && h->reset_expire > 0)
    p->expire = h->reset_expire + nowms ();
  unhold_bucket (p->v, v);
  add1 (h->ht[idx].nget);
  return 1;
}

/* only called in atomic_hash_add */
inline int
try_hit (hash_t *h, hv v, node_t *p, nid *seat, nid mi, int idx, void *arg)
{
  hold_bucket_otherwise_return_0 (p->v, v);
  if (*seat != mi)
    {
      unhold_bucket (p->v, v);
      return 0;
    }
  if (h->dtor[DTOR_TRY_HIT] && !h->dtor[DTOR_TRY_HIT](p->data, arg))
    {
      if (cas (seat, mi, NNULL))
        atomic_sub1 (h->ht[idx].ncur);
      memset (p, 0, sizeof (*p));
      add1 (h->ht[idx].ndup);
      free_node (h, mi);
      return 1;
    }
  if (p->expire > 0 && h->reset_expire > 0)
    p->expire = h->reset_expire + nowms ();
  unhold_bucket (p->v, v);
  add1 (h->ht[idx].ndup);
  return 1;
}

/* only called in atomic_hash_add */
inline int
try_add (hash_t *h, node_t *p, nid *seat, nid mi, int idx, void *arg)
{
  hvu x = p->v.x;
  p->v.x = 0;
  if (!cas (seat, NNULL, mi))
    {
      p->v.x = x;
      return 0; /* other thread wins, caller to retry other seats */
    }
  atomic_add1 (h->ht[idx].ncur);
  if (h->dtor[DTOR_TRY_ADD] && !h->dtor[DTOR_TRY_ADD](p->data, arg))
    {
      if (cas (seat, mi, NNULL))
        atomic_sub1 (h->ht[idx].ncur);
      memset (p, 0, sizeof (*p));
      free_node (h, mi);
      return 1; /* stop adding this node */
    }
  p->v.x = x;
  add1 (h->ht[idx].nadd);
  return 1;
}

/* only called in atomic_hash_del */
inline int
try_del (hash_t *h, hv v, node_t *p, nid *seat, nid mi, int idx, void *arg)
{
  hold_bucket_otherwise_return_0 (p->v, v);
  if (*seat != mi || !cas (seat, mi, NNULL))
    {
      unhold_bucket (p->v, v);
      return 0;
    }
  atomic_sub1 (h->ht[idx].ncur);
  if (h->dtor[DTOR_TRY_DEL])
    h->dtor[DTOR_TRY_DEL](p->data, arg);
  memset (p, 0, sizeof (*p));
  add1 (h->ht[idx].ndel);
  free_node (h, mi);
  return 1;
}

inline int
valid_ttl (hash_t *h, unsigned long now, node_t *p, nid *seat, nid mi, int idx, void *arg, nid *ret)
{
  unsigned long expire = p->expire;
  if (expire == 0 || expire > now)
     return 1; /* rapid return for most of cases*/
  add1 (h->stats.expires);
  hv v = p->v;
  hold_bucket_otherwise_return_0 (p->v, v);
  if (p->expire == 0 || p->expire > now)
    {
      unhold_bucket (p->v, v);
      return 1; /* caller to go ahead if ttl check meets exception */
    }
  if (*seat != mi || !cas (seat, mi, NNULL))
    {
      unhold_bucket (p->v, v);
      return 0; /* caller to try next bucket */
    }
  atomic_sub1 (h->ht[idx].ncur);
  if (h->dtor[DTOR_EXPIRED])
    h->dtor[DTOR_EXPIRED](p->data, arg);
  memset (p, 0, sizeof (*p));
  if (!ret || !cas(ret, NNULL, mi))
    free_node (h, mi);
  return 0;
}

#if NKEY == 4
#define collect_hash_pos(d, a)  do { register htab_t *pt; i = 0;\
  for (pt = &h->ht[0]; pt < &h->ht[NMHT]; pt++) { \
    a[i++] = &pt->b[d[0] % pt->nb]; \
    a[i++] = &pt->b[d[1] % pt->nb]; \
    a[i++] = &pt->b[d[2] % pt->nb]; \
    a[i++] = &pt->b[d[3] % pt->nb]; \
    for (j = 1; j < NCLUSTER; j++) { \
      a[i++] = &pt->b[(d[3] + j * d[0]) % pt->nb]; \
      a[i++] = &pt->b[(d[0] + j * d[1]) % pt->nb]; \
      a[i++] = &pt->b[(d[1] + j * d[2]) % pt->nb]; \
      a[i++] = &pt->b[(d[2] + j * d[3]) % pt->nb]; \
    } \
  }}while (0)
#elif NKEY == 3
#define collect_hash_pos(d, a)  do { register htab_t *pt; i = 0;\
  for (pt = &h->ht[0]; pt < &h->ht[NMHT]; pt++) { \
    a[i++] = &pt->b[d[0] % pt->nb]; \
    a[i++] = &pt->b[d[1] % pt->nb]; \
    a[i++] = &pt->b[d[2] % pt->nb]; \
    a[i++] = &pt->b[(d[2] + d[0]) % pt->nb]; \
    a[i++] = &pt->b[(d[0] + d[1]) % pt->nb]; \
    a[i++] = &pt->b[(d[1] + d[2]) % pt->nb]; \
    a[i++] = &pt->b[(d[2] - d[0]) % pt->nb]; \
    a[i++] = &pt->b[(d[0] - d[1]) % pt->nb]; \
    a[i++] = &pt->b[(d[1] - d[2]) % pt->nb]; \
  }}while (0)
#endif
/*
#define collect_hash_pos(d, a)  do { register htab_t *pt; j = 0;\
  for (pt = &h->ht[0]; pt < &h->ht[NMHT]; pt++){ \
    for(i = 0; i < NKEY; i++) \
      a[j++] = &pt->b[d[i] % pt->nb]; \
    for(i = 0; i < NKEY; i++) \
      a[j++] = &pt->b[(d[i] + d[(i+1)%NKEY]) % pt->nb]; \
  }}while (0)
*/

#define idx(j) (j<(NCLUSTER*NKEY)?0:1)
int
atomic_hash_add (hash_t *h, void *kwd, size_t len, void *data, int initial_ttl, void *arg)
{
  register unsigned int i, j;
  register nid mi;
  register node_t *p;
  memword nid *a[NSEAT];
  memword union { hv v; nid d[NKEY]; } t;
  nid ni = NNULL;
  unsigned long now = nowms ();
  
  h->hash_func (kwd, len, &t);
  collect_hash_pos(t.d, a);
  for (j = 0; j < NSEAT; j++)
    if ((mi = *a[j]) != NNULL && (p = i2p (h->mp, node_t, mi)))
      if (valid_ttl (h, now, p, a[j], mi, idx(j), arg, &ni))
        if (likely_equal (p->v, t.v) && try_hit (h, t.v, p, a[j], mi, idx(j), arg))
          return 0;
  for (i = h->ht[NMHT].ncur, j = 0; i > 0 && j < MINTAB; j++)
    if ((mi = h->ht[NMHT].b[j]) != NNULL && (p = i2p (h->mp, node_t, mi)) && i--)
      if (valid_ttl (h, now, p, &h->ht[NMHT].b[j], mi, NMHT, arg, &ni))
        if (likely_equal (p->v, t.v) && try_hit (h, t.v, p, &h->ht[NMHT].b[j], mi, NMHT, arg))
	  return 0;
  if (ni == NNULL && (ni = new_node (h)) == NNULL)
    return -2;
  p = i2p (h->mp, node_t, ni);
  set_hash_node (p, t.v, data, (initial_ttl>0 ? initial_ttl+now : 0));
  for (j = 0; j < NSEAT; j++)
    if (*a[j] == NNULL && try_add (h, p, a[j], ni, idx(j), arg))
      return 0;
  if (h->ht[NMHT].ncur < MINTAB)
    for (j = 0; j < MINTAB; j++)
      if (h->ht[NMHT].b[j] == NNULL && try_add (h, p, &h->ht[NMHT].b[j], ni, NMHT, arg))
	return 0;
  memset (p, 0, sizeof (*p));
  free_node (h, ni);
  add1 (h->stats.add_nosit);
  return -1;
}

int
atomic_hash_get (hash_t *h, void *kwd, size_t len, void *arg)
{
  register unsigned int i, j;
  register nid mi;
  register node_t *p;
  memword nid *a[NSEAT];
  memword union { hv v; nid d[NKEY]; } t;
  unsigned long now = nowms ();

  h->hash_func (kwd, len, &t);
  collect_hash_pos(t.d, a);
  for (j = 0; j < NSEAT; j++)
    if ((mi = *a[j]) != NNULL && (p = i2p (h->mp, node_t, mi)))
      if (valid_ttl (h, now, p, a[j], mi, idx(j), arg, NULL))
        if (likely_equal (p->v, t.v) && try_get (h, t.v, p, a[j], mi, idx(j), arg))
	  return 0;
  for (j = i = 0; i < h->ht[NMHT].ncur && j < MINTAB; j++)
    if ((mi = h->ht[NMHT].b[j]) != NNULL && (p = i2p (h->mp, node_t, mi)) && ++i)
      if (valid_ttl (h, now, p, &h->ht[NMHT].b[j], mi, NMHT, arg, NULL))
        if (likely_equal (p->v, t.v) && try_get (h, t.v, p, &h->ht[NMHT].b[j], mi, NMHT, arg))
	  return 0;
  add1 (h->stats.get_nohit);
  return -1;
}

int
atomic_hash_del (hash_t *h, void *kwd, size_t len, void *arg)
{
  register unsigned int i, j;
  register nid mi;
  register node_t *p;
  memword nid *a[NSEAT];
  memword union { hv v; nid d[NKEY]; } t;
  unsigned long now = nowms ();

  h->hash_func (kwd, len, &t);
  collect_hash_pos(t.d, a);
  i = 0; /* delete all matches */
  for (j = 0; j < NSEAT; j++)
    if ((mi = *a[j]) != NNULL && (p = i2p (h->mp, node_t, mi)))
      if (valid_ttl (h, now, p, a[j], mi, idx(j), arg, NULL))
	if (likely_equal (p->v, t.v) && try_del (h, t.v, p, a[j], mi, idx(j), arg))
	  i++;
  if (h->ht[NMHT].ncur > 0)
    for (j = 0; j < MINTAB; j++)
      if ((mi = h->ht[NMHT].b[j]) != NNULL && (p = i2p (h->mp, node_t, mi)))
        if (valid_ttl (h, now, p, &h->ht[NMHT].b[j], mi, NMHT, arg, NULL))
	  if (likely_equal (p->v, t.v) && try_del (h, t.v, p, &h->ht[NMHT].b[j], mi, NMHT, arg))
	    i++;
  if (i > 0)
    return 0;
  add1 (h->stats.del_nohit);
  return -1;
}
