/* userfaultfd_demo.c

   Licensed under the GNU General Public License version 2 or later.
*/
#define _GNU_SOURCE
#include <sys/types.h>
#include <stdio.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <poll.h>

#define errExit(msg)    do { perror(msg); exit(EXIT_FAILURE);	\
	} while (0)

static int page_size;

static void *
fault_handler_thread(void *arg)
{
	static struct uffd_msg msg;   /* Data read from userfaultfd */
	static int fault_cnt = 0;     /* Number of faults so far handled */
	long uffd;                    /* userfaultfd file descriptor */
	static char *page = NULL;
	struct uffdio_copy uffdio_copy;
	ssize_t nread;

	uffd = (long) arg;

	/* [H1: point 1]
	 * Create a mapping of virtual memory for the process at
   * an address chosen by the kernel that has read and write permissions
   * and is visible to other processes.
	 */
	if (page == NULL) {
		page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (page == MAP_FAILED)
			errExit("mmap");
	}

	/* [H2: point 1]
	 * An unconditional for loop, essentially a while(1) loop
	 */
	for (;;) {

		/* See what poll() tells us about the userfaultfd */

		struct pollfd pollfd;
		int nready;

		/* [H3: point 1]
		 * Wait for a page fault event, read the result, then print to console
		 */
		pollfd.fd = uffd;
		pollfd.events = POLLIN;
		nready = poll(&pollfd, 1, -1);
		if (nready == -1)
			errExit("poll");

		printf("\nfault_handler_thread():\n");
		printf("    poll() returns: nready = %d; "
                       "POLLIN = %d; POLLERR = %d\n", nready,
                       (pollfd.revents & POLLIN) != 0,
                       (pollfd.revents & POLLERR) != 0);

		/* [H4: point 1]
		 * Read from the user fault page 
		 */
		nread = read(uffd, &msg, sizeof(msg));
		if (nread == 0) {
			printf("EOF on userfaultfd!\n");
			exit(EXIT_FAILURE);
		}

		if (nread == -1)
			errExit("read");

		/* [H5: point 1]
		 * Check for a page fault event and exit if one does not occur
		 */
		if (msg.event != UFFD_EVENT_PAGEFAULT) {
			fprintf(stderr, "Unexpected event on userfaultfd\n");
			exit(EXIT_FAILURE);
		}

		/* [H6: point 1]
		 * Output what kind of event it was and where it happened
		 */
		printf("    UFFD_EVENT_PAGEFAULT event: ");
		printf("flags = %llx; ", msg.arg.pagefault.flags);
		printf("address = %llx\n", msg.arg.pagefault.address);

		/* [H7: point 1]
		 * Fill the page with characters based on the number of faults and increment faultss
		 */
		memset(page, 'A' + fault_cnt % 20, page_size);
		fault_cnt++;

		/* [H8: point 1]
		 * fill the uffdio struct using the page parameters set above
		 */
		uffdio_copy.src = (unsigned long) page;
		uffdio_copy.dst = (unsigned long) msg.arg.pagefault.address &
			~(page_size - 1);
		uffdio_copy.len = page_size;
		uffdio_copy.mode = 0;
		uffdio_copy.copy = 0;

		/* [H9: point 1]
		 * Request to continuously copy the the page initialized above into the user fault range
		 */
		if (ioctl(uffd, UFFDIO_COPY, &uffdio_copy) == -1)
			errExit("ioctl-UFFDIO_COPY");

		/* [H10: point 1]
		 * Print the copied memory to console
		 */
		printf("        (uffdio_copy.copy returned %lld)\n",
                       uffdio_copy.copy);
	}
}

