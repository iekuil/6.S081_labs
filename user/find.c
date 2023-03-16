#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

void find(char *dir, char *filename);
int fname_cmp(char *path, char *filename);

int main(int argc, char *argv[])
{
	if (argc < 3)
	{
		exit(0);
	}
	find(argv[1], argv[2]);
	exit(0);
}

void find(char *path, char *filename)
{
	char buf[512], *p;
	int fd;
	struct dirent de;
	struct stat st;
	
	strcpy(buf, path);
	p = buf + strlen(buf);
	//printf("the path received by callee: _%s_\n", buf);

	if ((fd = open(buf, 0)) < 0)
	{
		fprintf(2, "find: cannot open %s, fd=%d\n", path, fd);
		return;
	}

	if (fstat(fd, &st) < 0)
	{
		fprintf(2, "find: cannot stat %s\n", path);
		close(fd);
		return;
	}
	
	if (st.type == T_FILE)
	{
		if ( !fname_cmp(path, filename) )
		{
			printf("%s\n", path);
		}	
		close(fd);
		return;
	}

	if (st.type == T_DIR)
	{
		if (*(p-1) != '/')
			*p++ = '/';
		if (strlen(path) + DIRSIZ + 1 > sizeof(buf))
		{
			printf("find: path too long\n");
			close(fd);
			return;
		}	
		while ( read(fd, &de, sizeof(de)) == sizeof(de) )
		{
			if (de.inum == 0)
				continue;
			if ( !strcmp(de.name, ".") )
				continue;
			if ( !strcmp(de.name, ".."))
				continue;
			strcpy(p, de.name);
			//printf("\nthe path passed by caller: _%s_\n", buf);
			//p[strlen(de.name)] = '\0';
			find(buf, filename);
		}
	}
	close(fd);
	return;
}


int fname_cmp(char *path, char *filename)
{
	char *p;
	
	for ( p=path+strlen(path); p>=path && *p!='/'; p--)
		;
	p++;
	return strcmp(p, filename);
}
