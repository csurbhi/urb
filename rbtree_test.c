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




static struct extent *stl_rb_geq(off_t lba)
{
	struct extent *e = NULL;

	e = _stl_rb_geq(&extent_tbl_root, lba);
	return e;
}


static int check_node_contents(struct rb_node *node)
{
	int ret = 0;
	struct extent *e, *next, *prev;

	if (!node)
		return -1;

	e = rb_entry(node, struct extent, rb);

	if (e->lba < 0) {
		printf("\n LBA is <=0, tree corrupt!! \n");
		return -1;
	}
	if (e->pba <= 0) {
		printf("\n PBA is <=0, tree corrupt!! \n");
		return -1;
	}
	if (e->len <= 0) {
		printf("\n len is <=0, tree corrupt!! \n");
		return -1;
	}

	next = lsdm_rb_next(node);
	prev = lsdm_rb_prev(node);

	if (next  && next->lba == e->lba) {
		printf("\n LBA corruption (next) ! lba: %d is present in two nodes!", next->lba);
		printf("\n next->lba: %d next->pba: %d next->len: %d", next->lba, next->pba, next->len);
		return -1;
	}

	if (next && e->lba + e->len == next->lba) {
		if (e->pba + e->len == next->pba) {
			printf("\n Nodes not merged! ");
			printf("\n e->lba: %d e->pba: %d e->len: %d", e->lba, e->pba, e->len);
			printf("\n next->lba: %d next->pba: %d next->len: %d", next->lba, next->pba, next->len);
			return -1;
		}
	}

	if (prev && prev->lba == e->lba) {
		printf("\n LBA corruption (prev)! lba: %d is present in two nodes!", prev->lba);
		return -1;
	}

	if (prev && prev->lba + prev->len == e->lba) {
		if (prev->pba + prev->len == e->pba) {
			printf("\n Nodes not merged! ");
			printf("\n e->lba: %d e->pba: %d e->len: %d", e->lba, e->pba, e->len);
			printf("\n prev->lba: %d prev->pba: %d prev->len: %d", prev->lba, prev->pba, prev->len);
			return -1;

		}
	}

	if(node->rb_left)
		ret = check_node_contents(node->rb_left);

	if (ret < 0)
		return ret;

	if (node->rb_right)
		ret = check_node_contents(node->rb_right);

	return ret;

}

static int lsdm_tree_check()
{
	struct rb_node *node = extent_tbl_root.rb_node;
	int ret = 0;

	ret = check_node_contents(node);
	printf("\n");
	return ret;

}

