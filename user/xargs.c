#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

int read_line(char *buf);
void add_args(char *argv[], int old_argc, char *buf, int line_len);

int main(int argc, char *argv[])
{
	if ( argc < 2)
	{
		printf("xargs: need argument(s)\n");
		exit(0);
	}		
	char buf[512];
	int line_len;

	while ( (line_len = read_line(buf))!= 0)
	{
		if ( fork() > 0)
		{
			int chld_status;
			wait(&chld_status);
		}	
		else
		{
			char *new_argv[32];
			int i;
			for(i = 0; i < argc-1; i++)
				new_argv[i] = argv[i+1];	
			add_args(new_argv, argc-1, buf, line_len);
			//for (int i=0; i<5; i++)
			//	printf("arg[%d]: %s\n",i, argv[i]);
			exec(new_argv[0], new_argv);
		}
	}

	exit(0);	
}

int read_line(char *buf)
{
	int len=0;
	char *p;
	char c;
	
	p = buf;
	while(read(0, &c, 1) > 0)
	{
		if (c == '\n')
		{	
			*p = '\0';
			break;
		}
		*p++ = c;
		len++;
	}
	return len;
}


void add_args(char *argv[], int old_argc, char *buf, int line_len)
{
	int line_idx = 0;
	int argv_idx = old_argc;	
	int prev_space = 1;
	for(line_idx = 0; line_idx < line_len; line_idx++)
	{
		if (buf[line_idx] == ' ')
		{
			buf[line_idx] = '\0';
			if ( prev_space == 0)
				argv_idx++;
			prev_space = 1;
		}
		else{
			if(prev_space == 1)
				argv[argv_idx] = &buf[line_idx];
			prev_space = 0;
		}
	}
}
