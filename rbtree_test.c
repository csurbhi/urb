 /* Author : Muhammad Falak R Wani (mfrw)
 * This program is adapted from the linux kernel and in no way,
 * I claim I have any copyright.
 */

#include<stdio.h>
#include<stdlib.h>
#include<time.h>
#include<stdint.h>
#include"rbtree.h"
#include"rbtree_augmented.h"
#include<sys/time.h>
#include<errno.h>
#include<assert.h>
#include<string.h>
#include "rbtree_array.h"


#define NODES       2000
#define PERF_LOOPS  100
#define CHECK_LOOPS 10

#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)


/* total size = xx bytes (64b). fits in 1 cache line
   for 32b is xx bytes, fits in ARM cache line */
struct extent {
        struct rb_node rb;      /* 20 bytes */
        sector_t lba;           /* 512B LBA */
        sector_t pba;
        __u32      len;
}; /* xx bytes including padding after 'rb', xx on 32-bit */

static struct rb_root extent_tbl_root = RB_ROOT;
static struct extent nodes[NODES];
static int n_extents = 0;


/************** Extent map management *****************/

static void extent_init(struct extent *e, sector_t lba, sector_t pba, unsigned len)
{
        memset(e, 0, sizeof(*e));
        e->lba = lba;
        e->pba = pba;
        e->len = len;
}

/* find a map entry containing 'lba' or the next higher entry.
 * see Documentation/rbtree.txt
 */
static struct extent *_stl_rb_geq(struct rb_root *root, off_t lba)
{
	struct rb_node *node = root->rb_node;  /* top of the tree */
	struct extent *higher = NULL;
	struct extent *e = NULL;

	while (node) {
		e = container_of(node, struct extent, rb);
		if (e->lba >= lba && (!higher || e->lba < higher->lba)) {
			higher = e;
		}
		if (lba < e->lba) {
			node = node->rb_left;
		} else {
			if (lba >= e->lba + e->len) {
				node = node->rb_right;
			} else {
				/* lba falls within "e"
				 * (lba >= e->lba) && (lba < (e->lba + e->len)) */
				return e;
			}
		}
	}
	return higher;
}

static struct extent *stl_rb_geq(off_t lba)
{
	struct extent *e = NULL;

	e = _stl_rb_geq(&extent_tbl_root, lba);
	return e;
}


int _stl_verbose;
static void lsdm_rb_insert(struct extent *new)
{
	struct rb_root *root = &extent_tbl_root;
	struct rb_node **link = &root->rb_node, *parent = NULL;
	struct extent *e = NULL;

	RB_CLEAR_NODE(&new->rb);

	/* Go to the bottom of the tree */
	while (*link) {
		parent = *link;
		if (new->lba < rb_entry(parent, struct extent, rb)->lba) {
			link = &(*link)->rb_left;
		} else {
			link = &(*link)->rb_right;
		}
	}
	/* Put the new node there */
	rb_link_node(&new->rb, parent, link);
	rb_insert_color(&new->rb, root);
}

static void lsdm_rb_remove(struct extent *e)
{
	struct rb_root *root = &extent_tbl_root;
	rb_erase(&e->rb, root);
	n_extents--;
}


static struct extent *lsdm_rb_next(struct extent *e)
{
	struct rb_node *node = rb_next(&e->rb);
	return (node == NULL) ? NULL : container_of(node, struct extent, rb);
}

static struct extent *lsdm_rb_prev(struct extent *e)
{
        struct rb_node *node = rb_prev(&e->rb);
        return (node == NULL) ? NULL : container_of(node, struct extent, rb);
}

/* Check if we can be merged with the left or the right node */
static struct extent *merge(struct extent *e)
{
	struct extent *prev, *next;

	prev = lsdm_rb_prev(e);
	next = lsdm_rb_next(e);
	if (prev) {
		if(prev->lba + prev->len == e->lba) {
			if (prev->pba + prev->len == e->pba) {
				prev->len += e->len;
				lsdm_rb_remove(e);
				free(e);
				e = prev;
			}
		}
	}
	if (next) {
		if (next->lba == e->lba + e->len) {
			if (next->pba == e->pba + e->len) {
				e->len += next->len;
				lsdm_rb_remove(next);
				free(next);
			}
		}
	}
}

/* Update mapping. Removes any total overlaps, edits any partial
 * overlaps, adds new extent to map.
 */
