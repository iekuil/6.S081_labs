#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define	READ	0
#define	WRITE	1

int write_int(int fd, int num);
int read_int(int fd, int *num);

int main()
{
	int mypipe[2][2];
	pipe(mypipe[0]);
	int num_1;
	int num_2;

	if ( fork() > 0)
	{
		close(mypipe[0][READ]);
		for ( num_1 = 2; num_1 <36; num_1++)
			write_int(mypipe[0][WRITE], num_1);
		int chld_status;
		close(mypipe[0][WRITE]);
		wait(&chld_status);	
		exit(0);
	}
	int num_count;
	int chld_num = -1;
	close(mypipe[0][WRITE]);	// mypipe[write_p][WRITE] in child
	while(1)
	{
		chld_num++;

		int read_p = chld_num % 2;
		int write_p = (chld_num+1) % 2;
		int read_ret;

		pipe(mypipe[write_p]);

	
		num_count = 1;
		read_ret = read_int( mypipe[read_p][READ], &num_1 );
		printf("prime %d\n", num_1);

		while( (read_ret=read_int(mypipe[read_p][READ], &num_2)) > 0 )
		{
			if ( num_2 % num_1 != 0)
			{
				write_int(mypipe[write_p][WRITE], num_2);
			}
			num_count++;	
		}

		close(mypipe[read_p][READ]);	// mypipe[write_p][READ] in child
		close(mypipe[write_p][WRITE]);	// mypipe[read_p][WRITE] in child
		if ( num_count == 1)
		{
			break;
		}
		if ( fork() > 0)
		{
			close(mypipe[write_p][READ]);	// mypipe[read_p][READ] in child
			break;
		}
	}
	int chld_status;
	if ( num_count != 1)
		wait(&chld_status);
	exit(0);
}

int write_int(int fd, int num)
{
	return write(fd, &num, 4);
}

int read_int(int fd, int *num)
{
	return read(fd, num, 4);
}
