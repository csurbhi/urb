/*
 * Author : Muhammad Falak R Wani (mfrw)
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
#include<linux/types.h>
#include<errno.h>
#include<assert.h>
#include<string.h>


#define NODES       100
#define PERF_LOOPS  100
#define CHECK_LOOPS 10

#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)


typedef int sector_t;


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
static void stl_rb_insert(struct extent *new)
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

static void stl_rb_remove(struct extent *e)
{
	struct rb_root *root = &extent_tbl_root;
	rb_erase(&e->rb, root);
	n_extents--;
}


static struct extent *stl_rb_next(struct extent *e)
{
	struct rb_node *node = rb_next(&e->rb);
	return (node == NULL) ? NULL : container_of(node, struct extent, rb);
}

/* Update mapping. Removes any total overlaps, edits any partial
 * overlaps, adds new extent to map.
 * if blocked by cleaning, returns the extent which we're blocked on.
 */
static int stl_update_range(sector_t lba, sector_t pba, size_t len)
{
	struct extent *e = NULL, *new = NULL, *split = NULL;
	off_t new_lba, new_pba;
	size_t new_len;
	struct extent *tmp = NULL;
	struct rb_node *node = extent_tbl_root.rb_node;  /* top of the tree */
	struct extent *higher = NULL;
	int diff = 0;

	assert(len != 0);


	new = malloc(sizeof(struct extent));
	if (unlikely(!new)) {
		return -ENOMEM;
	}

	extent_init(new, lba, pba, len);
	while (node) {
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
		/* no overlap, but sequential node; MERGE this new
		 * node with existing node!
		 *
		 * Note that stl_update_range is called asynchronously
		 * and so may not be called sequentially!
		 *
		 * In theory this merge can cause a merge with more
		 * nodes on the left! But we dont take care of this
		 * situation right now. We can always go through the
		 * entire tree to merge the nodes!
		 */
		if (lba + len == e->lba) {
			if (pba + len == e->pba) {
				stl_rb_remove(e);
				e->len += len;
				e->lba = lba;
				e->pba = pba;
				stl_rb_insert(e);
				break;
			}
			/* else we cannot merge as physically
			 * discontiguous
			 */
			node = node->left;
			continue;
		}


		if (lba == e->lba + e->len) {
			if (pba == e->pba + e->len) {
				e->len = e->len + len;
				break;
			}
			/* else we cannot merge as physically
			 * discontiguous
			 */
			node = node->rb_right;
			continue;
		}
		/* new overlaps with e
		 * new: ++++
		 * e: --
		 */

		/* 
		 * Case 1:
		 *
		 * 		+++++++++++
		 * ---------------
		 */
		if ((lba > e->lba) && (lba < e->lba + len)) {
			diff = (e->lba + e->len) - lba;
			e->len = e->len - diff;
			stl_rb_insert(new);
			break;
		}


		 /*
		 * Case2: complete overlap
		 * 	++++++++++
		 * -----------------------
		 *
		 */
		if ((lba > e->lba)  && (lba + len < e->lba + e->len)) {
			split = malloc(sizeof(struct extent));
			diff = e->lba - lba;
			/* new should be physically discontiguous
			 */
			assert(e->pba + diff !=  pba);
			e->len = diff;
			stl_rb_insert(new);
			extent_init(split, lba + len, e->pba + (diff + len), e->len - (diff + len));
			stl_rb_insert(split);
			break;
		}


		/* 
		 * Case 3:
		 *	++++++++++++++++++++
		 *	  ----- ------  --------
		 * We need to remove all such e
		 * the last e can have case 1
		 *
		 * here we compare left ends and right ends of 
		 * new and existing node e
		 */
		while ((e!=NULL) && (lba < e->lba) && ((lba + len) > (e->lba + e->len))) {
			tmp = stl_rb_next(e);
			stl_rb_remove(e);
			e = tmp;
		}
		if (!e) {
			stl_rb_insert(new);
			break;
		}
		/* else fall down to the next case for the last
		 * component that partially overlaps*/

		/* 
		 * Case 4: 
		 *
		 * ++++++++++
		 * 	--------------
		 */
		if ((lba < e->lba) && (lba + len > e->lba)) {
			diff = lba + len - e->lba;
			e->lba = e->lba + diff;
			e->len = e->len - diff;
			e->pba = e->pba + diff;
			stl_rb_insert(new);
			break;
		}
	}
	if (!node) {
		/* new node has to be added */
		stl_rb_insert(new);
		printf( "\n Inserted (lba: %u pba: %u len: %d) ", new->lba, new->pba, new->len);
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

int trio[38][3] = {
	{39321472, 1048576, 128},
{10264, 1050752, 2048},
{8216, 1048704, 2048},
{272408, 1054848, 2048},
{270360, 1052800, 2048},
{524304, 1056896, 2048},
{526352, 1058944, 2048},
{796696, 1063040, 2048},
{794648, 1060992, 2048},
{1050640, 1067136, 2048},
{1048592, 1065088, 2048},
{1318936, 1069184, 2048},
{1320984, 1071232, 2048},
{1572880, 1073280, 2048},
{1574928, 1075328, 2048},
{1845272, 1079424, 2048},
{1843224, 1077376, 2048},
{2099216, 1083520, 2048},
{2097168, 1081472, 2048},
{2367512, 1085568, 2048},
{2369560, 1087616, 2048},
{2621456, 1089664, 2048},
{2623504, 1091712, 2048},
{2883600, 1093760, 2048},
{2885648, 1095808, 2048},
{3145744, 1097856, 2048},
{3147792, 1099904, 2048},
{3407888, 1101952, 2048},
{3409936, 1104000, 2048},
{3670032, 1106048, 2048},
{3672080, 1108096, 2048},
{3934224, 1112192, 2048},
{4196368, 1116288, 2048},
{3932176, 1110144, 2048},
{4458512, 1120384, 2048},
{4456464, 1118336, 2048},
{4194320, 1114240, 2048},
{4718608, 1122432, 2048},
};

int main(void)
{
	int i, j;
	sector_t lba, pba;
       	size_t len;
	
	printf("rbtree testing\n");

	for(i=0; i<38; i++) {
		stl_update_range(trio[i][0], trio[i][1], trio[i][2]);
	}
	start_printing();
	getchar();
	for(i=0; i<2; i++) {
		stl_update_range(lba, pba, len);
		lba = lba + 150;
		pba = pba + 100;
	}
	start_printing();
	getchar();
	for(i=0; i<2; i++) {
		stl_update_range(lba, pba, len);
		lba = lba - 175;
		pba = pba + 100;
	}
	start_printing();
	getchar();
	return 0; /* Fail will directly unload the module */
}