static int lsdm_update_range(sector_t lba, sector_t pba, size_t len)
{
	struct extent *e = NULL, *new = NULL, *split = NULL, *next=NULL, *prev=NULL;
	struct extent *tmp = NULL;
	struct rb_node *node = extent_tbl_root.rb_node;  /* top of the tree */
	int diff = 0;
	int i=0;

	assert(len != 0);

	//printf("\n %s lba: %d, pba: %d, len:%ld ", __func__, lba, pba, len);
	new = malloc(sizeof(struct extent ));
	if (unlikely(!new)) {
		return -ENOMEM;
	}
	extent_init(new, lba, pba, len);
	while (node) {
		i++;
		if (i==150) {
			printf("\n why are you here ?");
			printf("\n %s lba: %d, pba: %d, len:%ld ", __func__, lba, pba, len);
			printf("\n %s e->lba: %d, e->pba: %d, e->len:%d ", __func__, e->lba, e->pba, e->len);
			printf("\n");
			exit(-1);
		}
		e = rb_entry(node, struct extent, rb);
		/* No overlap */
		if (lba + len < e->lba) {
			node = node->rb_left;
			continue;
		}
		if (lba > e->lba + e->len) {
			node = node->rb_right;
			continue;
		}

		 /*
		 * Case 1: overwrite a part of the existing extent
		 * 	++++++++++
		 * ----------------------- 
		 *
		 *  No end matches!!
		 */

		if ((lba > e->lba)  && (lba + len < e->lba + e->len)) {
			split = malloc(sizeof(struct extent));
			if (!split) {
				free(new);
				return -ENOMEM;
			}
			diff =  lba - e->lba;
			/* new should be physically discontiguous
			 */
			//assert(e->pba + diff !=  pba);
			e->len = diff;
			lsdm_rb_insert(new);
			extent_init(split, lba + len, e->pba + (diff + len), e->len - (diff + len));
			lsdm_rb_insert(split);
			break;
		}


		/* 
		 * Case 2: Overwrites an existing extent partially;
		 * covers only the right portion of an existing extent (e1)
		 * Right end of e1 does not match with "+"
		 *
		 * 		+++++++++++++++++++++
		 * ---------------   --------    -----------
		 *  e1			e2	e3 
		 *
		 */
		if ((lba > e->lba) && (lba < e->lba + e->len)) {
			diff = (e->lba + e->len) - lba;
			e->len = e->len - diff;
			e = lsdm_rb_next(e);
			/*  
			 *  process the next overlapping segments!
			 *  Fall through to the next case.
			 */
		}
		/* else if
		 * (lba > e->lba) && (lba > e->lba + e->len)
		 *
		 *	++++++++++++++++
		 * ------------
		 *
		 *  else if 
		 *  (lba < e->lba) &&  (lba > e->lba + e->len)
		 *   
		 *   +++++++++++++
		 *   	-----
		 *   
		 *
		 *  else
		 *  (lba < e->lba) && (lba < e->lba + e->len)
		 *  
		 *  ++++++++
		 *   ----
		 *  or 
		 *
		 *  +++++++
		 *    --------
		 *
		 * All these cases are taken care of in different
		 * cases.
		 *
		 */


		/* 
		 * Case 3: Overwrite many extents completely
		 *	++++++++++++++++++++
		 *	  ----- ------  --------
		 *
		 * Could also be exact same: 
		 * 	+++++
		 * 	-----
		 * But this case is optimized in case 1.
		 * We need to remove all such e
		 * the last e can have case 1
		 *
		 * here we compare left ends and right ends of 
		 * new and existing node e
		 */
		while ((e!=NULL) && (lba <= e->lba) && ((lba + len) >= (e->lba + e->len))) {
			tmp = lsdm_rb_next(e);
			lsdm_rb_remove(e);
			free(e);
			e = tmp;
		}
		if (!e || (e->lba > lba + len))  {
			lsdm_rb_insert(new);
			merge(new);
			break;
		}
		/* else fall down to the next case for the last
		 * component that overwrites an extent partially
		 */

		/* 
		 * Case 4: 
		 * Partially overwrite an extent
		 * ++++++++++
		 * 	-------------- OR
		 *
		 * Left end of + and - matches!
		 * +++++++
		 * --------------
		 *
		 */
		if ((lba <= e->lba) && (lba + len > e->lba)) {
			diff = lba + len - e->lba;
			lsdm_rb_remove(e);
			e->lba = e->lba + diff;
			e->len = e->len - diff;
			e->pba = e->pba + diff;
			lsdm_rb_insert(new);
			lsdm_rb_insert(e);
			merge(new);
			break;
		}

		/* Case 5 
		 * 	    +++++++++
		 * ---------
		 *  Right end of - matches left end of +
		 *  Merge if the pba matches.
		 */
		if (e->lba + e->len == lba) {
			if(e->pba + e->len == pba) {
				e->len += len;
				free(new);
				break;
			}
			// else
			lsdm_rb_insert(new);
			merge(new);
			break;
		}

		/* Case 6
		 *
		 * Left end of - matches with right end of +
		 * +++++++
		 *        ---------
		 */
		if (lba + len == e->lba) {
			if (pba + len == e->pba) {
				len += e->len;
				lsdm_rb_remove(e);
				e->lba = lba;
				e->pba = pba;
				e->len = len;
				lsdm_rb_insert(e);
				break;
			}
			// else
			lsdm_rb_insert(new);
			merge(new);
			break;
		}
		/* If you are here then you haven't covered some
		 * case!
		 */
		printf("\n why are you here ?");
		printf("\n %s lba: %d, pba: %d, len:%ld ", __func__, lba, pba, len);
		printf("\n %s e->lba: %d, e->pba: %d, e->len:%d ", __func__, e->lba, e->pba, e->len);
		printf("\n");
		exit(-1);
	}
	if (!node) {
		/* new node has to be added */
		lsdm_rb_insert(new);
		//printk( "\n %s Inserted (lba: %u pba: %u len: %d) ", __func__, new->lba, new->pba, new->len);
	}
	return 0;
}

