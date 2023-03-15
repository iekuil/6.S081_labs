#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
	int pipe_1[2];
	int pipe_2[2];
	pipe(pipe_1);
	pipe(pipe_2);

	int mypid;
	char c = 'a';
	if (fork() == 0)
	{
		mypid = getpid();
		read(pipe_1[0], &c, 1);
		printf("%d: received ping\n", mypid);
		write(pipe_2[1], &c, 1);
		exit(0);	
	}	
	//int child_status;
	mypid = getpid();
	write(pipe_1[1], &c, 1); 
	read(pipe_2[0], &c, 1);
	//wait(&child_status);
	printf("%d: received pong\n", mypid);
	exit(0);
}