static void lsdm_rb_remove(struct extent *e)
{
	struct rb_root *root = &extent_tbl_root;
	rb_erase(&e->rb, root);
	n_extents--;
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




int _stl_verbose;
static int lsdm_rb_insert(struct extent *new)
{
	struct rb_root *root = &extent_tbl_root;
	struct rb_node **link = &root->rb_node, *parent = NULL;
	struct extent *e = NULL;
	int ret = 0;

	RB_CLEAR_NODE(&new->rb);

	/* Go to the bottom of the tree */
	while (*link) {
		parent = *link;
		e = rb_entry(parent, struct extent, rb);
		if ((new->lba + new->len) <= e->lba) {
			link = &(*link)->rb_left;
		} else if (new->lba >= (e->lba + e->len)){
			link = &(*link)->rb_right;
		} else {
			/* overlapping indicates (new->lba >= e->lba)
			 */
			if ((new->pba >= e->pba) && (new->pba <= e->pba + e->len)) {
				printf("\n Overlapping node found 1!");
				printf("\n e->lba: %d e->pba: %d e->len: %d", e->lba, e->pba, e->len);
				printf("\n new->lba: %d new->pba: %d new->len: %d \n", new->lba, new->pba, new->len);
				exit(-1);
			}
			if ((new->pba <= e->pba) && ((new->pba + new->len) > e->pba)) {
				printf("\n Overlapping node found 2!");
				printf("\n e->lba: %d e->pba: %d e->len: %d", e->lba, e->pba, e->len);
				printf("\n new->lba: %d new->pba: %d new->len: %d \n", new->lba, new->pba, new->len);
				exit(-1);
			}
			printf("\n e->lba: %d e->pba: %d e->len: %d", e->lba, e->pba, e->len);
			printf("\n new->lba: %d new->pba: %d new->len: %d \n", new->lba, new->pba, new->len);
			link = &(*link)->rb_right;
		}
	}
	/* Put the new node there */
	rb_link_node(&new->rb, parent, link);
	rb_insert_color(&new->rb, root);
	merge(new);
	ret = lsdm_tree_check();
	if (ret < 0) {
		printf("\n !!!! Corruption while Inserting: lba: %d pba: %d len: %d", new->lba, new->pba, new->len);
		return -1;
	}
	return 0;
}


/* Update mapping. Removes any total overlaps, edits any partial
 * overlaps, adds new extent to map.
 */
static int lsdm_update_range(sector_t lba, sector_t pba, int len)
{
	struct extent *e = NULL, *new = NULL, *split = NULL, *next=NULL, *prev=NULL;
	struct extent *tmp = NULL;
	struct rb_node *node = extent_tbl_root.rb_node;  /* top of the tree */
	struct rb_node *left;
	struct extent *e_left;
	int diff = 0;
	int i=0;
	int ret=0;
	int flag = 0;
	struct extent olde;

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
			printf("\n stuck in a while loop, why are you here ?");
			printf("\n %s lba: %d, pba: %d, len:%d ", __func__, lba, pba, len);
			printf("\n %s e->lba: %d, e->pba: %d, e->len:%d ", __func__, e->lba, e->pba, e->len);
			printf("\n");
			exit(-1);
		}
		e = rb_entry(node, struct extent, rb);
		/* No overlap */
		if ((lba + len) <= e->lba) {
			node = node->rb_left;
			continue;
		}
		if (lba >= (e->lba + e->len)) {
			node = node->rb_right;
			continue;
		}
		break;
	}
	/* There is no node with which this lba overlaps with */
	if (!node) {
		/* new node has to be added */
		printf( "\n %s flag: %d Inserting (lba: %u pba: %u len: %d) ", __func__, flag, new->lba, new->pba, new->len);
		printf("\n----------------- \n");
		ret = lsdm_rb_insert(new);
		if (ret < 0) {
			printf("\n Corruption in case 8!! ");
			printf ("\n lba: %d pba: %d len: %d ", lba, pba, len);
			printf ("\n e->lba: %d e->pba: %d e->len: %d ", e->lba, e->pba, e->len);
			printf("\n");
			exit(-1);
		}
		return;
	}
	/* We have found a "node"  that overlaps with our lba, pba,
	 * len trio
	 */

	 /*
	 * Case 1: overwrite a part of the existing extent
	 * 	++++++++++
	 * ----------------------- 
	 *
	 *  No end matches!!
	 */

	if ((lba > e->lba)  && (lba + len < e->lba + e->len)) {
		printf("\n case1 ! e->lba: %d e->pba: %d e->len: %d", e->lba, e->pba, e->len);
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
		ret = lsdm_rb_insert(new);
		if (ret < 0) {
			printf ("\n lba: %d pba: %d len: %d ", lba, pba, len);
			printf ("\n e->lba: %d e->pba: %d e->len: %d ", e->lba, e->pba, e->len);
			printf("\n");
			exit(-1);
		}
		extent_init(split, lba + len, e->pba + (diff + len), e->len - (diff + len));
		ret = lsdm_rb_insert(split);
		if (ret < 0) {
			printf("\n Corruption in case 1!! ");
			printf ("\n lba: %d pba: %d len: %d ", lba, pba, len);
			printf ("\n e->lba: %d e->pba: %d e->len: %d ", e->lba, e->pba, e->len);
			printf ("\n split->lba: %d split->pba: %d split->len: %d ", split->lba, split->pba, split->len);
			printf("\n");
			exit(-1);
		}
		return 0;
	}

	/* Start from the smallest node that overlaps*/
	prev = lsdm_rb_prev(e);
	while(prev) {
		if (prev->lba + prev->len <= lba)
			break;
		prev = lsdm_rb_prev(e);
		e = prev;
	}

	/* Now we consider the overlapping "e's" in an order of
	 * increasing LBA
	 */

	/* 
	 * Case 2: Overwrites an existing extent partially;
	 * covers only the right portion of an existing extent (e)
	 * 	++++++++
	 * ----------- 
	 *  e
	 *
	 *
	 * 	++++++++
	 * ------------- 
	 *
	 * (Right end of e1 and + could match!)  
	 *
	 */
	if ((lba > e->lba) && ((lba + len) >= (e->lba + e->len))) {
		e->len = lba - e->lba;
		e = lsdm_rb_next(e);
		/*  
		 *  process the next overlapping segments!
		 *  Fall through to the next case.
		 */
	}

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
		printf("\n case3 ! e->lba: %d e->pba: %d e->len: %d", e->lba, e->pba, e->len);
		tmp = lsdm_rb_next(e);
		lsdm_rb_remove(e);
		free(e);
		e = tmp;
	}
	if (!e || (e->lba >= (lba + len)))  {
		/* No overlap with any more e */
		ret = lsdm_rb_insert(new);
		if (ret < 0) {
			printf("\n Corruption in case 3!! ");
			printf ("\n lba: %d pba: %d len: %d ", lba, pba, len);
			printf ("\n e->lba: %d e->pba: %d e->len: %d ", e->lba, e->pba, e->len);
			printf("\n");
			exit(-1);
		}
		return 0;
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
	if ((lba <= e->lba) && (lba + len > e->lba) && (lba + len < e->lba + e->len))  {
		printf("\n case4 ! e->lba: %d e->pba: %d e->len: %d", e->lba, e->pba, e->len);
		diff = lba + len - e->lba;
		lsdm_rb_remove(e);
		e->lba = e->lba + diff;
		e->len = e->len - diff;
		e->pba = e->pba + diff;
		printf("\n e snipped! e->lba: %d e->pba: %d e->len: %d", e->lba, e->pba, e->len);
		ret = lsdm_rb_insert(new);
		if (ret < 0) {
			printf("\n Corruption in case 4!! ");
			printf ("\n lba: %d pba: %d len: %d ", lba, pba, len);
			printf ("\n e->lba: %d e->pba: %d e->len: %d ", e->lba, e->pba, e->len);
			printf("\n");
			exit(-1);
		}
		ret = lsdm_rb_insert(e);
		if (ret < 0) {
			printf("\n Corruption in case 4!! ");
			printf ("\n lba: %d pba: %d len: %d ", lba, pba, len);
			printf ("\n e->lba: %d e->pba: %d e->len: %d ", e->lba, e->pba, e->len);
			printf("\n");
			exit(-1);
		}
		return(0);
	}

	/* Case 5:
	 *
	 * Right end of + and - matches.
	 *
	 *  		+++++++++
	 * ----------------------
	 */
	if ((lba > e->lba) && (lba + len == e->lba + e->len)) {
		printf("\n case5 ! e->lba: %d e->pba: %d e->len: %d", e->lba, e->pba, e->len);
		diff = e->lba - lba;
		e->len = diff;
		ret = lsdm_rb_insert(new);
		if (ret < 0) {
			printf("\n Corruption in case 5!! ");
			printf ("\n lba: %d pba: %d len: %d ", lba, pba, len);
			printf ("\n e->lba: %d e->pba: %d e->len: %d ", e->lba, e->pba, e->len);
			printf("\n");
			exit(-1);
		}
		return(0);
	}

	/* If you are here then you haven't covered some
	 * case!
	 */
	printf("\n why are you here ? flag: %d", flag);
	printf("\n %s lba: %d, pba: %d, len:%d ", __func__, lba, pba, len);
	if (flag)
		printf("\n %s olde.lba: %d, olde.pba: %d, olde.len:%d ", __func__, olde.lba, olde.pba, olde.len);
	printf("\n %s e->lba: %d, e->pba: %d, e->len:%d ", __func__, e->lba, e->pba, e->len);
	printf("\n");
	exit(-1);

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

#define NUM 3051

void overwrite()
{
	for(int i=0; i<NUM; i++) {
		//printf("\n %d %d %d ", trio[i][0], trio[i][1], trio[i][2]);
		lsdm_update_range(replace[i][0], replace[i][1], replace[i][2]);
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
		if (replace[i][0] < 0) {
			printf("\n LBA is indeed < 0");
			exit(-1);
		}
		if (replace[i][1] <= 0) {
			printf("\n PBA is indeed < 0");
			exit(-1);
		}
		if (replace[i][2] <= 0) {
			printf("\n len is indeed < 0");
			exit(-1);
		}

	}

	for(i=0; i<NUM; i++) {
		//printf("\n %d %d %d ", trio[i][0], trio[i][1], trio[i][2]);
		lsdm_update_range(replace[i][0], replace[i][1], replace[i][2]);
	}
	start_printing();
	getchar();

	overwrite();
	return 0; /* Fail will directly unload the module */
}