static void print_tree_contents(struct rb_node *node)
{

	if (!node)
		return;

	struct extent *e = rb_entry(node, struct extent, rb);

	printf("\n %d %d %d", e->lba, e->pba, e->len);

	if(node->rb_left)
		print_tree_contents(node->rb_left);
	if (node->rb_right)
		print_tree_contents(node->rb_right);
}

static void start_printing()
{
	struct rb_node *node = extent_tbl_root.rb_node;
	print_tree_contents(node);
	printf("\n");
}



static void init(void)
{
	int i;
	unsigned int lba = 0, pba = 0, len=0;
	for (i = 0; i < NODES; i++) {
		extent_init(&nodes[i], lba, pba, len);
		lba = lba + 20;
		len = len + 20;
		pba = pba + 20;
	}
}

static int is_red(struct rb_node *rb)
{
	return !(rb->__rb_parent_color & 1);
}

static int black_path_count(struct rb_node *rb)
{
	int count;
	for (count = 0; rb; rb = rb_parent(rb))
		count += !is_red(rb);
	return count;
}

static void check_postorder_foreach(int nr_nodes)
{
	struct extent *cur, *n;
	int count = 0;
	rbtree_postorder_for_each_entry_safe(cur, n, &extent_tbl_root, rb)
		count++;

}

static void check_postorder(int nr_nodes)
{
	struct rb_node *rb;
	int count = 0;
	for (rb = rb_first_postorder(&extent_tbl_root); rb; rb = rb_next_postorder(rb))
		count++;

}

static void check(int nr_nodes)
{
	struct rb_node *rb;
	int count = 0, blacks = 0;
	uint32_t prev_key = 0;

	for (rb = rb_first(&extent_tbl_root); rb; rb = rb_next(rb)) {
		struct extent *node = rb_entry(rb, struct extent, rb);
		if (!count)
			blacks = black_path_count(rb);
		else
		prev_key = node->lba;
		count++;
	}


	check_postorder(nr_nodes);
	check_postorder_foreach(nr_nodes);
}

static void check_augmented(int nr_nodes)
{
	struct rb_node *rb;

	check(nr_nodes);
	for (rb = rb_first(&extent_tbl_root); rb; rb = rb_next(rb)) {
		struct extent *node = rb_entry(rb, struct extent, rb);
	}
}

#define NUM 1066

void overwrite()
{
	for(int i=0; i<NUM; i++) {
		//printf("\n %d %d %d ", trio[i][0], trio[i][1], trio[i][2]);
		lsdm_update_range(trio[i][0], trio[i][1], trio[i][2]);
	}
	start_printing();
}

int main(void)
{
	int i, j;
	sector_t lba, pba;
       	size_t len;
	
	printf("rbtree testing\n");

	for(i=0; i<NUM; i++) {
		//printf("\n %d %d %d ", trio[i][0], trio[i][1], trio[i][2]);
		lsdm_update_range(trio[i][0], trio[i][1], trio[i][2]);
	}
	start_printing();
	getchar();

	overwrite();
	return 0; /* Fail will directly unload the module */
}
