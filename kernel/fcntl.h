#define O_RDONLY  0x000
#define O_WRONLY  0x001
#define O_RDWR    0x002
#define O_CREATE  0x200
#define O_TRUNC   0x400

#define O_NOFOLLOW  0x010   //symbolic links修改8：添加一个open文件时的控制flag，
                            //表示要处理这个符号链接文件本身，而不是符号链接指向的文件
