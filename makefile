CFLAGS = -g -O0 -fPIC
LDFLAGS= -Wl,-R -Wl,`$PWD`
-LFLAGS+= -L /home/csurbhi/github/userspace-rbtree/urb
LIBS= -lurb
OBJS= rbtree_test.o

rbtest: liburb.so rbtree_test.o
	gcc $(CFLAGS) -L . -o rbtest rbtree_test.o $(LIBS)

liburb.so: rbtree.o
	gcc -shared -o liburb.so rbtree.o

rbtree.o: rbtree.c rbtree.h rbtree_augmented.h
	gcc $(CFLAGS) -c -Wall -Werror rbtree.c

rbtree_test.o: rbtree_test.c rbtree.h rbtree_augmented.h

ctags: rbtree.c rbtree_test.c rbtree.h rbtree_augmented.h
	ctags rbtree.c rbtree_test.c rbtree.h rbtree_augmented.h
clean:
	rm -f *.o rbtest liburb.so tags