int
main(int argc, char *argv[])
{
	long uffd;          /* userfaultfd file descriptor */
	char *addr;         /* Start of region handled by userfaultfd */
	unsigned long len;  /* Length of region handled by userfaultfd */
	pthread_t thr;      /* ID of thread that handles page faults */
	struct uffdio_api uffdio_api;
	struct uffdio_register uffdio_register;
	int s;
	int l;

	/* [M1: point 1]
	 * Print the number of pages requested be copied to the console
	 */
	if (argc != 2) {
		fprintf(stderr, "Usage: %s num-pages\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	/* [M2: point 1]
	 * Get the size of a page and then the total size of the memory to be copied
	 */
	page_size = sysconf(_SC_PAGE_SIZE);
	len = strtoul(argv[1], NULL, 0) * page_size;

	/* [M3: point 1]
	 *   Init the userfault API with exec permissions and nonblocking status
	 */
	uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
	if (uffd == -1)
		errExit("userfaultfd");

	/* [M4: point 1]
	 * Enable userfault fd operation and perform api handshake
	 */
	uffdio_api.api = UFFD_API;
	uffdio_api.features = 0;
	if (ioctl(uffd, UFFDIO_API, &uffdio_api) == -1)
		errExit("ioctl-UFFDIO_API");

	/* [M5: point 1]
	 * Map the virtual memory address chosen by the kernel with the 
   * specified length to be visible with read and write permissions
   * to other processes
	 */
	addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (addr == MAP_FAILED)
		errExit("mmap");

	printf("Address returned by mmap() = %p\n", addr);

	/* [M6: point 1]
	 * Fill the uffd register using the mmap parameters above
	 */
	uffdio_register.range.start = (unsigned long) addr;
	uffdio_register.range.len = len;
	uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING;
	if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1)
		errExit("ioctl-UFFDIO_REGISTER");

	/* [M7: point 1]
	 * Create a thread to run the fault handler with the initialized uffd descriptor
	 */
	s = pthread_create(&thr, NULL, fault_handler_thread, (void *) uffd);
	if (s != 0) {
		errno = s;
		errExit("pthread_create");
	}

	/*
	 * [U1]: point 5
	 * The output will be all A's
	 */
	printf("-----------------------------------------------------\n");
	l = 0x0;
	while (l < len) {
		char c = addr[l];
		printf("#1. Read address %p in main(): ", addr + l);
		printf("%c\n", c);
		l += 1024;
	}

	/*
	 * [U2]: point 5
	 * The output will still be all A's due to the page size
	 */
	printf("-----------------------------------------------------\n");
	l = 0x0;
	while (l < len) {
		char c = addr[l];
		printf("#2. Read address %p in main(): ", addr + l);
		printf("%c\n", c);
		l += 1024;
	}

	/*
	 * [U3]: point 5
	 * The output will be now be all B's because the kernel begins to overwrite the data
	 */
	printf("-----------------------------------------------------\n");
	if (madvise(addr, len, MADV_DONTNEED)) {
		errExit("fail to madvise");
	}
	l = 0x0;
	while (l < len) {
		char c = addr[l];
		printf("#3. Read address %p in main(): ", addr + l);
		printf("%c\n", c);
		l += 1024;
	}

	/*
	 * [U4]: point 5
	 * The output below will be still be all B's
	 */
	printf("-----------------------------------------------------\n");
	l = 0x0;
	while (l < len) {
		char c = addr[l];
		printf("#4. Read address %p in main(): ", addr + l);
		printf("%c\n", c);
		l += 1024;
	}

	/*
	 * [U5]: point 5
	 * The output below wil be all @'s
	 */
	printf("-----------------------------------------------------\n");
	if (madvise(addr, len, MADV_DONTNEED)) {
		errExit("fail to madvise");
	}
	l = 0x0;
	while (l < len) {
		memset(addr+l, '@', 1024);
		printf("#5. write address %p in main(): ", addr + l);
		printf("%c\n", addr[l]);
		l += 1024;
	}

	/*
	 * [U6]: point 5
	 * The output below will still be all @'s
	 */
	printf("-----------------------------------------------------\n");
	l = 0x0;
	while (l < len) {
		char c = addr[l];
		printf("#6. Read address %p in main(): ", addr + l);
		printf("%c\n", c);
		l += 1024;
	}

	/*
	 * [U7]: point 5
	 * THe output below will be all ^'s
	 */
	printf("-----------------------------------------------------\n");
	l = 0x0;
	while (l < len) {
		memset(addr+l, '^', 1024);
		printf("#7. write address %p in main(): ", addr + l);
		printf("%c\n", addr[l]);
		l += 1024;
	}

	/*
	 * [U8]: point 5
	 * The output below will still be all ^'s because no faults will have occured
	 */
	printf("-----------------------------------------------------\n");
	l = 0x0;
	while (l < len) {
		char c = addr[l];
		printf("#8. Read address %p in main(): ", addr + l);
		printf("%c\n", c);
		l += 1024;
	}

	exit(EXIT_SUCCESS);
}
