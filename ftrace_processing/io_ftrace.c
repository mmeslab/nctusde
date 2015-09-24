#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>



#define _STR(x) #x
#define STR(x) _STR(x)
#define MAX_PATH 256

const char *find_debugfs(void)
{
       static char debugfs[MAX_PATH+1];
       static int debugfs_found;
       char type[100];
       FILE *fp;

       if (debugfs_found)
               return debugfs;

       if ((fp = fopen("/proc/mounts","r")) == NULL) {
               perror("/proc/mounts");
               return NULL;
       }

       while (fscanf(fp, "%*s %"
                     STR(MAX_PATH)
                     "s %99s %*s %*d %*d\n",
                     debugfs, type) == 2) {
               if (strcmp(type, "debugfs") == 0)
                       break;
       }
       fclose(fp);

       if (strcmp(type, "debugfs") != 0) {
               fprintf(stderr, "debugfs not mounted");
               return NULL;
       }

       strcat(debugfs, "/tracing/");
       debugfs_found = 1;

       return debugfs;
}

const char *tracing_file(const char *file_name)
{
       static char trace_file[MAX_PATH+1];
       snprintf(trace_file, MAX_PATH, "%s/%s", find_debugfs(), file_name);
       return trace_file;
}

int trace_fd;

void trace_write(const char *fmt, ...)
{
	va_list ap;
	char buf[256];
	int n;

	if (trace_fd < 0)
		return;

	va_start(ap, fmt);
	n = vsnprintf(buf, 256, fmt, ap);
	va_end(ap);

	write(trace_fd, buf, n);
}


int main (int argc, char **argv)
{

        if (argc < 1)
                exit(-1);

        if (fork() > 0) {
                int fd, ffd, fd_trace, fd_device;
                char line[64];
				char buffer[512];
                int s;

				// open trace marker
				trace_fd = open(tracing_file("trace_marker"), O_WRONLY | O_SYNC);

				// clear the current trace
				fd_trace = open(tracing_file("trace"), O_WRONLY);
				if(fd_trace < 0)
					exit(-1);
				write(fd_trace, "0", 1);
				close(fd_trace);

				// open the block device for reading test
				fd_device = open("/dev/sda", O_RDONLY | O_SYNC);
				

                ffd = open(tracing_file("current_tracer"), O_WRONLY);
                if (ffd < 0)
                        exit(-1);
                write(ffd, "nop", 3);

                fd = open(tracing_file("set_ftrace_pid"), O_WRONLY);
                s = sprintf(line, "%d\n", getpid());
                write(fd, line, s);

                write(ffd, "function_graph", 8+6);
                //write(ffd, "function", 8);

                close(fd);
                close(ffd);

				trace_write("nctuss----1\n");
				read(fd_device, buffer, 1);
				trace_write("nctuss----2\n");
				//read(fd_device, buffer, 1);

				close(fd_device);
				//printf("hello world\n");
                //execvp(argv[1], argv+1);
                
        }

        return 0;
}
