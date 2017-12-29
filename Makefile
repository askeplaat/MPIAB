#CC=gcc
CC=mpicc -g
#OPT=-g -fcilkplus -O3
#OPT=-g -pg -fcilkplus 
#LIBS=-lcilkrts -lpthread -ldl
DEPS=parabm.h Makefile
SRCS=parab9m.c parab_utilm.c parab_jobqm.c
#OBJS=parab4.o
TARGET=parabm


#%.o: %.c $(DEPS)
#	$(CC) -o $@ $< $(OPT) 

#$(TARGET): $(OBJS)
#	$(CC) $(LIBS)  -o $@ $<


$(TARGET): $(SRCS) $(DEPS)
	$(CC) -o $(TARGET) $(OPT) $(LIBS) $(SRCS)


#time CILK_WORKERS=16 ./parab

